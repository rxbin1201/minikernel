/* Bluetooth, Stufe 5: Musik auf eine A2DP-Senke (Soundbar, Kopfhoerer) - Thread "a2dp".
 *
 * Steht die Verbindung (btconn.c), richtet der Thread den Strom ein: SBC 48 kHz Stereo, AVDTP Open und Medienkanal.
 * Ab dann mischt hda.c alle Stimmen in den Ring hier statt auf die Soundkarte (im Takt der Uhr, etwas voraus). Kommen
 * Samples, startet der Thread den Strom (AVDTP Start), kodiert je 128 Samples zu einem SBC-Frame und schickt so viele
 * Frames, wie in ein Paket passen, als RTP-Paket (Kopf 12 Byte, Zahl der Frames, Frames). Zwei Sekunden ohne Ton:
 * AVDTP Suspend - die Soundbar darf schlafen, und beim naechsten Ton geht es mit Start weiter. */

#include "drivers/bt/bt_internal.h"
#include "drivers/bt/sbc.h"
#include "arch/x86_64/apic.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"

#define RING      32768 /* Frames (48 kHz Stereo), gut 0,6 s */
#define IDLE_MS   2000
#define RTP_HDR   12

static int16_t          *ring;
static volatile uint64_t wr, rd;
static volatile int      state; /* 0 aus, 1 eingerichtet (pausiert), 2 spielt */
static Event             ev;
static int               started;

int bt_a2dp_active(void)
{
    return state >= 1 && bt_conn_ready();
}

void bt_a2dp_underrun(void)
{
    bt_conn_state()->a2dp_underruns++;
}

uint32_t bt_a2dp_room(void)
{
    return bt_a2dp_active() ? (uint32_t)(RING - (wr - rd)) : 0;
}

uint32_t bt_a2dp_write(const int16_t *s, uint32_t n)
{
    if (!bt_a2dp_active())
        return 0;
    uint32_t room = (uint32_t)(RING - (wr - rd));
    if (n > room) {
        bt_conn_state()->a2dp_dropped += n - room;
        n = room;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (uint32_t)((wr + i) % RING) * 2;
        ring[k] = s[i * 2];
        ring[k + 1] = s[i * 2 + 1];
    }
    __sync_synchronize();
    wr += n;
    event_signal(&ev);
    return n;
}

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void a2dp_thread(void *arg)
{
    (void)arg;
    static SbcEnc  enc;
    static uint8_t pkt[1100];
    static int16_t pcm[SBCENC_SAMPLES * 2];
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint64_t last_data = 0, retry_at = 0;
    int fails = 0;
    for (;;) {
        event_wait(&ev, state == 2 ? 10 : 100);
        BtConn *c = bt_conn_state();
        if (!bt_conn_ready()) {
            if (state)
                kprintf("bt: A2DP aus (Verbindung weg)\n");
            state = 0;
            fails = 0;
            retry_at = 0;
            rd = wr;
            continue;
        }
        uint64_t now = time_ms();
        if (state == 0) { /* Strom einrichten (einmal je Verbindung, hoechstens drei Versuche) */
            if (fails >= 3 || now < retry_at)
                continue;
            mutex_lock(&bt_lock);
            int r = bt_av_setup();
            mutex_unlock(&bt_lock);
            if (r == 0) {
                sbcenc_init(&enc, SBCENC_FREQ_48000, c->a2dp_bitpool);
                rd = wr;
                state = 1;
            } else {
                fails++;
                retry_at = now + 3000;
            }
            c->a2dp_state = (uint8_t)state;
            continue;
        }
        if (state == 1 && wr - rd >= SBCENC_SAMPLES) { /* Ton da: Strom starten */
            mutex_lock(&bt_lock);
            int r = bt_av_start(1);
            mutex_unlock(&bt_lock);
            if (r == 0) {
                state = 2;
                last_data = now;
            } else {
                c->a2dp_errors++;
                rd = wr; /* verwerfen; beim naechsten Ton neu versuchen */
            }
        }
        if (state == 2) {
            uint32_t flen = sbcenc_frame_len(&enc), mtu = c->media_mtu ? c->media_mtu : 672;
            if (mtu > sizeof(pkt))
                mtu = sizeof(pkt);
            uint32_t per = (mtu - RTP_HDR - 1) / flen;
            if (per > 15)
                per = 15;
            if (per < 1)
                per = 1;
            while (wr - rd >= per * SBCENC_SAMPLES && bt_conn_ready()) {
                pkt[0] = 0x80; /* RTP Version 2 */
                pkt[1] = 96;   /* dynamischer Nutzlasttyp */
                pkt[2] = (uint8_t)(seq >> 8);
                pkt[3] = (uint8_t)seq;
                put32be(pkt + 4, ts);
                put32be(pkt + 8, 1); /* SSRC */
                pkt[12] = (uint8_t)per; /* Zahl der SBC-Frames */
                uint32_t off = RTP_HDR + 1;
                for (uint32_t k = 0; k < per; k++) {
                    for (uint32_t i = 0; i < SBCENC_SAMPLES; i++) {
                        uint32_t idx = (uint32_t)((rd + i) % RING) * 2;
                        pcm[i * 2] = ring[idx];
                        pcm[i * 2 + 1] = ring[idx + 1];
                    }
                    rd += SBCENC_SAMPLES;
                    off += sbcenc_encode(&enc, pcm, pkt + off);
                }
                uint64_t ts0 = time_us();
                if (bt_media_send(pkt, off) == 0)
                    c->a2dp_packets++;
                else
                    c->a2dp_errors++;
                uint32_t waited = (uint32_t)((time_us() - ts0) / 1000); /* meist: auf freie ACL-Puffer gewartet */
                if (waited > 30)
                    c->a2dp_stalls++;
                if (waited > c->a2dp_max_wait_ms)
                    c->a2dp_max_wait_ms = waited;
                seq++;
                ts += per * SBCENC_SAMPLES;
                last_data = now;
            }
            if (wr - rd < SBCENC_SAMPLES && time_ms() - last_data > IDLE_MS) { /* Stille: pausieren */
                mutex_lock(&bt_lock);
                bt_av_start(0);
                mutex_unlock(&bt_lock);
                state = 1;
            }
        }
        c->a2dp_state = (uint8_t)state;
    }
}

void bt_a2dp_init(void)
{
    if (started)
        return;
    if (!ring && !(ring = (int16_t *)pmm_alloc_frames(RING * 4 / 4096)))
        return;
    started = 1;
    thread_create("a2dp", a2dp_thread, 0);
}
