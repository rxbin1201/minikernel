/* Bluetooth, Stufe 4: Verbindung zu einem klassischen Geraet (z.B. einer Soundbar) - ACL, Koppeln, Verschluesseln,
 * L2CAP-Kanal zum Audio-Dienst (AVDTP) und dessen Endpunkte (Codecs) auslesen.
 *
 * ACL: Pakete (Handle | Paketgrenze << 12, Laenge, Daten) ueber Bulk-OUT/-IN. Was der Controller annimmt, begrenzen
 * seine Puffer (Read Buffer Size); jedes gesendete Paket kostet einen Platz, Number Of Completed Packets gibt sie
 * zurueck. Empfangene Pakete werden zu L2CAP-PDUs (Laenge, Kanal, Daten) zusammengesetzt.
 *
 * Koppeln: Authentication Requested; der Controller fragt dann nach einem gespeicherten Schluessel (Link Key Request)
 * und sonst nach unseren Faehigkeiten (IO Capability: keine Ein-/Ausgabe -> "Just Works", wie bei Soundbars), laesst
 * den Vergleichswert bestaetigen (User Confirmation) und meldet den neuen Schluessel (Link Key Notification). Alte
 * Geraete ohne Secure Simple Pairing fragen nach einer PIN: wir antworten "0000". Danach Verschluesselung.
 *
 * L2CAP: Signalkanal 1 - Verbindung zum PSM 0x19 (AVDTP), Konfiguration in beide Richtungen (MTU). Anfragen der
 * Gegenstelle (Information, Echo, eigene Kanaele) beantwortet der Thread "bt".
 * AVDTP: Discover nennt die Endpunkte (Quelle/Senke, Medienart), Get Capabilities deren Codecs. Fragt die Soundbar
 * selbst nach unseren Endpunkten, nennen wir eine Audio-Quelle mit SBC.
 *
 * Ereignisse und Daten kommen im Kontext des USB-Controllers an: dort werden nur Zustaende gesetzt; was eine Antwort
 * (einen Befehl) braucht, erledigt der Thread "bt". */

#include "drivers/bt/bt_internal.h"
#include "drivers/usb/usb.h"
#include "drivers/sound/hda.h"
#include "arch/x86_64/apic.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include <stdarg.h>

#define CID_SIG     0x0001
#define PSM_AVDTP   0x0019
#define LOCAL_CID   0x0040 /* unser Ende des AVDTP-Signalkanals */
#define MEDIA_CID   0x0041 /* unser Ende des AVDTP-Medienkanals (Audiodaten) */
#define AVRCP_CID   0x0042 /* unser Ende des AVRCP-Kanals (Fernbedienung, AVCTP) */
#define PSM_AVRCP   0x0017
#define L2_MTU      1024
#define NO_HANDLE   0xFFFF

_Static_assert(sizeof(BtConn) == 304 && sizeof(BtKey) == 24, "BtConn/BtKey wie in user.h");

static BtConn            st;
static uint8_t           peer[6];             /* Gegenstelle, wie im HCI (niedrigstes Byte zuerst) */
static volatile uint16_t handle = NO_HANDLE;
static volatile int      conn_done, auth_done, enc_done, disc_done, remote_cfg_done, chan_closed, media_cfg_done;
/* AVDTP-Kanaele, die die Gegenstelle oeffnet (Soundbars tun das oft selbst): unsere Konfigurationsanfrage dazu
 * (Kennung, Ergebnis: 0 offen, 1 angenommen, -1 abgelehnt); *_connecting: eigene Anfrage laeuft gerade */
static volatile int      sig_connecting, media_connecting, sig_our_cfg, media_our_cfg;
static uint8_t           sig_cfg_id, media_cfg_id;
static uint16_t          avrcp_remote;   /* Kanal der Gegenstelle fuer AVRCP, 0 = keiner */
/* Tasten der Fernbedienung fuer Programme (music): Play, Pause, Stop, vor, zurueck */
static uint8_t           mkeys[16];
static volatile uint32_t mkey_head, mkey_tail;
static volatile int      credits;             /* ACL-Pakete, die der Controller noch annimmt */
static uint8_t          *acl_dma;
static Mutex             acl_lock = MUTEX_INIT;

/* Ereignisse, die eine Antwort brauchen (fuer den Thread "bt") */
#define EVQ 16
static struct {
    uint8_t code, len;
    uint8_t p[255];
} evq[EVQ];
static volatile uint32_t evq_head, evq_tail;
static Event             worker_ev;
static int               worker_started;

/* ACL-Strom vom Bulk-IN, eine L2CAP-PDU im Aufbau, fertige PDUs fuer den Thread */
static uint8_t  acl_as[4096];
static uint32_t acl_as_len;
static uint8_t  l2_buf[L2_MTU + 64];
static uint32_t l2_len, l2_need;
#define RXQ 8
static struct {
    uint16_t cid, len;
    uint8_t  d[L2_MTU + 8];
} rxq[RXQ];
static volatile uint32_t rxq_head, rxq_tail;

/* Verbindungsschluessel (niedrigstes Byte zuerst wie im HCI) */
#define MAX_KEYS 8
static struct {
    int     used;
    uint8_t addr[6], key[16], type;
} keys[MAX_KEYS];

/* Warten auf eine L2CAP-Signalantwort (nach Kennung) bzw. eine AVDTP-Antwort (nach Transaktionsnummer) */
static struct {
    volatile int got;
    int          active;
    uint8_t      id, code;
    uint16_t     len;
    uint8_t      d[64];
} l2w;
static uint8_t sig_id = 1;
static struct {
    volatile int got;
    int          active;
    uint8_t      label, msg_type, sig;
    uint32_t     len;
    uint8_t      d[300];
} avw;
static uint8_t av_label;

static void abs_reset(void);
static volatile uint64_t avrcp_cfg_ms;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(st.msg, sizeof(st.msg), fmt, ap);
    va_end(ap);
    kprintf("bt: %s\n", st.msg);
}

static void rev6(uint8_t *dst, const uint8_t *src)
{
    for (int i = 0; i < 6; i++)
        dst[i] = src[5 - i];
}

static int key_find(const uint8_t *addr_le)
{
    for (int i = 0; i < MAX_KEYS; i++)
        if (keys[i].used && memcmp(keys[i].addr, addr_le, 6) == 0)
            return i;
    return -1;
}

static void key_store(const uint8_t *addr_le, const uint8_t *key, uint8_t type)
{
    int i = key_find(addr_le);
    for (int k = 0; i < 0 && k < MAX_KEYS; k++)
        if (!keys[k].used)
            i = k;
    if (i < 0)
        i = 0; /* voll: den ersten ersetzen */
    keys[i].used = 1;
    memcpy(keys[i].addr, addr_le, 6);
    memcpy(keys[i].key, key, 16);
    keys[i].type = type;
}

/* ---------- Ereignisse (Controller-Kontext) ---------- */

static void queue_event(uint8_t code, const uint8_t *p, uint32_t plen)
{
    uint32_t next = (evq_head + 1) % EVQ;
    if (next == evq_tail)
        return;
    evq[evq_head].code = code;
    evq[evq_head].len = (uint8_t)plen;
    memcpy(evq[evq_head].p, p, plen);
    __sync_synchronize();
    evq_head = next;
    event_signal(&worker_ev);
}

int bt_conn_event(uint8_t code, const uint8_t *p, uint32_t plen)
{
    switch (code) {
    case 0x03: /* Connection Complete: Status, Handle, Adresse, Art, Verschluesselung */
        if (plen >= 11 && memcmp(p + 3, peer, 6) == 0 && !conn_done) {
            st.conn_status = p[0];
            if (!p[0])
                handle = le16(p + 1) & 0x0FFF;
            __sync_synchronize();
            conn_done = 1;
        } else if (plen >= 11) {
            kprintf("bt: unerwartete Verbindung (Status %#x, Handle %#x)\n", p[0], le16(p + 1));
        }
        return 1;
    case 0x05: /* Disconnection Complete: Status, Handle, Grund */
        if (plen >= 4 && (le16(p + 1) & 0x0FFF) == handle) {
            st.disc_reason = p[3];
            handle = NO_HANDLE;
            disc_done = 1;
            if (st.state == BT_CONN_READY) {
                st.state = BT_CONN_IDLE;
                say("Verbindung getrennt (Grund %#x)", p[3]);
            }
        }
        return 1;
    case 0x06: /* Authentication Complete: Status, Handle */
        if (plen >= 3) {
            st.auth_status = p[0];
            auth_done = 1;
        }
        return 1;
    case 0x08: /* Encryption Change: Status, Handle, an/aus */
        if (plen >= 4) {
            st.enc_status = p[0];
            st.encrypted = p[0] == 0 && p[3] != 0;
            enc_done = 1;
        }
        return 1;
    case 0x13: /* Number Of Completed Packets: Zahl, je Handle und Anzahl */
        if (plen >= 1)
            for (uint32_t i = 0; i < p[0] && 1 + i * 4 + 4 <= plen; i++)
                if ((le16(p + 1 + i * 4) & 0x0FFF) == handle)
                    __atomic_add_fetch(&credits, le16(p + 3 + i * 4), __ATOMIC_RELAXED);
        return 1;
    case 0x18: /* Link Key Notification: Adresse, Schluessel, Art */
        if (plen >= 23) {
            key_store(p, p + 6, p[22]);
            st.key_type = p[22];
            st.paired_new = 1;
            kprintf("bt: neuer Verbindungsschluessel (Art %u)\n", p[22]);
        }
        return 1;
    case 0x04: /* Connection Request */
    case 0x16: /* PIN Code Request */
    case 0x17: /* Link Key Request */
    case 0x31: /* IO Capability Request */
    case 0x32: /* IO Capability Response (die der Gegenstelle) */
    case 0x33: /* User Confirmation Request */
    case 0x34: /* User Passkey Request */
    case 0x36: /* Simple Pairing Complete */
        queue_event(code, p, plen);
        return 1;
    case 0x07: case 0x0B: case 0x0C: case 0x14: case 0x1B: case 0x20: case 0x23: case 0x3D: case 0x30:
        return 1; /* Name, Faehigkeiten, Modus, Slots usw.: nicht gebraucht */
    }
    return 0;
}

/* ---------- ACL-Empfang (Controller-Kontext) ---------- */

static void deliver(uint16_t cid, const uint8_t *d, uint32_t len)
{
    st.l2_rx++;
    uint32_t next = (rxq_head + 1) % RXQ;
    if (next == rxq_tail || len > L2_MTU) {
        kprintf("bt: L2CAP-PDU verworfen (Kanal %#x, %u Byte)\n", cid, len);
        return;
    }
    rxq[rxq_head].cid = cid;
    rxq[rxq_head].len = (uint16_t)len;
    memcpy(rxq[rxq_head].d, d, len);
    __sync_synchronize();
    rxq_head = next;
    event_signal(&worker_ev);
}

static void acl_packet(uint16_t h, const uint8_t *d, uint32_t len)
{
    st.acl_rx++;
    if ((h & 0x0FFF) != handle)
        return;
    int pb = (h >> 12) & 3;
    if (pb != 1) { /* Anfang einer L2CAP-PDU */
        if (len < 4)
            return;
        l2_need = le16(d) + 4u;
        l2_len = 0;
    } else if (!l2_need) {
        return;
    }
    if (l2_len + len > sizeof(l2_buf)) {
        l2_need = 0;
        return;
    }
    memcpy(l2_buf + l2_len, d, len);
    l2_len += len;
    if (l2_len >= l2_need) {
        deliver(le16(l2_buf + 2), l2_buf + 4, l2_need - 4);
        l2_need = 0;
    }
}

void bt_conn_acl(const uint8_t *buf, uint32_t len)
{
    if (acl_as_len + len > sizeof(acl_as))
        acl_as_len = 0; /* aus dem Tritt */
    memcpy(acl_as + acl_as_len, buf, len);
    acl_as_len += len;
    while (acl_as_len >= 4) {
        uint32_t dl = le16(acl_as + 2);
        if (acl_as_len < 4 + dl)
            break;
        acl_packet(le16(acl_as), acl_as + 4, dl);
        memmove(acl_as, acl_as + 4 + dl, acl_as_len - 4 - dl);
        acl_as_len -= 4 + dl;
    }
}

/* ---------- ACL senden ---------- */

static int l2_send(uint16_t cid, const uint8_t *d, uint32_t len)
{
    static uint8_t pdu[L2_MTU + 8];
    BtInfo *bi = bt_state();
    struct UsbDevice *ud = bt_usb_dev();
    if (handle == NO_HANDLE || len > L2_MTU || !ud)
        return -1;
    mutex_lock(&acl_lock);
    put16(pdu, (uint16_t)len);
    put16(pdu + 2, cid);
    memcpy(pdu + 4, d, len);
    uint32_t total = len + 4, off = 0, mtu = bi->acl_mtu ? bi->acl_mtu : 310;
    int r = 0;
    while (off < total) {
        uint32_t k = total - off < mtu ? total - off : mtu;
        /* Controller voll: schlafend auf Number Of Completed Packets warten (gibt den BKL frei) */
        if (__atomic_load_n(&credits, __ATOMIC_RELAXED) <= 0 && (wait_events(&credits, 2000) != 0 || handle == NO_HANDLE)) {
            r = -2;
            goto out;
        }
        __atomic_sub_fetch(&credits, 1, __ATOMIC_RELAXED);
        put16(acl_dma, (uint16_t)(handle | (off ? 0x1 : 0x2) << 12));
        put16(acl_dma + 2, (uint16_t)k);
        memcpy(acl_dma + 4, pdu + off, k);
        usb_lock(ud);
        int n = usb_bulk(ud, bi->ep_bulk_out, acl_dma, 4 + k, 1000);
        usb_unlock(ud);
        if (n < 0) {
            kprintf("bt: ACL senden fehlgeschlagen (%d)\n", n);
            r = -3;
            goto out;
        }
        st.acl_tx++;
        off += k;
    }
out:
    mutex_unlock(&acl_lock);
    return r;
}

/* ---------- L2CAP-Signalkanal ---------- */

static int sig_send(uint8_t code, uint8_t id, const void *data, uint16_t len)
{
    uint8_t b[80];
    b[0] = code;
    b[1] = id;
    put16(b + 2, len);
    memcpy(b + 4, data, len);
    return l2_send(CID_SIG, b, 4u + len);
}

/* Anfrage senden und auf die Antwort mit derselben Kennung warten: Laenge der Antwortdaten, <0 Fehler */
static int sig_request(uint8_t code, const void *data, uint16_t len, uint32_t ms, uint8_t *rcode, uint8_t *resp)
{
    uint8_t id = sig_id++;
    if (!sig_id)
        sig_id = 1;
    l2w.got = 0;
    l2w.id = id;
    l2w.active = 1;
    if (sig_send(code, id, data, len) != 0) {
        l2w.active = 0;
        return -3;
    }
    int r = wait_events(&l2w.got, ms);
    l2w.active = 0;
    if (r < 0)
        return r;
    *rcode = l2w.code;
    memcpy(resp, l2w.d, l2w.len);
    return l2w.len;
}

static void sig_rx(const uint8_t *d, uint32_t len)
{
    for (uint32_t o = 0; o + 4 <= len;) {
        uint8_t code = d[o], id = d[o + 1];
        uint16_t cl = le16(d + o + 2);
        const uint8_t *v = d + o + 4;
        if (o + 4 + cl > len)
            break;
        uint8_t rsp[16];
        switch (code) {
        case 0x01: case 0x03: case 0x05: case 0x07: case 0x0B: /* Antworten */
            if (code == 0x05 && cl >= 6 && id && (id == sig_cfg_id || id == media_cfg_id)) { /* zu unseren Kanaelen */
                int ok = le16(v + 4) == 0 ? 1 : -1;
                if (id == sig_cfg_id)
                    sig_our_cfg = ok;
                else
                    media_our_cfg = ok;
                if (ok < 0)
                    kprintf("bt: L2CAP: Konfiguration abgelehnt (Ergebnis %#x)\n", le16(v + 4));
                break;
            }
            if (code == 0x03 && cl >= 8 && le16(v + 4) == 0) { /* Kanal der Gegenstelle gleich merken: ihre
                                                                 * Konfiguration kann im selben Paket folgen */
                if (le16(v + 2) == LOCAL_CID)
                    st.l2_remote_cid = le16(v);
                else if (le16(v + 2) == MEDIA_CID)
                    st.media_remote_cid = le16(v);
            }
            if (code == 0x03 && cl >= 8 && le16(v + 4) == 1) { /* Verbindung "wird bearbeitet": weiter warten */
                kprintf("bt: L2CAP: Verbindung wird bearbeitet (Status %u)\n", le16(v + 6));
                break;
            }
            if (l2w.active && !l2w.got && id == l2w.id) {
                l2w.code = code;
                l2w.len = cl < sizeof(l2w.d) ? cl : sizeof(l2w.d);
                memcpy(l2w.d, v, l2w.len);
                __sync_synchronize();
                l2w.got = 1;
            }
            break;
        case 0x02: /* Connection Request der Gegenstelle: AVDTP annehmen (erst Signal-, dann Medienkanal), sonst ab */
            if (cl >= 4) {
                uint16_t psm = le16(v), scid = le16(v + 2), local = 0, result = 0x0002; /* PSM nicht unterstuetzt */
                const char *what = "abgelehnt";
                if (psm == PSM_AVRCP && handle != NO_HANDLE && !avrcp_remote) { /* Fernbedienung der Soundbar */
                    local = AVRCP_CID;
                    abs_reset();
                    avrcp_remote = scid;
                    what = "angenommen als AVRCP (Fernbedienung)";
                } else if (psm == PSM_AVDTP && handle != NO_HANDLE) {
                    if (!st.l2_remote_cid && !sig_connecting) {
                        local = LOCAL_CID;
                        st.l2_remote_cid = scid;
                        what = "angenommen als Signalkanal";
                    } else if (st.l2_remote_cid && !st.media_remote_cid && !media_connecting) {
                        local = MEDIA_CID;
                        st.media_remote_cid = scid;
                        what = "angenommen als Medienkanal";
                    } else {
                        result = 0x0004; /* keine Mittel frei: unsere eigene Anfrage laeuft */
                        what = "abgelehnt (eigene Anfrage laeuft)";
                    }
                }
                if (local)
                    result = 0;
                kprintf("bt: L2CAP: Gegenstelle oeffnet PSM %#x (ihr Kanal %#x) - %s\n", psm, scid, what);
                put16(rsp, local);
                put16(rsp + 2, scid);
                put16(rsp + 4, result);
                put16(rsp + 6, 0);
                sig_send(0x03, id, rsp, 8);
                if (local) { /* unsere Seite konfigurieren (MTU); die Antwort kommt hier wieder an */
                    uint8_t cfg[8];
                    uint8_t cid = sig_id++;
                    if (!sig_id)
                        sig_id = 1;
                    if (local == LOCAL_CID) {
                        sig_our_cfg = 0;
                        sig_cfg_id = cid;
                    } else if (local == MEDIA_CID) {
                        media_our_cfg = 0;
                        media_cfg_id = cid;
                    } /* AVRCP: die Antwort auf unsere Konfiguration wird nicht gebraucht */
                    put16(cfg, scid);
                    put16(cfg + 2, 0);
                    cfg[4] = 0x01;
                    cfg[5] = 2;
                    put16(cfg + 6, L2_MTU);
                    sig_send(0x04, cid, cfg, 8);
                }
            }
            break;
        case 0x04: /* Configuration Request: Kanal, Flags, Optionen (MTU) */
            if (cl >= 4 && le16(v) == AVRCP_CID && avrcp_remote) {
                put16(rsp, avrcp_remote);
                put16(rsp + 2, 0);
                put16(rsp + 4, 0);
                sig_send(0x05, id, rsp, 6);
                if (!avrcp_cfg_ms)
                    avrcp_cfg_ms = time_ms(); /* eingerichtet: gleich die Lautstaerke anmelden (abs_tick) */
            } else if (cl >= 4 && (le16(v) == LOCAL_CID || le16(v) == MEDIA_CID)) {
                int media = le16(v) == MEDIA_CID;
                uint16_t mtu = 672; /* ohne Angabe gilt der Standard */
                for (uint32_t k = 4; k + 2 <= cl; k += 2u + v[k + 1])
                    if ((v[k] & 0x7F) == 0x01 && v[k + 1] == 2 && k + 4 <= cl)
                        mtu = le16(v + k + 2);
                put16(rsp, media ? st.media_remote_cid : st.l2_remote_cid);
                put16(rsp + 2, 0);
                put16(rsp + 4, 0); /* angenommen */
                sig_send(0x05, id, rsp, 6);
                if (media) {
                    st.media_mtu = mtu;
                    media_cfg_done = 1;
                } else {
                    st.l2_remote_mtu = mtu;
                    remote_cfg_done = 1;
                }
            } else {
                put16(rsp, 0x0002); /* ungueltiger Kanal */
                put16(rsp + 2, cl >= 2 ? le16(v) : 0);
                put16(rsp + 4, 0);
                sig_send(0x01, id, rsp, 6);
            }
            break;
        case 0x06: /* Disconnection Request: Kanaele zurueck */
            if (cl >= 4) {
                sig_send(0x07, id, v, 4);
                if (le16(v) == LOCAL_CID) {
                    chan_closed = 1;
                    kprintf("bt: L2CAP: Gegenstelle schliesst den AVDTP-Kanal\n");
                } else if (le16(v) == AVRCP_CID) {
                    avrcp_remote = 0;
                    abs_reset(); /* Lautstaerke wieder digital */
                }
            }
            break;
        case 0x08: /* Echo */
            sig_send(0x09, id, v, cl < 60 ? cl : 60);
            break;
        case 0x0A: /* Information Request: nicht unterstuetzt */
            if (cl >= 2) {
                put16(rsp, le16(v));
                put16(rsp + 2, 0x0001);
                sig_send(0x0B, id, rsp, 4);
            }
            break;
        default:
            put16(rsp, 0x0000); /* Befehl nicht verstanden */
            sig_send(0x01, id, rsp, 2);
        }
        o += 4u + cl;
    }
}

/* ---------- AVDTP ---------- */

static int av_send(const uint8_t *d, uint32_t len)
{
    return l2_send(st.l2_remote_cid, d, len);
}

static void av_rx(const uint8_t *d, uint32_t len)
{
    if (len < 2)
        return;
    uint8_t label = d[0] >> 4, pt = (d[0] >> 2) & 3, mt = d[0] & 3, sig = d[1] & 0x3F;
    if (pt != 0) {
        kprintf("bt: AVDTP: zerteilte Nachricht (Art %u) - nicht unterstuetzt\n", pt);
        return;
    }
    if (mt != 0) { /* Antwort */
        if (avw.active && !avw.got && label == avw.label) {
            avw.msg_type = mt;
            avw.sig = sig;
            avw.len = len < sizeof(avw.d) ? len : sizeof(avw.d);
            memcpy(avw.d, d, avw.len);
            __sync_synchronize();
            avw.got = 1;
        }
        return;
    }
    /* Befehl der Gegenstelle: wir sind eine Audio-Quelle (Endpunkt 1) mit SBC */
    uint8_t r[16];
    r[0] = (uint8_t)(label << 4 | 2);
    r[1] = sig;
    kprintf("bt: AVDTP: Gegenstelle fragt Signal %#x\n", sig);
    if (sig == 0x01) { /* Discover */
        r[2] = 1 << 2;
        r[3] = 0 << 4 | 0 << 3;
        av_send(r, 4);
    } else if ((sig == 0x02 || sig == 0x0C) && len >= 3 && (d[2] >> 2) == 1) { /* (All) Capabilities */
        static const uint8_t caps[10] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35};
        memcpy(r + 2, caps, sizeof(caps));
        av_send(r, 2 + sizeof(caps));
    } else { /* allgemeine Ablehnung */
        r[0] = (uint8_t)(label << 4 | 1);
        av_send(r, 2);
    }
}

/* Befehl senden und auf die Antwort warten: Laenge (ab dem Kopf) bei "angenommen", sonst <0 */
static int av_request(uint8_t sig, const uint8_t *param, uint32_t plen, uint8_t *resp, uint32_t max, uint32_t ms)
{
    uint8_t b[64];
    uint8_t label = av_label++ & 0x0F;
    b[0] = (uint8_t)(label << 4);
    b[1] = sig;
    memcpy(b + 2, param, plen);
    avw.got = 0;
    avw.label = label;
    avw.active = 1;
    if (av_send(b, 2 + plen) != 0) {
        avw.active = 0;
        return -3;
    }
    int r = wait_events(&avw.got, ms);
    avw.active = 0;
    if (r < 0)
        return r;
    if (avw.msg_type != 2) {
        kprintf("bt: AVDTP: Signal %#x abgelehnt (Art %u, Fehler %#x)\n", sig, avw.msg_type,
                avw.len > 2 ? avw.d[avw.len - 1] : 0);
        return -4;
    }
    uint32_t n = avw.len < max ? avw.len : max;
    memcpy(resp, avw.d, n);
    return (int)avw.len;
}

/* ---------- AVRCP: Fernbedienung (wir sind "Target", die Soundbar "Controller") ----------
 * AVCTP-Kopf: Transaktion << 4 | Paketart << 2 | Antwort << 1 | ungueltige PID; PID 0x110E (AV Remote Control).
 * Dahinter AV/C: Art (Befehl bzw. Antwort), Subunit, Opcode, Operanden. Unit/Subunit Info beantworten wir als Panel;
 * Pass Through (Tasten) nehmen wir an und fuehren sie aus: Lautstaerke direkt am Mischer, Play/Pause/Stop/vor/zurueck
 * landen in einer Warteschlange fuer Programme (SYS_BT 10, music liest sie). Herstellerbefehle (Titel-Infos, absolute
 * Lautstaerke) und alles andere: "nicht implementiert" - dann bleibt die Soundbar bei den einfachen Tasten. */
#define AVC_NOT_IMPLEMENTED 0x08
#define AVC_ACCEPTED        0x09
#define AVC_STABLE          0x0C

static void mkey_push(uint8_t k)
{
    uint32_t next = (mkey_head + 1) % sizeof(mkeys);
    if (next == mkey_tail)
        return;
    mkeys[mkey_head] = k;
    mkey_head = next;
}

int bt_media_key(void)
{
    if (mkey_tail == mkey_head)
        return 0;
    int k = mkeys[mkey_tail];
    mkey_tail = (mkey_tail + 1) % sizeof(mkeys);
    return k;
}

static void pass_through(uint8_t op)
{
    static int saved_volume = 50;
    int v = hda_volume(-1);
    const char *name = "?";
    switch (op) {
    case 0x41: name = "lauter"; hda_volume(v + 5); break;
    case 0x42: name = "leiser"; hda_volume(v - 5 > 0 ? v - 5 : 0); break;
    case 0x43: /* stumm bzw. zurueck */
        name = "stumm";
        if (v > 0) {
            saved_volume = v;
            hda_volume(0);
        } else {
            hda_volume(saved_volume);
        }
        break;
    case 0x44: name = "Play"; mkey_push(BT_KEY_PLAY); break;
    case 0x46: name = "Pause"; mkey_push(BT_KEY_PAUSE); break;
    case 0x45: name = "Stop"; mkey_push(BT_KEY_STOP); break;
    case 0x4B: name = "vor"; mkey_push(BT_KEY_NEXT); break;
    case 0x4C: name = "zurueck"; mkey_push(BT_KEY_PREV); break;
    }
    kprintf("bt: AVRCP: Taste %s (%#x)\n", name, op);
}

/* ---------- AVRCP 1.4: absolute Lautstaerke (hier sind wir "Controller", die Soundbar "Target") ----------
 * Herstellerabhaengige Befehle (AV/C Opcode 0x00, Firma 0x001958 = Bluetooth SIG): RegisterNotification (PDU 0x31)
 * fuer EVENT_VOLUME_CHANGED (0x0D) - die Soundbar antwortet sofort mit INTERIM und ihrer Lautstaerke (0-127) und
 * spaeter mit CHANGED, wenn sie sich aendert (dann neu anmelden). SetAbsoluteVolume (PDU 0x50) stellt sie ein.
 * Angemeldet wird kurz nachdem die Soundbar den AVRCP-Kanal geoeffnet und eingerichtet hat. */
#define AVC_CONTROL  0x00
#define AVC_NOTIFY   0x03
#define AVC_REJECTED 0x0A
#define AVC_CHANGED  0x0D
#define AVC_INTERIM  0x0F
#define PDU_REGISTER_NOTIFICATION 0x31
#define PDU_SET_ABSOLUTE_VOLUME   0x50
#define EVENT_VOLUME_CHANGED      0x0D

static volatile int abs_state;        /* 0 noch nicht, 1 angemeldet (wartet), 2 laeuft, -1 kann die Soundbar nicht */
static volatile int abs_pending = -1; /* an die Soundbar zu schicken (0-127), -1 = nichts */
static uint64_t     abs_sent_ms; /* avrcp_cfg_ms (oben): Zeitpunkt, zu dem die Soundbar den Kanal eingerichtet hat */
static uint8_t      avrcp_label;

static void abs_reset(void)
{
    abs_state = 0;
    abs_pending = -1;
    avrcp_cfg_ms = 0;
}

int bt_abs_volume_active(void) { return abs_state == 2 && avrcp_remote; }

void bt_abs_volume_set(int percent)
{
    if (!bt_abs_volume_active())
        return;
    abs_pending = (percent * 127 + 50) / 100;
    event_signal(&worker_ev);
}

static int avrcp_vendor(uint8_t ctype, uint8_t pdu, const uint8_t *param, uint16_t plen)
{
    uint8_t b[32];
    if (!avrcp_remote || plen > sizeof(b) - 13)
        return -1;
    avrcp_label = (uint8_t)((avrcp_label + 1) & 0x0F);
    b[0] = (uint8_t)(avrcp_label << 4); /* einzelnes Paket, Befehl */
    b[1] = 0x11;
    b[2] = 0x0E;
    b[3] = ctype;
    b[4] = 0x48; /* Panel */
    b[5] = 0x00; /* herstellerabhaengig */
    b[6] = 0x00;
    b[7] = 0x19;
    b[8] = 0x58;
    b[9] = pdu;
    b[10] = 0x00;
    b[11] = (uint8_t)(plen >> 8);
    b[12] = (uint8_t)plen;
    memcpy(b + 13, param, plen);
    return l2_send(avrcp_remote, b, 13u + plen);
}

static void abs_register(void)
{
    static const uint8_t p[5] = {EVENT_VOLUME_CHANGED, 0, 0, 0, 0};
    if (avrcp_vendor(AVC_NOTIFY, PDU_REGISTER_NOTIFICATION, p, sizeof(p)) == 0) {
        if (abs_state != 2)
            abs_state = 1;
        abs_sent_ms = time_ms();
    }
}

/* im Thread "bt", nach jedem Aufwachen */
static void abs_tick(void)
{
    if (!avrcp_remote)
        return;
    uint64_t now = time_ms();
    if (abs_state == 0 && avrcp_cfg_ms && now - avrcp_cfg_ms >= 500) {
        abs_register();
    } else if (abs_state == 1 && now - abs_sent_ms > 3000) {
        kprintf("bt: AVRCP: Soundbar antwortet nicht auf die Anmeldung der Lautstaerke\n");
        abs_state = -1;
    } else if (abs_state == 2 && abs_pending >= 0) {
        uint8_t v = (uint8_t)abs_pending;
        abs_pending = -1;
        avrcp_vendor(AVC_CONTROL, PDU_SET_ABSOLUTE_VOLUME, &v, 1);
    }
}

/* Antwort der Soundbar auf unsere Befehle */
static void avrcp_response(const uint8_t *d, uint32_t len)
{
    if (len < 13 || d[5] != 0x00 || d[6] != 0x00 || d[7] != 0x19 || d[8] != 0x58)
        return;
    uint8_t rc = d[3], pdu = d[9];
    const uint8_t *p = d + 13;
    uint32_t plen = (uint32_t)(d[11] << 8 | d[12]);
    if (13 + plen > len)
        plen = len - 13;
    if (pdu == PDU_REGISTER_NOTIFICATION) {
        if ((rc == AVC_INTERIM || rc == AVC_CHANGED) && plen >= 2 && p[0] == EVENT_VOLUME_CHANGED) {
            int abs = p[1] & 0x7F, pct = (abs * 100 + 63) / 127;
            if (abs_state != 2)
                kprintf("bt: AVRCP: absolute Lautstaerke aktiv, Soundbar steht auf %d/127 (%d %%)\n", abs, pct);
            abs_state = 2;
            hda_volume_remote(pct);
            if (rc == AVC_CHANGED)
                abs_register(); /* jede Meldung gilt nur einmal: wieder anmelden */
        } else if (rc == AVC_NOT_IMPLEMENTED || rc == AVC_REJECTED) {
            kprintf("bt: AVRCP: Soundbar meldet ihre Lautstaerke nicht (Antwort %#x)\n", rc);
            abs_state = -1;
        }
    } else if (pdu == PDU_SET_ABSOLUTE_VOLUME && rc != AVC_ACCEPTED) {
        kprintf("bt: AVRCP: Soundbar lehnt die Lautstaerke ab (Antwort %#x)\n", rc);
    }
}

static void avrcp_rx(const uint8_t *d, uint32_t len)
{
    if (len < 3 || !avrcp_remote)
        return;
    if (d[0] & 0x02) { /* Antwort auf einen unserer Befehle */
        if (d[1] == 0x11 && d[2] == 0x0E && !(d[0] & 0x01))
            avrcp_response(d, len);
        return;
    }
    uint8_t r[64];
    uint32_t n = len < sizeof(r) ? len : sizeof(r);
    memcpy(r, d, n);
    r[0] = (uint8_t)((d[0] & 0xF0) | 0x02); /* Antwort, gleiche Transaktion */
    if (d[1] != 0x11 || d[2] != 0x0E) { /* andere PID: nur den Kopf mit "ungueltige PID" zurueck */
        r[0] |= 0x01;
        l2_send(avrcp_remote, r, 3);
        return;
    }
    if (len < 6)
        return;
    uint8_t op = d[5];
    if (op == 0x30 || op == 0x31) { /* Unit Info / Subunit Info: wir sind ein Panel */
        static const uint8_t info[5] = {0x07, 0x48, 0xFF, 0xFF, 0xFF};
        r[3] = AVC_STABLE;
        r[4] = 0xFF;
        r[5] = op;
        memcpy(r + 6, info, 5);
        l2_send(avrcp_remote, r, 11);
    } else if (op == 0x7C && len >= 8) { /* Pass Through: Taste (Bit 7 = losgelassen) */
        r[3] = AVC_ACCEPTED;
        l2_send(avrcp_remote, r, n);
        if (!(d[6] & 0x80))
            pass_through(d[6] & 0x7F);
    } else {
        r[3] = AVC_NOT_IMPLEMENTED;
        l2_send(avrcp_remote, r, n);
    }
}

/* ---------- Thread "bt": Antworten auf Anfragen des Geraets ---------- */

static void handle_event(uint8_t code, const uint8_t *p, uint32_t plen)
{
    uint8_t c[32], r[16];
    switch (code) {
    case 0x04: /* jemand will sich mit uns verbinden: ablehnen (wir verbinden selbst) */
        if (plen >= 6) {
            memcpy(c, p, 6);
            c[6] = 0x0D; /* begrenzte Mittel */
            kprintf("bt: Verbindungswunsch von aussen abgelehnt\n");
            hci_cmd_status(0x040A, c, 7);
        }
        break;
    case 0x17: { /* Link Key Request: gespeicherter Schluessel, sonst neu koppeln */
        if (plen < 6)
            break;
        int i = key_find(p);
        memcpy(c, p, 6);
        if (i >= 0) {
            memcpy(c + 6, keys[i].key, 16);
            kprintf("bt: gespeicherten Schluessel verwendet\n");
            hci_cmd(0x040B, c, 22, r, sizeof(r), 2000);
        } else {
            kprintf("bt: kein Schluessel gespeichert - neu koppeln\n");
            hci_cmd(0x040C, c, 6, r, sizeof(r), 2000);
        }
        break;
    }
    case 0x16: /* PIN Code Request (altes Koppeln): "0000" */
        if (plen >= 6) {
            memset(c, 0, sizeof(c));
            memcpy(c, p, 6);
            c[6] = 4;
            memcpy(c + 7, "0000", 4);
            kprintf("bt: Gegenstelle will eine PIN - versuche 0000\n");
            hci_cmd(0x040D, c, 23, r, sizeof(r), 2000);
        }
        break;
    case 0x31: /* IO Capability Request: keine Ein-/Ausgabe, kein OOB, Bonding ohne MITM */
        if (plen >= 6) {
            memcpy(c, p, 6);
            c[6] = 0x03;
            c[7] = 0x00;
            c[8] = 0x04;
            hci_cmd(0x042B, c, 9, r, sizeof(r), 2000);
        }
        break;
    case 0x32:
        if (plen >= 9)
            kprintf("bt: Gegenstelle: IO-Faehigkeit %u, OOB %u, Anforderung %#x\n", p[6], p[7], p[8]);
        break;
    case 0x33: /* User Confirmation Request: annehmen ("Just Works") */
        if (plen >= 6) {
            memcpy(c, p, 6);
            kprintf("bt: Kopplung bestaetigt (Vergleichswert %06u)\n", plen >= 10 ? (unsigned)(p[6] | p[7] << 8 |
                    p[8] << 16 | (uint32_t)p[9] << 24) : 0);
            hci_cmd(0x042C, c, 6, r, sizeof(r), 2000);
        }
        break;
    case 0x34: /* User Passkey Request: wir koennen nichts eingeben */
        if (plen >= 6) {
            memcpy(c, p, 6);
            kprintf("bt: Gegenstelle will einen Zahlencode - koennen wir nicht eingeben\n");
            hci_cmd(0x042F, c, 6, r, sizeof(r), 2000);
        }
        break;
    case 0x36:
        if (plen >= 1)
            kprintf("bt: Simple Pairing fertig: Status %#x\n", p[0]);
        break;
    }
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        event_wait(&worker_ev, 200);
        while (evq_tail != evq_head) {
            handle_event(evq[evq_tail].code, evq[evq_tail].p, evq[evq_tail].len);
            evq_tail = (evq_tail + 1) % EVQ;
        }
        while (rxq_tail != rxq_head) {
            uint16_t cid = rxq[rxq_tail].cid;
            if (cid == CID_SIG)
                sig_rx(rxq[rxq_tail].d, rxq[rxq_tail].len);
            else if (cid == LOCAL_CID)
                av_rx(rxq[rxq_tail].d, rxq[rxq_tail].len);
            else if (cid == MEDIA_CID)
                ; /* Audiodaten der Gegenstelle: nicht erwartet */
            else if (cid == AVRCP_CID)
                avrcp_rx(rxq[rxq_tail].d, rxq[rxq_tail].len);
            else
                kprintf("bt: L2CAP-PDU fuer unbekannten Kanal %#x\n", cid);
            rxq_tail = (rxq_tail + 1) % RXQ;
        }
        abs_tick(); /* absolute Lautstaerke anmelden bzw. an die Soundbar schicken */
    }
}

void bt_conn_start(void)
{
    if (worker_started)
        return;
    if (!acl_dma && !(acl_dma = (uint8_t *)pmm_alloc_frames(1)))
        return;
    worker_started = 1;
    thread_create("bt", worker, 0);
    bt_a2dp_init();
}

/* ---------- Verbinden ---------- */

static int fail(int step, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(int step, int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(st.msg, sizeof(st.msg), fmt, ap);
    va_end(ap);
    kprintf("bt: Verbinden gescheitert: %s\n", st.msg);
    st.step = (uint32_t)step;
    st.error = err;
    st.state = BT_CONN_FAILED;
    if (handle != NO_HANDLE) { /* Verbindung wieder abbauen */
        uint8_t d[3] = {(uint8_t)handle, (uint8_t)(handle >> 8), 0x13};
        disc_done = 0;
        if (hci_cmd_status(0x0406, d, 3) == 0)
            wait_events(&disc_done, 3000);
        handle = NO_HANDLE;
    }
    return err;
}

static const char *hci_err(uint8_t s)
{
    switch (s) {
    case 0x04: return "Geraet antwortet nicht (eingeschaltet und in Reichweite?)";
    case 0x05: return "Authentifizierung fehlgeschlagen";
    case 0x06: return "Schluessel fehlt - Geraet in den Kopplungsmodus bringen";
    case 0x08: return "Zeit abgelaufen";
    case 0x0D: case 0x0E: case 0x0F: return "Gegenstelle lehnt ab";
    case 0x13: return "Gegenstelle hat getrennt";
    case 0x16: return "lokal getrennt";
    case 0x18: return "Koppeln nicht erlaubt";
    case 0x29: return "Koppeln mit Einheitsschluessel nicht unterstuetzt";
    default: return "Fehler";
    }
}

static int connect_locked(const uint8_t addr[6])
{
    if (handle != NO_HANDLE) {
        if (memcmp(addr, st.addr, 6) == 0 && st.state == BT_CONN_READY)
            return 0;
        say("schon mit einem anderen Geraet verbunden - erst bt disconnect");
        return -12;
    }
    memset(&st, 0, sizeof(st));
    memcpy(st.addr, addr, 6);
    rev6(peer, addr);
    st.state = BT_CONN_CONNECTING;
    st.l2_local_cid = LOCAL_CID;
    uint64_t t0 = time_ms();

    st.step = BT_CSTEP_HCI;
    int r = hci_init_locked();
    if (r < 0)
        return fail(BT_CSTEP_HCI, r, "Firmware/HCI nicht bereit (%d)", r);

    /* 1. Verbindung: Pakettypen DM1..DH5, Page-Scan-Modus R2, Rollentausch erlaubt */
    st.step = BT_CSTEP_PAGE;
    uint8_t cc[13];
    memcpy(cc, peer, 6);
    put16(cc + 6, 0xCC18);
    cc[8] = 0x02;
    cc[9] = 0x00;
    put16(cc + 10, 0x0000);
    cc[12] = 0x01;
    conn_done = 0;
    remote_cfg_done = media_cfg_done = chan_closed = 0;
    sig_connecting = media_connecting = sig_our_cfg = media_our_cfg = 0;
    sig_cfg_id = media_cfg_id = 0;
    avrcp_remote = 0;
    abs_reset();
    mkey_tail = mkey_head;
    acl_as_len = l2_need = 0;
    rxq_tail = rxq_head;
    say("verbinde mit %02x:%02x:%02x:%02x:%02x:%02x ...", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    if ((r = hci_cmd_status(0x0405, cc, 13)) != 0)
        return fail(BT_CSTEP_PAGE, r, "Create Connection abgelehnt (%d)", r);
    if (wait_events(&conn_done, 15000) != 0)
        return fail(BT_CSTEP_PAGE, -1, "keine Verbindung nach 15 s");
    if (st.conn_status)
        return fail(BT_CSTEP_PAGE, -14, "%s (Status %#x)", hci_err(st.conn_status), st.conn_status);
    st.handle = handle;
    credits = bt_state()->acl_pkts ? bt_state()->acl_pkts : 4;
    kprintf("bt: verbunden nach %u ms, Handle %#x\n", (uint32_t)(time_ms() - t0), handle);

    /* 2. Koppeln bzw. gespeicherten Schluessel verwenden */
    st.step = BT_CSTEP_AUTH;
    uint8_t hp[3] = {(uint8_t)handle, (uint8_t)(handle >> 8), 0x01};
    auth_done = 0;
    if ((r = hci_cmd_status(0x0411, hp, 2)) != 0)
        return fail(BT_CSTEP_AUTH, r, "Authentication Requested abgelehnt (%d)", r);
    if (wait_events(&auth_done, 40000) != 0)
        return fail(BT_CSTEP_AUTH, -1, "Koppeln dauert zu lange");
    if (st.auth_status)
        return fail(BT_CSTEP_AUTH, -14, "%s (Status %#x)", hci_err(st.auth_status), st.auth_status);
    kprintf("bt: authentifiziert nach %u ms%s\n", (uint32_t)(time_ms() - t0), st.paired_new ? " (neu gekoppelt)" : "");

    /* 3. Verschluesselung */
    st.step = BT_CSTEP_ENCRYPT;
    enc_done = 0;
    if ((r = hci_cmd_status(0x0413, hp, 3)) != 0)
        return fail(BT_CSTEP_ENCRYPT, r, "Set Connection Encryption abgelehnt (%d)", r);
    if (wait_events(&enc_done, 10000) != 0 || !st.encrypted)
        return fail(BT_CSTEP_ENCRYPT, -14, "Verschluesselung nicht eingeschaltet (Status %#x)", st.enc_status);

    /* 4. L2CAP-Kanal zu AVDTP und Konfiguration */
    st.step = BT_CSTEP_L2CAP;
    uint8_t req[8], resp[64], rc = 0;
    int n;
    wait_events(&remote_cfg_done, 300); /* oeffnet die Soundbar den Kanal gerade selbst? Kurz abwarten */
    if (st.l2_remote_cid) { /* ja: ihren Kanal nehmen, unsere Konfiguration abwarten */
        kprintf("bt: die Gegenstelle hat den AVDTP-Kanal geoeffnet - wird verwendet\n");
        uint64_t tc = time_ms();
        while (!sig_our_cfg && time_ms() - tc < 5000)
            wait_events(&sig_our_cfg, 100);
        if (sig_our_cfg != 1)
            return fail(BT_CSTEP_L2CAP, -14, "Konfiguration des Kanals der Gegenstelle abgelehnt");
    } else {
        put16(req, PSM_AVDTP);
        put16(req + 2, LOCAL_CID);
        sig_connecting = 1;
        n = sig_request(0x02, req, 4, 10000, &rc, resp);
        if (n >= 8 && rc == 0x03 && le16(resp + 4) == 0)
            st.l2_remote_cid = le16(resp);
        sig_connecting = 0;
        if (n < 8 || rc != 0x03)
            return fail(BT_CSTEP_L2CAP, -15, "keine Antwort auf die L2CAP-Verbindung (%d, Code %#x)", n, rc);
        if (le16(resp + 4) != 0)
            return fail(BT_CSTEP_L2CAP, -14, "Gegenstelle lehnt AVDTP ab (Ergebnis %#x)", le16(resp + 4));
        put16(req, st.l2_remote_cid);
        put16(req + 2, 0);
        req[4] = 0x01; /* Option MTU */
        req[5] = 2;
        put16(req + 6, L2_MTU);
        n = sig_request(0x04, req, 8, 5000, &rc, resp);
        if (n < 6 || rc != 0x05 || le16(resp + 4) != 0)
            return fail(BT_CSTEP_L2CAP, -14, "Konfiguration abgelehnt (%d, Code %#x, Ergebnis %#x)", n, rc,
                        n >= 6 ? le16(resp + 4) : 0);
    }
    if (wait_events(&remote_cfg_done, 5000) != 0)
        return fail(BT_CSTEP_L2CAP, -15, "Gegenstelle konfiguriert den Kanal nicht");
    kprintf("bt: AVDTP-Kanal offen: unser %#x, ihrer %#x, MTU der Gegenstelle %u\n", LOCAL_CID, st.l2_remote_cid,
            st.l2_remote_mtu);

    /* 5. AVDTP: Endpunkte und ihre Codecs */
    st.step = BT_CSTEP_DISCOVER;
    static uint8_t av[300];
    n = av_request(0x01, 0, 0, av, sizeof(av), 5000);
    if (n < 2)
        return fail(BT_CSTEP_DISCOVER, -15, "AVDTP Discover ohne Antwort (%d)", n);
    for (int i = 2; i + 1 < n && st.n_seps < 8; i += 2) {
        BtSep *s = &st.seps[st.n_seps++];
        s->seid = av[i] >> 2;
        s->in_use = (av[i] >> 1) & 1;
        s->media = av[i + 1] >> 4;
        s->tsep = (av[i + 1] >> 3) & 1;
        s->codec = 0xFE; /* noch unbekannt */
    }
    st.step = BT_CSTEP_CAPS;
    for (int i = 0; i < st.n_seps; i++) {
        BtSep *s = &st.seps[i];
        if (s->media != 0 || s->tsep != 1)
            continue; /* nur Audio-Senken */
        uint8_t q = (uint8_t)(s->seid << 2);
        n = av_request(0x02, &q, 1, av, sizeof(av), 5000);
        for (int o = 2; n > 0 && o + 2 <= n;) { /* Faehigkeiten: Kategorie, Laenge, Daten */
            uint8_t cat = av[o], l = av[o + 1];
            if (o + 2 + l > n)
                break;
            if (cat == 0x07 && l >= 2) { /* Media Codec: Medienart, Codec, Angaben */
                s->codec = av[o + 3];
                s->caps_len = (uint8_t)(l - 2 < 8 ? l - 2 : 8);
                memcpy(s->caps, av + o + 4, s->caps_len);
            }
            o += 2 + l;
        }
        kprintf("bt: Endpunkt %u: Senke, Codec %#x, Angaben %02x %02x %02x %02x%s\n", s->seid, s->codec, s->caps[0],
                s->caps[1], s->caps[2], s->caps[3], s->in_use ? " (belegt)" : "");
    }
    st.step = BT_CSTEP_DONE;
    st.state = BT_CONN_READY;
    say("verbunden nach %u ms: %u Audio-Endpunkt(e)%s", (uint32_t)(time_ms() - t0), st.n_seps,
        st.paired_new ? ", neu gekoppelt" : "");
    return 0;
}

/* ---------- A2DP: Codec einstellen, Medienkanal, Start/Pause (mit bt_lock, aus a2dp.c) ---------- */

/* L2CAP-Kanal zu AVDTP oeffnen und in beide Richtungen konfigurieren; 0 oder <0 */
static int l2_open(uint16_t local, uint16_t *remote, volatile int *remote_done)
{
    uint8_t req[8], resp[64], rc = 0;
    put16(req, PSM_AVDTP);
    put16(req + 2, local);
    int n = sig_request(0x02, req, 4, 10000, &rc, resp);
    if (n < 8 || rc != 0x03 || le16(resp + 4) != 0) {
        kprintf("bt: L2CAP-Kanal %#x: Verbindung abgelehnt (%d, Code %#x, Ergebnis %#x)\n", local, n, rc,
                n >= 6 ? le16(resp + 4) : 0);
        return -1;
    }
    *remote = le16(resp);
    put16(req, *remote);
    put16(req + 2, 0);
    req[4] = 0x01;
    req[5] = 2;
    put16(req + 6, L2_MTU);
    n = sig_request(0x04, req, 8, 5000, &rc, resp);
    if (n < 6 || rc != 0x05 || le16(resp + 4) != 0) {
        kprintf("bt: L2CAP-Kanal %#x: Konfiguration abgelehnt\n", local);
        return -1;
    }
    if (wait_events(remote_done, 5000) != 0) {
        kprintf("bt: L2CAP-Kanal %#x: Gegenstelle konfiguriert nicht\n", local);
        return -1;
    }
    return 0;
}

int bt_conn_ready(void)
{
    return st.state == BT_CONN_READY && handle != NO_HANDLE;
}

BtConn *bt_conn_state(void)
{
    return &st;
}

int bt_av_setup(void)
{
    if (!bt_conn_ready())
        return -1;
    BtSep *sep = 0;
    for (int i = 0; i < st.n_seps && !sep; i++)
        if (st.seps[i].media == 0 && st.seps[i].tsep == 1 && st.seps[i].codec == 0 && !st.seps[i].in_use &&
            st.seps[i].caps_len >= 4)
            sep = &st.seps[i];
    if (!sep) {
        say("keine freie SBC-Senke unter den Endpunkten");
        return -2;
    }
    /* Pflicht fuer jede SBC-Senke: 48 kHz, Stereo, 16 Bloecke, 8 Baender, Loudness */
    uint8_t maxbp = sep->caps[3] < 53 ? sep->caps[3] : 53, minbp = sep->caps[2] > 2 ? sep->caps[2] : 2;
    if (maxbp < minbp)
        maxbp = minbp;
    uint8_t p[12] = {(uint8_t)(sep->seid << 2), 1 << 2, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x10 | 0x02,
                     0x10 | 0x04 | 0x01, minbp, maxbp};
    uint8_t r[64];
    if (av_request(0x03, p, sizeof(p), r, sizeof(r), 5000) < 0) {
        say("Soundbar lehnt die SBC-Einstellung ab (Endpunkt %u)", sep->seid);
        return -3;
    }
    st.a2dp_seid = sep->seid;
    st.a2dp_bitpool = maxbp;
    uint8_t q = (uint8_t)(sep->seid << 2);
    if (av_request(0x06, &q, 1, r, sizeof(r), 5000) < 0) { /* Open */
        say("AVDTP Open abgelehnt");
        return -4;
    }
    wait_events(&media_cfg_done, 300); /* manche Soundbars oeffnen den Medienkanal nach Open selbst */
    if (st.media_remote_cid) {
        kprintf("bt: die Gegenstelle hat den Medienkanal geoeffnet - wird verwendet\n");
        uint64_t tc = time_ms();
        while ((!media_our_cfg || !media_cfg_done) && time_ms() - tc < 5000)
            wait_events(&media_cfg_done, 100);
        if (media_our_cfg != 1 || !media_cfg_done) {
            say("Medienkanal der Gegenstelle nicht konfiguriert");
            return -5;
        }
    } else {
        media_connecting = 1;
        int r2 = l2_open(MEDIA_CID, (uint16_t *)&st.media_remote_cid, &media_cfg_done);
        media_connecting = 0;
        if (r2 != 0) {
            say("Medienkanal nicht geoeffnet");
            return -5;
        }
    }
    kprintf("bt: A2DP eingerichtet: Endpunkt %u, SBC 48 kHz Stereo, Bitpool %u, Medienkanal %#x <-> %#x, MTU %u\n",
            sep->seid, maxbp, MEDIA_CID, st.media_remote_cid, st.media_mtu);
    return 0;
}

int bt_av_start(int start)
{
    uint8_t q = (uint8_t)(st.a2dp_seid << 2), r[16];
    int n = av_request(start ? 0x07 : 0x09, &q, 1, r, sizeof(r), 3000); /* Start bzw. Suspend */
    kprintf("bt: AVDTP %s: %s\n", start ? "Start" : "Suspend", n >= 0 ? "ok" : "abgelehnt");
    return n < 0 ? -1 : 0;
}

int bt_media_send(const uint8_t *d, uint32_t len)
{
    return l2_send(st.media_remote_cid, d, len);
}

int bt_connect(const uint8_t addr[6])
{
    if (!bt_usb_dev() || !usb_alive(bt_usb_dev()))
        return -1;
    mutex_lock(&bt_lock);
    int r = connect_locked(addr);
    mutex_unlock(&bt_lock);
    return r;
}

int bt_disconnect(void)
{
    if (!bt_usb_dev())
        return -1;
    mutex_lock(&bt_lock);
    if (handle != NO_HANDLE) {
        uint8_t d[3] = {(uint8_t)handle, (uint8_t)(handle >> 8), 0x13}; /* Grund: Benutzer beendet */
        disc_done = 0;
        if (hci_cmd_status(0x0406, d, 3) == 0)
            wait_events(&disc_done, 3000);
        handle = NO_HANDLE;
        say("getrennt");
    }
    st.state = BT_CONN_IDLE;
    mutex_unlock(&bt_lock);
    return 0;
}

void bt_conn_info(BtConn *out)
{
    *out = st;
    out->handle = handle;
}

int bt_key_get(unsigned i, BtKey *out)
{
    for (int k = 0; k < MAX_KEYS; k++)
        if (keys[k].used && i-- == 0) {
            rev6(out->addr, keys[k].addr);
            out->type = keys[k].type;
            out->pad = 0;
            memcpy(out->key, keys[k].key, 16);
            return 0;
        }
    return -1;
}

int bt_key_add(const BtKey *k)
{
    uint8_t a[6];
    rev6(a, k->addr);
    key_store(a, k->key, k->type);
    return 0;
}
