/* Intel AX200, Stufe 5: mit einem Netz verbinden (offen oder WPA2-PSK) und Daten austauschen - als wlan0 im
 * Netzwerk-Stack.
 *
 * Ablauf wie iwlmvm in Linux (Befehle mit der Firmware-Schnittstelle "ohne MLD"; Versionen aus der Firmware-Datei
 * cc-a0-77: PHY_CONTEXT 4, ADD_STA 12, ADD_STA_KEY 3, TX_CMD 9, SCD_QUEUE_CONFIG 3, RLC_CONFIG 2):
 *   1 PHY-Kontext auf den Kanal des AP, Empfangsketten (RLC), MAC-Kontext (Station, BSSID), Bindung MAC <-> PHY
 *   2 Station fuer den AP (ADD_STA), Sendewarteschlangen fuer Verwaltungsrahmen (TID 15) und Daten (TID 0)
 *   3 Zeitfenster auf dem Kanal (SESSION_PROTECTION), Authentifizierung (offen), Assoziierung
 *   4 MAC-Kontext "assoziiert"; bei WPA2 der 4-Wege-Handshake (net/wpa.c), dann Paar- und Gruppenschluessel in die
 *     Firmware (ADD_STA_KEY) - sie ver- und entschluesselt CCMP selbst
 * Wir treten als 802.11a/g-Station auf (ohne HT/VHT/HE und ohne QoS): einfache Datenrahmen, feste Senderate (nach der
 * Signalstaerke beim Suchen gewaehlt), keine Aggregation. Das versteht jeder AP, nur nicht so schnell.
 *
 * Senden: jede Warteschlange ist ein Ring aus 64 TFDs (je ein Puffer mit Befehlskopf, TX-Befehl, 802.11-Kopf, auf 4 Byte
 * aufgefuellt, Nutzdaten) und einer Tabelle mit der Laenge je Eintrag (in 32-Bit-Worten, gen2). Die Firmware meldet
 * jeden Rahmen mit einer TX-Antwort. Empfangen: Datenrahmen vom AP werden zu Ethernet-Rahmen (LLC/SNAP weg) und warten
 * in einem Ring, bis der Netzwerk-Thread sie abholt; EAPOL-Rahmen gehen an den Handshake. */

#include "drivers/net/iwl_internal.h"
#include <stdarg.h>
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "lib/crypto.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "net/net.h"
#include "net/wpa.h"

/* Befehle */
#define CMD_PHY_CONTEXT    0x08
#define CMD_ADD_STA_KEY    0x17
#define CMD_ADD_STA        0x18
#define CMD_MAC_CONTEXT    0x28
#define CMD_BINDING        0x2B
#define CMD_SESSION_PROT   0x05 /* MAC_CONF_GROUP */
#define NOTIF_SESSION_PROT 0xFB /* MAC_CONF_GROUP */
#define CMD_RLC_CONFIG     0x08 /* DATA_PATH_GROUP */
#define CMD_SCD_QUEUE_CFG  0x17 /* DATA_PATH_GROUP */

#define ACTION_ADD      1
#define ACTION_MODIFY   2
#define FW_CTXT_INVALID 0xFFFFFFFFu
#define MAC_ID          0 /* Kennung und Farbe (Farbe 0): id | farbe << 8 */
#define PHY_ID          0
#define STA_ID          0 /* der AP */
#define TID_DATA        0
#define TID_MGMT        15

/* PHY_CONTEXT_CMD Version 4 mit der Kanalbeschreibung fuer Kanalnummern ueber 255 (Faehigkeit ULTRA_HB_CHANNELS) */
typedef struct __attribute__((packed)) {
    uint32_t id_and_color, action;
    uint32_t channel;
    uint8_t  band, width, ctrl_pos, reserved;
    uint32_t lmac_id, rxchain_info, dsp_cfg_flags, reserved2;
} PhyCtxtCmd;
_Static_assert(sizeof(PhyCtxtCmd) == 32, "PHY_CONTEXT_CMD");

/* RLC_CONFIG_CMD Version 2: Empfangsketten des PHY-Kontexts */
typedef struct __attribute__((packed)) {
    uint32_t phy_id;
    uint32_t rx_chain_info, rlc_reserved;
    uint32_t sad[4];
    uint8_t  flags, reserved[3];
} RlcCmd;
_Static_assert(sizeof(RlcCmd) == 32, "RLC_CONFIG_CMD");

typedef struct __attribute__((packed)) {
    uint16_t cw_min, cw_max;
    uint8_t  aifsn, fifos_mask;
    uint16_t edca_txop;
} AcQos;

/* MAC_CONTEXT_CMD (iwl_mac_ctx_cmd) mit den Daten einer Station (iwl_mac_data_sta, Union bis 48 Byte) */
typedef struct __attribute__((packed)) {
    uint32_t id_and_color, action, mac_type, tsf_id;
    uint8_t  node_addr[6];
    uint16_t reserved_node;
    uint8_t  bssid_addr[6];
    uint16_t reserved_bssid;
    uint32_t cck_rates, ofdm_rates, protection_flags, cck_short_preamble, short_slot, filter_flags;
    uint32_t qos_flags;
    AcQos    ac[5];
    uint32_t is_assoc, dtim_time;
    uint64_t dtim_tsf;
    uint32_t bi, reserved1, dtim_interval, data_policy, listen_interval, assoc_id, assoc_beacon_arrive_time;
    uint32_t union_rest;
} MacCtxtCmd;
_Static_assert(sizeof(MacCtxtCmd) == 148, "MAC_CONTEXT_CMD");

#define FW_MAC_TYPE_BSS_STA    5
#define MAC_FILTER_ACCEPT_GRP  (1u << 2)
#define MAC_FILTER_IN_BEACON   (1u << 6)
#define MAC_FLG_SHORT_SLOT     (1u << 4)
#define MAC_FLG_SHORT_PREAMBLE (1u << 5)

/* BINDING_CONTEXT_CMD Version 2 (mit lmac_id) */
typedef struct __attribute__((packed)) {
    uint32_t id_and_color, action, macs[3], phy, lmac_id;
} BindingCmd;
_Static_assert(sizeof(BindingCmd) == 28, "BINDING_CONTEXT_CMD");

/* ADD_STA Version 12 (iwl_mvm_add_sta_cmd) */
typedef struct __attribute__((packed)) {
    uint8_t  add_modify, awake_acs;
    uint16_t tid_disable_tx;
    uint32_t mac_id_n_color;
    uint8_t  addr[6];
    uint16_t reserved2;
    uint8_t  sta_id, modify_mask;
    uint16_t reserved3;
    uint32_t station_flags, station_flags_msk;
    uint8_t  add_immediate_ba_tid, remove_immediate_ba_tid;
    uint16_t add_immediate_ba_ssn, sleep_tx_count;
    uint8_t  sleep_state_flags, station_type;
    uint16_t assoc_id, beamform_flags;
    uint32_t tfd_queue_msk;
    uint16_t rx_ba_window;
    uint8_t  sp_length, uapsd_acs;
} AddStaCmd;
_Static_assert(sizeof(AddStaCmd) == 48, "ADD_STA");

#define STA_FLG_RTS_MIMO_PROT (1u << 17)
#define STA_FLG_FAT_EN_MSK    (3u << 26)
#define STA_FLG_MIMO_EN_MSK   (3u << 28)
#define ADD_STA_SUCCESS       1

/* ADD_STA_KEY Version 3 (iwl_mvm_add_sta_key_cmd) */
typedef struct __attribute__((packed)) {
    uint8_t  sta_id, key_offset;
    uint16_t key_flags;
    uint8_t  key[32];
    uint8_t  rx_secur_seq_cnt[16];
    uint64_t rx_mic_key, tx_mic_key, transmit_seq_cnt;
} AddStaKeyCmd;
_Static_assert(sizeof(AddStaKeyCmd) == 76, "ADD_STA_KEY");

#define STA_KEY_FLG_CCM      2
#define STA_KEY_FLG_KEY_MAP  (1u << 3)
#define STA_KEY_FLG_KEYID(i) ((uint16_t)((i) & 3) << 8)
#define STA_KEY_MULTICAST    (1u << 14)

/* SESSION_PROTECTION_CMD: die Firmware bleibt so lange auf dem Kanal (Auth, Assoc, Handshake) */
typedef struct __attribute__((packed)) {
    uint32_t id_and_color, action, conf_id, duration_tu, repetition_count, interval;
} SessionProtCmd;

/* SCD_QUEUE_CONFIG_CMD Version 3, Operation "hinzufuegen" */
typedef struct __attribute__((packed)) {
    uint32_t operation, sta_mask;
    uint8_t  tid, reserved[3];
    uint32_t flags, cb_size;
    uint64_t bc_dram_addr, tfdq_dram_addr;
} ScdQueueCmd;
_Static_assert(sizeof(ScdQueueCmd) == 36, "SCD_QUEUE_CONFIG_CMD");

/* TX_CMD fuer die Familie 22000 (iwl_tx_cmd_gen2), danach der 802.11-Kopf */
typedef struct __attribute__((packed)) {
    uint16_t len, offload_assist;
    uint32_t flags;
    uint32_t pn_low;
    uint16_t pn_high, aux_info;
    uint32_t rate_n_flags;
} TxCmd;
_Static_assert(sizeof(TxCmd) == 20, "TX_CMD");

#define TX_FLAGS_CMD_RATE    (1u << 0) /* Rate aus rate_n_flags (sonst waehlt die Firmware) */
#define TX_FLAGS_ENCRYPT_DIS (1u << 1)
#define TX_FLAGS_HIGH_PRI    (1u << 2)
/* rate_n_flags im neuen Format (TX_CMD ab Version 9): Rate 0..7 (OFDM 6..54) bzw. 0..3 (CCK 1..11), Art, Antenne */
#define RATE_OFDM            (1u << 8)
#define RATE_ANT_POS         14

/* Beschreibung vor einem empfangenen Rahmen (iwl_rx_mpdu_desc bis einschliesslich v1) */
#define RX_DESC          48
#define RX_CRC_OK        (1u << 0)
#define RX_MIC_OK        (1u << 6)
#define RX_SEC_MASK      (7u << 8)
#define RX_SEC_CCM       (2u << 8)
#define RX_DUPLICATE     (1u << 22)
#define RX_MFLG2_PAD     0x20

_Static_assert(sizeof(WlanStatus) == 216 && sizeof(WlanConnect) == 104, "WlanStatus/WlanConnect wie in user.h");

#define TXQ_SIZE  64
#define TXQ_SLOT  2048
#define TFD_SIZE  256
#define HBUS_TARG_WRPTR 0x460
#define RXQ_LEN   64
#define RXQ_SLOT  1536
#define EAPOL_MAX 1024

typedef struct {
    int       qid;       /* Nummer bei der Firmware, -1 = keine */
    uint8_t  *tfds, *buf;
    uint16_t *bc;        /* Laengentabelle */
    uint32_t  write;     /* Schreibzeiger 0..255 */
    uint32_t  inflight;  /* gesendet, noch ohne TX-Antwort */
    uint64_t  last_ms;   /* letzte TX-Antwort */
} TxQ;

static struct {
    volatile int state;    /* WLAN_ST_* */
    int       joined;      /* Kontexte in der Firmware: vor dem naechsten Verbinden neu laden */
    WlanNet   net;
    IwlBss    bss;
    uint8_t   band;        /* PHY_BAND_24 = 1, PHY_BAND_5 = 0 */
    uint8_t   ant;         /* Sendeantenne (1 = A, 2 = B) */
    uint16_t  seq;         /* Folgenummern der gesendeten Rahmen */
    uint32_t  rate_mgmt, rate_data;
    int       secure;      /* WPA2 */
    volatile int keys_on;  /* Paarschluessel eingebaut: Daten werden verschluesselt */
    Wpa       wpa;
    uint8_t   rsn_ie[24];
    uint32_t  rsn_ie_len;
    /* Antworten des AP (beim Verbinden) */
    volatile int auth_got, assoc_got;
    uint16_t  auth_status, assoc_status, assoc_aid;
    /* letzter Beacon des AP: Zeitstempel (TSF), Zeit der Karte (GP2), DTIM-Zaehler */
    uint64_t  beacon_tsf;
    uint32_t  beacon_gp2;
    uint8_t   dtim_count;
    int       beacon_seen;
    /* Replay-Schutz: naechste erlaubte CCMP-Paketnummer, paarweise und je Gruppenschluessel (Nummer 0..3) */
    uint64_t  pn_next_uc, pn_next_mc[4];
    volatile int kicked; /* der AP hat uns abgemeldet (Deauth/Disassoc) */
    /* EAPOL-Rahmen vom AP, wartet auf die Auswertung (ab dem EAPOL-Kopf) */
    uint8_t   eapol[EAPOL_MAX];
    volatile uint32_t eapol_len;
    WlanStatus st;
} sta;

static TxQ      mgmtq = {.qid = -1}, dataq = {.qid = -1};
static uint8_t *rxq;                 /* Ring empfangener Ethernet-Rahmen: je 2 Byte Laenge + Rahmen */
static uint32_t rxq_head, rxq_count;
static Event    worker_ev;
static int      worker_started;
static uint8_t  eapol_tmp[EAPOL_MAX], eapol_out[512];

static const char *const step_name[] = {"-", "Firmware", "Netz suchen", "Schluessel aus dem Passwort", "Kontexte",
                                        "Station", "Warteschlangen", "Zeitfenster", "Authentifizierung",
                                        "Assoziierung", "WPA2-Handshake", "verbunden"};

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(sta.st.msg, sizeof(sta.st.msg), fmt, ap);
    va_end(ap);
    kprintf("wlan: %s\n", sta.st.msg);
}

static uint16_t get16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* ---------- Zufall (SNonce) ---------- */

static void random_bytes(uint8_t *out, uint32_t n)
{
    static uint8_t pool[20];
    static int have_rdrand = -1;
    if (have_rdrand < 0) {
        uint32_t a, b, c, d;
        __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        have_rdrand = (c >> 30) & 1;
    }
    while (n) {
        uint64_t mix[6] = {rdtsc(), time_us(), 0, 0, 0, 0};
        for (int i = 2; i < 6 && have_rdrand; i++) {
            unsigned char ok = 0;
            for (int t = 0; t < 10 && !ok; t++)
                __asm__ __volatile__("rdrand %0; setc %1" : "=r"(mix[i]), "=qm"(ok));
        }
        Sha1 h;
        sha1_init(&h);
        sha1_update(&h, pool, sizeof(pool));
        sha1_update(&h, mix, sizeof(mix));
        sha1_final(&h, pool);
        uint32_t k = n < 16 ? n : 16; /* 4 Byte des Pools bleiben geheim */
        memcpy(out, pool, k);
        out += k;
        n -= k;
    }
}

/* ---------- Senden ---------- */

static int txq_alloc(TxQ *q, uint8_t tid)
{
    if (!q->tfds) {
        q->tfds = iwl_dma(TXQ_SIZE * TFD_SIZE);
        q->buf = iwl_dma(TXQ_SIZE * TXQ_SLOT);
        q->bc = iwl_dma(4096);
        if (!q->tfds || !q->buf || !q->bc) {
            q->tfds = 0;
            return -5;
        }
    }
    memset(q->tfds, 0, TXQ_SIZE * TFD_SIZE);
    memset(q->bc, 0, 4096);
    ScdQueueCmd c;
    memset(&c, 0, sizeof(c));
    c.operation = 0; /* IWL_SCD_QUEUE_ADD */
    c.sta_mask = 1u << STA_ID;
    c.tid = tid;
    c.cb_size = 3; /* ld(64) - 3 */
    c.bc_dram_addr = (uint64_t)q->bc;
    c.tfdq_dram_addr = (uint64_t)q->tfds;
    uint8_t r[8];
    int n = iwl_cmd(GRP_DATA_PATH, CMD_SCD_QUEUE_CFG, &c, sizeof(c), tid == TID_MGMT ? "Warteschlange Verwaltung" :
                    "Warteschlange Daten", r, sizeof(r));
    if (n < 6)
        return -6;
    mutex_lock(&iwl_ring_lock);
    q->qid = get16le(r);
    q->write = get16le(r + 4) & 0xFF;
    q->inflight = 0;
    q->last_ms = time_ms();
    mutex_unlock(&iwl_ring_lock);
    kprintf("wlan: Warteschlange %d fuer TID %u, Schreibzeiger %u\n", q->qid, tid, q->write);
    return 0;
}

/* Rahmen in eine Warteschlange stellen (mit iwl_ring_lock): 802.11-Kopf (hlen Byte), Nutzdaten aus zwei Teilen */
static int txq_send(TxQ *q, const uint8_t *hdr, uint32_t hlen, const uint8_t *p1, uint32_t l1, const uint8_t *p2,
                    uint32_t l2, uint32_t flags, uint32_t rate)
{
    if (q->qid < 0)
        return -1;
    if (q->inflight >= TXQ_SIZE - 8) {
        if (time_ms() - q->last_ms < 2000)
            return -1; /* voll */
        kprintf("wlan: Warteschlange %d: %u Rahmen ohne Antwort seit 2 s - Zaehler zurueckgesetzt\n", q->qid,
                q->inflight);
        q->inflight = 0;
    }
    uint32_t pad = hlen & 3 ? 4 - (hlen & 3) : 0, flen = hlen + l1 + l2, total = 4 + sizeof(TxCmd) + hlen + pad + l1 + l2;
    if (total > TXQ_SLOT)
        return -1;
    uint32_t idx = q->write & (TXQ_SIZE - 1);
    uint8_t *c = q->buf + idx * TXQ_SLOT, *tfd = q->tfds + idx * TFD_SIZE;
    uint16_t seq = (uint16_t)((q->qid & 0x7F) << 8 | idx);
    c[0] = CMD_TX;
    c[1] = 0;
    c[2] = (uint8_t)seq;
    c[3] = (uint8_t)(seq >> 8);
    TxCmd *t = (TxCmd *)(c + 4);
    memset(t, 0, sizeof(*t));
    t->len = (uint16_t)flen;
    t->offload_assist = (uint16_t)((hlen / 2) << 8 | (pad ? 1u << 13 : 0)); /* Kopflaenge in Worten, Auffuellung */
    t->flags = flags;
    t->rate_n_flags = rate;
    uint8_t *p = c + 4 + sizeof(TxCmd);
    memcpy(p, hdr, hlen);
    memset(p + hlen, 0, pad);
    p += hlen + pad;
    memcpy(p, p1, l1);
    memcpy(p + l1, p2, l2);
    memset(tfd, 0, TFD_SIZE);
    uint64_t a0 = (uint64_t)c, a1 = (uint64_t)c + 20;
    uint32_t len1 = total - 20;
    tfd[0] = 2;
    tfd[2] = 20;
    memcpy(tfd + 4, &a0, 8);
    tfd[12] = (uint8_t)len1;
    tfd[13] = (uint8_t)(len1 >> 8);
    memcpy(tfd + 14, &a1, 8);
    q->bc[idx] = (uint16_t)((flen + 3) / 4); /* Worte; Bits 12..15: TFD in einem 64-Byte-Stueck */
    q->write = (q->write + 1) & 0xFF;
    q->inflight++;
    sta.st.tx_frames++;
    __sync_synchronize();
    iwl_wr(HBUS_TARG_WRPTR, q->write | (uint32_t)q->qid << 16);
    return 0;
}

void iwl_sta_tx_resp(uint16_t seq, const uint8_t *d, uint32_t len)
{
    int qid = (seq >> 8) & 0x7F;
    TxQ *q = qid == mgmtq.qid ? &mgmtq : qid == dataq.qid ? &dataq : 0;
    if (q) {
        if (q->inflight)
            q->inflight--;
        q->last_ms = time_ms();
    }
    if (len < 42)
        return;
    uint32_t status = get16le(d + 40) & 0xFF; /* agg_tx_status nach dem festen Teil (iwl_mvm_tx_resp v6) */
    if (status != 1 && status != 2) {
        sta.st.tx_failed++;
        if (sta.st.tx_failed <= 20 || q == &mgmtq)
            kprintf("wlan: Rahmen %#x (FC %04x, Warteschlange %d) nicht angekommen: Status %#x nach %u Versuchen\n", seq,
                    get16le(d + 34), qid, status, d[3] + 1u);
    }
}

static void hdr80211(uint8_t *h, uint8_t fc0, uint8_t fc1, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3)
{
    h[0] = fc0;
    h[1] = fc1;
    h[2] = h[3] = 0;
    memcpy(h + 4, a1, 6);
    memcpy(h + 10, a2, 6);
    memcpy(h + 16, a3, 6);
    uint16_t sc = (uint16_t)(sta.seq++ << 4);
    h[22] = (uint8_t)sc;
    h[23] = (uint8_t)(sc >> 8);
}

static int send_mgmt(uint8_t fc0, const uint8_t *body, uint32_t len)
{
    uint8_t h[24];
    const uint8_t *mac = iwl_state()->mac;
    mutex_lock(&iwl_ring_lock);
    hdr80211(h, fc0, 0, sta.net.bssid, mac, sta.net.bssid);
    int r = txq_send(&mgmtq, h, 24, body, len, 0, 0, TX_FLAGS_CMD_RATE | TX_FLAGS_ENCRYPT_DIS | TX_FLAGS_HIGH_PRI,
                     sta.rate_mgmt);
    mutex_unlock(&iwl_ring_lock);
    return r;
}

/* EAPOL an den AP: verschluesselt, sobald der Paarschluessel eingebaut ist */
static int send_eapol(const uint8_t *e, uint32_t len)
{
    static const uint8_t llc[8] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x88, 0x8E};
    uint8_t h[24];
    const uint8_t *mac = iwl_state()->mac;
    mutex_lock(&iwl_ring_lock);
    int enc = sta.keys_on;
    hdr80211(h, 0x08, (uint8_t)(0x01 | (enc ? 0x40 : 0)), sta.net.bssid, mac, sta.net.bssid);
    int r = txq_send(&dataq, h, 24, llc, 8, e, len,
                     TX_FLAGS_CMD_RATE | TX_FLAGS_HIGH_PRI | (enc ? 0 : TX_FLAGS_ENCRYPT_DIS), sta.rate_mgmt);
    mutex_unlock(&iwl_ring_lock);
    return r;
}

/* ---------- Empfangen (mit iwl_ring_lock, aus der Auswertung des Empfangsrings) ---------- */

int iwl_sta_wants_rx(void)
{
    return sta.state == WLAN_ST_CONNECTING || sta.state == WLAN_ST_CONNECTED;
}

static void link_lost(const char *why, uint16_t reason)
{
    if (sta.state == WLAN_ST_CONNECTED)
        say("Verbindung zu '%s' getrennt: %s (Grund %u)", sta.net.ssid, why, reason);
    sta.st.reason = reason;
    sta.state = WLAN_ST_IDLE;
    sta.keys_on = 0;
}

static void rx_mgmt(const uint8_t *f, uint32_t len, const uint8_t *d)
{
    uint8_t sub = f[0] >> 4;
    const uint8_t *body = f + 24;
    uint32_t blen = len - 24;
    if (memcmp(f + 16, sta.net.bssid, 6) != 0) /* BSSID */
        return;
    if (sub == 8 && blen >= 12) { /* Beacon: Zeitstempel und DTIM-Zaehler fuer den MAC-Kontext */
        sta.beacon_tsf = 0;
        for (int i = 7; i >= 0; i--)
            sta.beacon_tsf = sta.beacon_tsf << 8 | body[i];
        sta.beacon_gp2 = iwl_le32(d + 36);
        for (uint32_t o = 12; o + 2 <= blen; o += 2u + body[o + 1])
            if (body[o] == 5 && body[o + 1] >= 2 && o + 4 <= blen)
                sta.dtim_count = body[o + 2];
        sta.beacon_seen = 1;
        return;
    }
    if (memcmp(f + 4, iwl_state()->mac, 6) != 0 && !(f[4] & 1))
        return;
    if (sub == 11 && blen >= 6 && get16le(body + 2) == 2) { /* Authentifizierung, Schritt 2 */
        sta.auth_status = get16le(body + 4);
        sta.auth_got = 1;
    } else if ((sub == 1 || sub == 3) && blen >= 6) { /* (Re-)Assoziierungsantwort */
        sta.assoc_status = get16le(body + 2);
        sta.assoc_aid = get16le(body + 4) & 0x3FFF;
        sta.assoc_got = 1;
    } else if ((sub == 12 || sub == 10) && blen >= 2) { /* Deauthentifizierung, Disassoziierung */
        uint16_t reason = get16le(body);
        kprintf("wlan: %s vom AP, Grund %u\n", sub == 12 ? "Deauthentifizierung" : "Disassoziierung", reason);
        sta.st.reason = reason;
        sta.kicked = 1; /* wer beim Verbinden wartet, bricht ab */
        if (sta.state == WLAN_ST_CONNECTED)
            link_lost(sub == 12 ? "Deauthentifizierung" : "Disassoziierung", reason);
    }
}

static void rx_data(const uint8_t *f, uint32_t len, uint32_t status, int pad)
{
    uint8_t fc0 = f[0], fc1 = f[1], sub = fc0 >> 4;
    if (sub & 4) /* Null-Rahmen ohne Daten */
        return;
    int qos = (sub & 8) != 0;
    uint32_t hl = 24 + (qos ? 2u : 0) + (qos && (fc1 & 0x80) ? 4u : 0);
    if ((fc1 & 3) != 2 || memcmp(f + 10, sta.net.bssid, 6) != 0) /* nur vom AP (FromDS) */
        return;
    if (len < hl + pad + 8 || (fc1 & 0x04) || (get16le(f + 22) & 0xF)) { /* zu kurz, Fragmente */
        sta.st.rx_dropped++;
        return;
    }
    if (status & RX_DUPLICATE)
        return;
    const uint8_t *da = f + 4, *sa = f + 16, *b = f + hl + pad;
    uint32_t bl = len - hl - pad;
    int eapol_only = 0;
    if (fc1 & 0x40) { /* verschluesselt: die Firmware hat entschluesselt und den MIC geprueft, der CCMP-Kopf bleibt */
        if ((status & RX_SEC_MASK) != RX_SEC_CCM || !(status & RX_MIC_OK) || bl < 16) {
            sta.st.rx_dropped++;
            return;
        }
        uint64_t pn = (uint64_t)b[0] | (uint64_t)b[1] << 8 | (uint64_t)b[4] << 16 | (uint64_t)b[5] << 24 |
                      (uint64_t)b[6] << 32 | (uint64_t)b[7] << 40;
        uint64_t *next = da[0] & 1 ? &sta.pn_next_mc[b[3] >> 6] : &sta.pn_next_uc;
        if (pn < *next) { /* schon gesehen: nicht noch einmal annehmen */
            sta.st.rx_dropped++;
            return;
        }
        *next = pn + 1;
        b += 8;
        bl -= 8;
    } else if (sta.keys_on) {
        eapol_only = 1; /* unverschluesselt trotz Schluessel: nur EAPOL */
    }
    if (qos && (f[24] & 0x80)) { /* A-MSDU (bekommen wir ohne QoS eigentlich nicht) */
        sta.st.rx_dropped++;
        return;
    }
    if (bl < 8 || b[0] != 0xAA || b[1] != 0xAA || b[2] != 0x03 || b[3] || b[4] || b[5]) {
        sta.st.rx_dropped++;
        return;
    }
    uint16_t type = (uint16_t)(b[6] << 8 | b[7]);
    if (type == 0x888E) {
        if (bl - 8 <= EAPOL_MAX) {
            memcpy(sta.eapol, b + 8, bl - 8);
            __sync_synchronize();
            sta.eapol_len = bl - 8;
            event_signal(&worker_ev);
        }
        return;
    }
    if (eapol_only || sta.state != WLAN_ST_CONNECTED || memcmp(sa, iwl_state()->mac, 6) == 0)
        return; /* eigene Broadcasts kommen vom AP zurueck */
    uint32_t elen = 14 + bl - 8;
    if (!rxq || elen > RXQ_SLOT - 2 || rxq_count == RXQ_LEN) {
        sta.st.rx_dropped++;
        return;
    }
    uint8_t *e = rxq + ((rxq_head + rxq_count) % RXQ_LEN) * RXQ_SLOT;
    e[0] = (uint8_t)elen;
    e[1] = (uint8_t)(elen >> 8);
    memcpy(e + 2, da, 6);
    memcpy(e + 8, sa, 6);
    memcpy(e + 14, b + 6, bl - 6); /* Ethertype und Daten */
    rxq_count++;
    sta.st.rx_frames++;
}

void iwl_sta_rx_mpdu(const uint8_t *d, uint32_t len)
{
    if (len < RX_DESC + 24)
        return;
    uint32_t mpdu_len = get16le(d), status = iwl_le32(d + 12);
    int pad = d[3] & RX_MFLG2_PAD ? 2 : 0;
    if (!(status & RX_CRC_OK) || RX_DESC + mpdu_len > len || mpdu_len < 24)
        return;
    const uint8_t *f = d + RX_DESC;
    uint32_t type = (f[0] >> 2) & 3;
    if (type == 0)
        rx_mgmt(f, mpdu_len, d);
    else if (type == 2)
        rx_data(f, mpdu_len, status, pad);
}

void iwl_sta_fw_reset(void)
{
    mutex_lock(&iwl_ring_lock);
    if (sta.state == WLAN_ST_CONNECTED)
        say("Firmware wird neu geladen - Verbindung getrennt");
    sta.state = WLAN_ST_IDLE;
    sta.keys_on = 0;
    sta.joined = 0;
    mgmtq.qid = dataq.qid = -1;
    rxq_head = rxq_count = 0;
    sta.eapol_len = 0;
    mutex_unlock(&iwl_ring_lock);
}

/* ---------- Befehle beim Verbinden (mit iwl_op_lock) ---------- */

static int add_phy(void)
{
    PhyCtxtCmd c;
    memset(&c, 0, sizeof(c));
    c.id_and_color = PHY_ID;
    c.action = ACTION_ADD;
    c.channel = sta.net.channel;
    c.band = sta.band;
    c.width = 0;    /* 20 MHz */
    c.ctrl_pos = 0;
    c.lmac_id = 0;  /* ohne CDB immer LMAC 0 */
    if (iwl_cmd(GRP_LONG, CMD_PHY_CONTEXT, &c, sizeof(c), "PHY_CONTEXT", 0, 0) < 0)
        return -1;
    /* beide Empfangsketten (Diversity), wie Linux bei nur einem Datenstrom */
    uint32_t rx = iwl_state()->nvm_rx_chains ? iwl_state()->nvm_rx_chains : 3;
    RlcCmd rl;
    memset(&rl, 0, sizeof(rl));
    rl.phy_id = PHY_ID;
    rl.rx_chain_info = rx << 1 | 2u << 10 | 2u << 12; /* gueltige Ketten, aktiv im Leerlauf, aktiv bei MIMO */
    return iwl_cmd(GRP_DATA_PATH, CMD_RLC_CONFIG, &rl, sizeof(rl), "RLC_CONFIG", 0, 0) < 0 ? -1 : 0;
}

static void ac_param(AcQos *a, uint16_t cwmin, uint16_t cwmax, uint8_t aifs, uint8_t fifo, uint16_t txop_us)
{
    a->cw_min = cwmin;
    a->cw_max = cwmax;
    a->aifsn = aifs;
    a->fifos_mask = (uint8_t)(1u << fifo);
    a->edca_txop = txop_us;
}

static int mac_ctxt(uint32_t action, int assoc)
{
    MacCtxtCmd c;
    memset(&c, 0, sizeof(c));
    c.id_and_color = MAC_ID;
    c.action = action;
    c.mac_type = FW_MAC_TYPE_BSS_STA;
    c.tsf_id = 0;
    memcpy(c.node_addr, iwl_state()->mac, 6);
    memcpy(c.bssid_addr, sta.net.bssid, 6);
    /* Raten fuer ACK/CTS: CCK 1-11 auf 2,4 GHz, OFDM 6/12/24 */
    c.cck_rates = sta.band == 1 ? 0xF : 0;
    c.ofdm_rates = 0x15;
    c.cck_short_preamble = sta.band == 1 && (sta.bss.cap & 0x20) ? MAC_FLG_SHORT_PREAMBLE : 0;
    c.short_slot = sta.band == 0 || (sta.bss.cap & 0x400) ? MAC_FLG_SHORT_SLOT : 0;
    c.filter_flags = MAC_FILTER_ACCEPT_GRP | (assoc ? 0 : MAC_FILTER_IN_BEACON);
    /* Zugriff ohne QoS (DCF) wie mac80211: alle Klassen AIFSN 2, CW 15..1023; Reihenfolge BE, BK, VI, VO;
     * FIFOs der Familie 22000: BK 1, BE 2, VI 3, VO 4 */
    ac_param(&c.ac[0], 15, 1023, 2, 2, 0);
    ac_param(&c.ac[1], 15, 1023, 2, 1, 0);
    ac_param(&c.ac[2], 15, 1023, 2, 3, 0);
    ac_param(&c.ac[3], 15, 1023, 2, 4, 0);
    uint32_t bi = sta.bss.beacon_int ? sta.bss.beacon_int : 100;
    c.bi = bi;
    c.dtim_interval = bi * sta.bss.dtim_period;
    c.listen_interval = 10;
    if (assoc) {
        uint64_t offs = (uint64_t)sta.dtim_count * bi * 1024; /* bis zum naechsten DTIM-Beacon, in us */
        c.is_assoc = 1;
        c.assoc_id = sta.assoc_aid;
        c.dtim_tsf = sta.beacon_tsf + offs;
        c.dtim_time = (uint32_t)(sta.beacon_gp2 + offs);
        c.assoc_beacon_arrive_time = sta.beacon_gp2;
    }
    return iwl_cmd(GRP_LONG, CMD_MAC_CONTEXT, &c, sizeof(c), assoc ? "MAC_CONTEXT (assoziiert)" : "MAC_CONTEXT", 0, 0) < 0
               ? -1 : 0;
}

static int add_binding(void)
{
    BindingCmd c;
    memset(&c, 0, sizeof(c));
    c.id_and_color = PHY_ID;
    c.action = ACTION_ADD;
    c.macs[0] = MAC_ID;
    c.macs[1] = c.macs[2] = FW_CTXT_INVALID;
    c.phy = PHY_ID;
    c.lmac_id = 0;
    uint8_t r[4] = {0};
    int n = iwl_cmd(GRP_LONG, CMD_BINDING, &c, sizeof(c), "BINDING_CONTEXT", r, sizeof(r));
    if (n >= 4 && iwl_le32(r) != 0)
        kprintf("wlan: Bindung: Status %#x\n", iwl_le32(r));
    return n < 0 || (n >= 4 && iwl_le32(r) != 0) ? -1 : 0;
}

static int add_sta(int modify)
{
    AddStaCmd c;
    memset(&c, 0, sizeof(c));
    c.add_modify = (uint8_t)modify;
    c.tid_disable_tx = 0xFFFF; /* keine Aggregation */
    c.mac_id_n_color = MAC_ID;
    memcpy(c.addr, sta.net.bssid, 6);
    c.sta_id = STA_ID;
    c.station_flags = 0;       /* 20 MHz, ein Datenstrom */
    c.station_flags_msk = STA_FLG_FAT_EN_MSK | STA_FLG_MIMO_EN_MSK | STA_FLG_RTS_MIMO_PROT;
    c.station_type = 0;        /* IWL_STA_LINK */
    c.assoc_id = modify ? sta.assoc_aid : 0;
    uint8_t r[4] = {0};
    int n = iwl_cmd(GRP_LONG, CMD_ADD_STA, &c, sizeof(c), modify ? "ADD_STA (assoziiert)" : "ADD_STA", r, sizeof(r));
    if (n >= 4 && (iwl_le32(r) & 0xFF) != ADD_STA_SUCCESS)
        kprintf("wlan: ADD_STA: Status %#x\n", iwl_le32(r));
    return n < 4 || (iwl_le32(r) & 0xFF) != ADD_STA_SUCCESS ? -1 : 0;
}

static int protect_session(uint32_t tu)
{
    SessionProtCmd c;
    memset(&c, 0, sizeof(c));
    c.id_and_color = MAC_ID;
    c.action = ACTION_ADD;
    c.conf_id = 0; /* SESSION_PROTECT_CONF_ASSOC */
    c.duration_tu = tu;
    iwl_expect_notif(GRP_MAC_CONF, NOTIF_SESSION_PROT);
    if (iwl_cmd(GRP_MAC_CONF, CMD_SESSION_PROT, &c, sizeof(c), "SESSION_PROTECTION", 0, 0) < 0)
        return -1;
    uint8_t n[16];
    int r = iwl_wait_notif(GRP_MAC_CONF, NOTIF_SESSION_PROT, 1500, "Zeitfenster beginnt", n, sizeof(n));
    if (r < 12)
        return -1;
    if (!iwl_le32(n + 4) || !iwl_le32(n + 8)) { /* Status, Beginn */
        kprintf("wlan: Zeitfenster: Status %u, Beginn %u\n", iwl_le32(n + 4), iwl_le32(n + 8));
        return -1;
    }
    return 0;
}

static int install_key(int pairwise, const uint8_t *key, int keyidx)
{
    AddStaKeyCmd c;
    memset(&c, 0, sizeof(c));
    c.sta_id = STA_ID;
    c.key_offset = (uint8_t)(pairwise ? 0 : keyidx ? keyidx : 4); /* Platz in der Schluesseltabelle der Firmware */
    c.key_flags = (uint16_t)(STA_KEY_FLG_CCM | STA_KEY_FLG_KEY_MAP | STA_KEY_FLG_KEYID(keyidx) |
                             (pairwise ? 0 : STA_KEY_MULTICAST));
    memcpy(c.key, key, 16);
    uint8_t r[4] = {0};
    int n = iwl_cmd(GRP_LONG, CMD_ADD_STA_KEY, &c, sizeof(c), pairwise ? "ADD_STA_KEY (paarweise)" :
                    "ADD_STA_KEY (Gruppe)", r, sizeof(r));
    if (n >= 4 && (iwl_le32(r) & 0xFF) != ADD_STA_SUCCESS)
        kprintf("wlan: ADD_STA_KEY: Status %#x\n", iwl_le32(r));
    return n < 4 || (iwl_le32(r) & 0xFF) != ADD_STA_SUCCESS ? -1 : 0;
}

/* Wartenden EAPOL-Rahmen auswerten, antworten, Schluessel einbauen. 1 = etwas getan, -1 Fehler */
static int handle_eapol(void)
{
    mutex_lock(&iwl_ring_lock);
    uint32_t n = sta.eapol_len;
    if (n)
        memcpy(eapol_tmp, sta.eapol, n);
    sta.eapol_len = 0;
    mutex_unlock(&iwl_ring_lock);
    if (!n)
        return 0;
    if (!sta.secure)
        return 0;
    int was_done = sta.wpa.done;
    int r = wpa_rx(&sta.wpa, eapol_tmp, n, eapol_out, sizeof(eapol_out));
    if (r < 0) {
        say("EAPOL-Rahmen abgelehnt: %s", sta.wpa.err);
        return 0; /* ein verfaelschter oder alter Rahmen beendet nichts; der AP wiederholt */
    }
    if (r > 0 && send_eapol(eapol_out, (uint32_t)r) != 0)
        kprintf("wlan: EAPOL-Antwort nicht gesendet\n");
    if (sta.wpa.events & WPA_EV_PTK) {
        if (install_key(1, sta.wpa.ptk + 32, 0) != 0)
            return -1;
        mutex_lock(&iwl_ring_lock);
        sta.keys_on = 1;
        sta.pn_next_uc = 0;
        mutex_unlock(&iwl_ring_lock);
    }
    if (sta.wpa.events & WPA_EV_GTK) {
        if (sta.wpa.gtk_len != 16) {
            say("Gruppenschluessel mit %u Byte (nur CCMP-128)", sta.wpa.gtk_len);
            return -1;
        }
        if (install_key(0, sta.wpa.gtk, sta.wpa.gtk_idx) != 0)
            return -1;
        uint64_t rsc = 0;
        for (int i = 5; i >= 0; i--)
            rsc = rsc << 8 | sta.wpa.gtk_rsc[i];
        mutex_lock(&iwl_ring_lock);
        sta.pn_next_mc[sta.wpa.gtk_idx & 3] = rsc;
        mutex_unlock(&iwl_ring_lock);
        if (was_done) {
            sta.st.rekeys++;
            kprintf("wlan: neuer Gruppenschluessel (Nummer %d)\n", sta.wpa.gtk_idx);
        }
    }
    sta.wpa.events = 0;
    return 1;
}

/* Bis flag gesetzt ist, den Empfang auswerten; 0, -1 Zeit um, -2 Firmware-Fehler, -3 vom AP abgemeldet */
static int wait_flag(volatile int *flag, uint32_t ms)
{
    uint64_t t0 = time_ms();
    while (!*flag) {
        iwl_poll();
        if (*flag)
            break;
        if (sta.kicked)
            return -3;
        if (iwl_fw_failed())
            return -2;
        if (time_ms() - t0 > ms)
            return -1;
        thread_sleep_ms(1);
    }
    return 0;
}

/* ---------- Netz auswaehlen ---------- */

/* RSN-Element des AP pruefen: Gruppe CCMP, paarweise CCMP, AKM PSK, kein Pflicht-MFP. Eigenes Element bauen. */
static int rsn_choose(char *why, uint32_t why_len)
{
    const uint8_t *v = sta.bss.rsn + 2;
    uint32_t l = sta.bss.rsn_len >= 2 ? sta.bss.rsn[1] : 0, o = 2;
    static const uint8_t ccmp[4] = {0x00, 0x0F, 0xAC, 4}, psk[4] = {0x00, 0x0F, 0xAC, 2};
    if (l < 2 || get16le(v) != 1) {
        ksnprintf(why, why_len, "RSN-Element fehlt oder unbekannte Version");
        return -1;
    }
    const uint8_t *group = o + 4 <= l ? v + o : ccmp;
    o += 4;
    int pw_ok = o + 2 > l; /* fehlt die Liste, gilt CCMP */
    if (o + 2 <= l) {
        uint32_t n = get16le(v + o);
        o += 2;
        for (uint32_t i = 0; i < n && o + 4 <= l; i++, o += 4)
            pw_ok |= memcmp(v + o, ccmp, 4) == 0;
    }
    int akm_ok = 0;
    if (o + 2 <= l) {
        uint32_t n = get16le(v + o);
        o += 2;
        for (uint32_t i = 0; i < n && o + 4 <= l; i++, o += 4)
            akm_ok |= memcmp(v + o, psk, 4) == 0;
    }
    uint16_t caps = o + 2 <= l ? get16le(v + o) : 0;
    if (memcmp(group, ccmp, 4) != 0) {
        ksnprintf(why, why_len, "Gruppenschluessel mit %02x-%02x-%02x-%u (nur CCMP; TKIP nicht)", group[0], group[1],
                  group[2], group[3]);
        return -1;
    }
    if (!pw_ok) {
        ksnprintf(why, why_len, "AP bietet kein CCMP fuer Paarschluessel");
        return -1;
    }
    if (!akm_ok) {
        ksnprintf(why, why_len, "kein WPA2-PSK (nur WPA3/SAE oder 802.1X/Enterprise)");
        return -1;
    }
    if (caps & 0x40) {
        ksnprintf(why, why_len, "AP verlangt Management Frame Protection (802.11w)");
        return -1;
    }
    static const uint8_t ie[22] = {0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                   1,    0,  0x00, 0x0F, 0xAC, 2, 0, 0};
    memcpy(sta.rsn_ie, ie, sizeof(ie));
    sta.rsn_ie_len = sizeof(ie);
    return 0;
}

/* staerkster AP mit der SSID (und ggf. BSSID) aus der letzten Suche */
static int find_net(const WlanConnect *c)
{
    static const uint8_t zero[6] = {0};
    int best = -1, best_sig = -1000;
    WlanNet n;
    IwlBss x;
    uint32_t sl = (uint32_t)strlen(c->ssid);
    for (unsigned i = 0; iwl_scan_bss(i, &n, &x) == 0; i++) {
        if (n.ssid_len != sl || memcmp(n.ssid, c->ssid, sl) != 0)
            continue;
        if (memcmp(c->bssid, zero, 6) != 0 && memcmp(c->bssid, n.bssid, 6) != 0)
            continue;
        if (n.signal > best_sig) {
            best = (int)i;
            best_sig = n.signal;
            sta.net = n;
            sta.bss = x;
        }
    }
    return best;
}

/* Feste Senderate: Verwaltung 1 Mbit/s (2,4 GHz) bzw. 6 Mbit/s; Daten nach der Signalstaerke */
static void choose_rates(void)
{
    static const uint16_t ofdm_kbps[8] = {6000, 9000, 12000, 18000, 24000, 36000, 48000, 54000};
    uint32_t ant = (uint32_t)sta.ant << RATE_ANT_POS;
    sta.rate_mgmt = sta.band == 1 ? ant : RATE_OFDM | ant;
    int s = sta.net.signal, r = s >= -55 ? 7 : s >= -62 ? 5 : s >= -70 ? 4 : s >= -77 ? 2 : 0;
    sta.rate_data = RATE_OFDM | ant | (uint32_t)r;
    sta.st.rate_kbps = ofdm_kbps[r];
}

/* Datenraten fuer die Assoc-Anfrage: die des AP, ohne die "BSS Membership Selectors" (HT/VHT/HE/SAE-Pflicht) */
static uint32_t put_rates(uint8_t *p)
{
    uint8_t r[16];
    uint32_t n = 0, o = 0;
    int need_ht = 0;
    for (uint32_t i = 0; i < sta.bss.n_rates; i++) {
        uint8_t v = sta.bss.rates[i];
        if ((v & 0x7F) >= 121) {
            need_ht |= v == 0xFF;
            continue;
        }
        r[n++] = v;
    }
    if (need_ht)
        kprintf("wlan: Achtung: der AP verlangt 802.11n (HT) - das koennen wir noch nicht\n");
    if (!n) { /* keine Raten bekannt: die Pflichtraten */
        static const uint8_t g[8] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24}, a[8] = {0x8C, 0x12, 0x98, 0x24,
                                                                                            0xB0, 0x48, 0x60, 0x6C};
        memcpy(r, sta.band == 1 ? g : a, 8);
        n = 8;
    }
    uint32_t k = n > 8 ? 8 : n;
    p[o++] = 1;
    p[o++] = (uint8_t)k;
    memcpy(p + o, r, k);
    o += k;
    if (n > k) {
        p[o++] = 50;
        p[o++] = (uint8_t)(n - k);
        memcpy(p + o, r + k, n - k);
        o += n - k;
    }
    return o;
}

/* ---------- Verbinden ---------- */

static int fail(int step, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(int step, int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(sta.st.msg, sizeof(sta.st.msg), fmt, ap);
    va_end(ap);
    kprintf("wlan: Verbinden gescheitert (%s): %s\n", step_name[step], sta.st.msg);
    sta.st.step = (uint32_t)step;
    sta.st.error = err;
    sta.state = WLAN_ST_FAILED;
    sta.keys_on = 0;
    return err;
}

static void deauth(uint16_t reason)
{
    uint8_t b[2] = {(uint8_t)reason, (uint8_t)(reason >> 8)};
    if (mgmtq.qid >= 0)
        send_mgmt(0xC0, b, 2);
}

static void wlan_worker(void *arg);

static int connect_op(const WlanConnect *c)
{
    uint64_t t0 = time_ms();
    const WlanInfo *wi = iwl_state();
    memset(&sta.st, 0, sizeof(sta.st));
    /* 1. Firmware frisch laden, wenn sie schon Kontexte hat (alte Verbindung) oder noch nicht laeuft */
    sta.st.step = WLAN_STEP_FW;
    if (sta.joined || wi->init_step != 9) {
        if (sta.state == WLAN_ST_CONNECTED)
            deauth(3); /* verlaesst das Netz */
        int r = iwl_load_fw_op();
        if (r < 0)
            return fail(WLAN_STEP_FW, r, "Firmware startet nicht (%d, Schritt %u)", r, wi->init_step);
    }
    net_set_mac("wlan0", wi->mac); /* die MAC-Adresse kennt erst die Firmware */
    /* 2. Netz aus der letzten Suche, sonst suchen */
    sta.st.step = WLAN_STEP_SCAN;
    if (find_net(c) < 0) {
        kprintf("wlan: '%s' nicht in der letzten Suche - suche ...\n", c->ssid);
        int r = iwl_scan_op();
        if (r < 0)
            return fail(WLAN_STEP_SCAN, r, "Suche fehlgeschlagen (%d)", r);
        if (find_net(c) < 0)
            return fail(WLAN_STEP_SCAN, -10, "Netz '%s' nicht gefunden", c->ssid);
    }
    memcpy(sta.st.ssid, sta.net.ssid, sizeof(sta.st.ssid));
    memcpy(sta.st.bssid, sta.net.bssid, 6);
    sta.st.channel = sta.net.channel;
    sta.st.signal = sta.net.signal;
    sta.st.security = sta.net.security;
    sta.band = sta.net.channel <= 14 ? 1 : 0;
    sta.ant = wi->nvm_tx_chains & 1 ? 1 : 2;
    choose_rates();
    kprintf("wlan: verbinde mit '%s' (%02x:%02x:%02x:%02x:%02x:%02x, Kanal %u, %d dBm, Beacon %u TU, DTIM %u), "
            "Daten mit %u Mbit/s\n", sta.net.ssid, sta.net.bssid[0], sta.net.bssid[1], sta.net.bssid[2],
            sta.net.bssid[3], sta.net.bssid[4], sta.net.bssid[5], sta.net.channel, sta.net.signal, sta.bss.beacon_int,
            sta.bss.dtim_period, sta.st.rate_kbps / 1000);

    /* 3. Verschluesselung: offen oder WPA2-PSK (auch WPA2/WPA3 gemischt) */
    sta.st.step = WLAN_STEP_PMK;
    sta.secure = 0;
    uint8_t pmk[32];
    char why[80];
    switch (sta.net.security) {
    case WLAN_SEC_OPEN:
        break;
    case WLAN_SEC_WPA2:
    case WLAN_SEC_WPA2_3:
        if (rsn_choose(why, sizeof(why)) != 0)
            return fail(WLAN_STEP_PMK, -13, "%s", why);
        if (wpa_pmk(c->pass, (const uint8_t *)sta.net.ssid, sta.net.ssid_len, pmk) != 0)
            return fail(WLAN_STEP_PMK, -11, "Passwort muss 8 bis 63 Zeichen haben (oder 64 Hex-Zeichen)");
        sta.secure = 1;
        break;
    default:
        return fail(WLAN_STEP_PMK, -13, "Verschluesselung %s wird nicht unterstuetzt",
                    sta.net.security == WLAN_SEC_WEP ? "WEP" : sta.net.security == WLAN_SEC_WPA ? "WPA (1)" :
                    "WPA3 (SAE)");
    }
    if (!rxq && !(rxq = iwl_dma(RXQ_LEN * RXQ_SLOT)))
        return fail(WLAN_STEP_CONTEXT, -5, "kein Speicher");

    mutex_lock(&iwl_ring_lock);
    sta.state = WLAN_ST_CONNECTING; /* ab jetzt gehen empfangene Rahmen an uns */
    sta.keys_on = 0;
    sta.auth_got = sta.assoc_got = 0;
    sta.beacon_seen = 0;
    sta.eapol_len = 0;
    sta.kicked = 0;
    sta.pn_next_uc = 0;
    memset(sta.pn_next_mc, 0, sizeof(sta.pn_next_mc));
    rxq_head = rxq_count = 0;
    mutex_unlock(&iwl_ring_lock);

    /* 4. Kontexte: PHY (Kanal), MAC (Station mit BSSID), Bindung */
    sta.st.step = WLAN_STEP_CONTEXT;
    sta.joined = 1;
    if (add_phy() != 0)
        return fail(WLAN_STEP_CONTEXT, -6, "PHY-Kontext abgelehnt (Kanal %u)", sta.net.channel);
    if (mac_ctxt(ACTION_ADD, 0) != 0)
        return fail(WLAN_STEP_CONTEXT, -6, "MAC-Kontext abgelehnt");
    if (add_binding() != 0)
        return fail(WLAN_STEP_CONTEXT, -6, "Bindung MAC/PHY abgelehnt");
    /* 5. Station fuer den AP und ihre Warteschlangen */
    sta.st.step = WLAN_STEP_STATION;
    if (add_sta(0) != 0)
        return fail(WLAN_STEP_STATION, -6, "Station abgelehnt");
    sta.st.step = WLAN_STEP_QUEUES;
    if (txq_alloc(&mgmtq, TID_MGMT) != 0 || txq_alloc(&dataq, TID_DATA) != 0)
        return fail(WLAN_STEP_QUEUES, -6, "Sendewarteschlangen abgelehnt");
    /* 6. Zeitfenster auf dem Kanal */
    sta.st.step = WLAN_STEP_PROTECT;
    if (protect_session(1000) != 0)
        kprintf("wlan: kein Zeitfenster bestaetigt - versuche es trotzdem\n");

    /* 7. Authentifizierung (Open System) */
    sta.st.step = WLAN_STEP_AUTH;
    int ok = 0;
    for (int t = 0; t < 3 && !ok; t++) {
        static const uint8_t auth[6] = {0, 0, 1, 0, 0, 0}; /* Verfahren 0, Schritt 1, Status 0 */
        sta.auth_got = 0;
        sta.auth_status = 0;
        if (send_mgmt(0xB0, auth, sizeof(auth)) != 0)
            return fail(WLAN_STEP_AUTH, -6, "Senden nicht moeglich");
        int r = wait_flag(&sta.auth_got, 500);
        if (r == -2)
            return fail(WLAN_STEP_AUTH, -2, "Firmware-Fehler");
        if (r == -3)
            return fail(WLAN_STEP_AUTH, -14, "vom AP abgewiesen (Deauthentifizierung, Grund %u)", sta.st.reason);
        ok = r == 0;
        if (ok && sta.auth_status != 0)
            return fail(WLAN_STEP_AUTH, -14, "AP lehnt ab (Status %u)", sta.auth_status);
    }
    sta.st.status_code = sta.auth_status;
    if (!ok)
        return fail(WLAN_STEP_AUTH, -15, "keine Antwort des AP (3 Versuche)");
    kprintf("wlan: authentifiziert nach %u ms\n", (uint32_t)(time_ms() - t0));

    /* 8. Assoziierung: Faehigkeiten, Hoerintervall, SSID, Raten, RSN-Element */
    sta.st.step = WLAN_STEP_ASSOC;
    static uint8_t req[128];
    uint32_t n = 0;
    uint16_t cap = 0x0001; /* ESS */
    if (sta.band == 1)
        cap |= 0x0020 | 0x0400; /* kurze Praeambel, kurzer Slot */
    if (sta.bss.cap & 0x0010)
        cap |= 0x0010; /* Privacy */
    req[n++] = (uint8_t)cap;
    req[n++] = (uint8_t)(cap >> 8);
    req[n++] = 10; /* Hoerintervall in Beacons */
    req[n++] = 0;
    req[n++] = 0;
    req[n++] = sta.net.ssid_len;
    memcpy(req + n, sta.net.ssid, sta.net.ssid_len);
    n += sta.net.ssid_len;
    n += put_rates(req + n);
    if (sta.secure) {
        memcpy(req + n, sta.rsn_ie, sta.rsn_ie_len);
        n += sta.rsn_ie_len;
    }
    ok = 0;
    for (int t = 0; t < 3 && !ok; t++) {
        sta.assoc_got = 0;
        sta.assoc_status = 0;
        if (send_mgmt(0x00, req, n) != 0)
            return fail(WLAN_STEP_ASSOC, -6, "Senden nicht moeglich");
        int r = wait_flag(&sta.assoc_got, 500);
        if (r == -2)
            return fail(WLAN_STEP_ASSOC, -2, "Firmware-Fehler");
        if (r == -3)
            return fail(WLAN_STEP_ASSOC, -14, "vom AP abgewiesen (Grund %u)", sta.st.reason);
        ok = r == 0;
        if (ok && sta.assoc_status != 0)
            return fail(WLAN_STEP_ASSOC, -14, "AP lehnt ab (Status %u%s)", sta.assoc_status,
                        sta.assoc_status == 18 ? ": Datenraten" : sta.assoc_status == 27 ? ": braucht 802.11n" :
                        sta.assoc_status == 17 ? ": AP voll" : "");
    }
    sta.st.status_code = sta.assoc_status;
    if (!ok)
        return fail(WLAN_STEP_ASSOC, -15, "keine Antwort des AP (3 Versuche)");
    sta.st.aid = sta.assoc_aid;
    kprintf("wlan: assoziiert nach %u ms, AID %u%s\n", (uint32_t)(time_ms() - t0), sta.assoc_aid,
            sta.beacon_seen ? "" : " (noch kein Beacon gesehen)");
    if (add_sta(1) != 0 || mac_ctxt(ACTION_MODIFY, 1) != 0) {
        deauth(1);
        return fail(WLAN_STEP_ASSOC, -6, "Firmware nimmt die Assoziierung nicht an");
    }

    /* 9. WPA2: 4-Wege-Handshake */
    if (sta.secure) {
        sta.st.step = WLAN_STEP_KEYS;
        uint8_t snonce[32];
        random_bytes(snonce, sizeof(snonce));
        wpa_start(&sta.wpa, pmk, sta.net.bssid, wi->mac, sta.rsn_ie, sta.rsn_ie_len, snonce);
        if (protect_session(1000) != 0)
            kprintf("wlan: kein zweites Zeitfenster - weiter\n");
        uint64_t tk = time_ms();
        while (!(sta.wpa.done && sta.keys_on)) {
            iwl_poll();
            if (handle_eapol() < 0) {
                deauth(1);
                return fail(WLAN_STEP_KEYS, -6, "Schluessel nicht eingebaut");
            }
            if (iwl_fw_failed())
                return fail(WLAN_STEP_KEYS, -2, "Firmware-Fehler");
            if (sta.kicked)
                return fail(WLAN_STEP_KEYS, -14, "vom AP getrennt (Grund %u)%s", sta.st.reason,
                            sta.st.reason == 15 || sta.st.reason == 2 ? " - Passwort falsch?" : "");
            if (time_ms() - tk > 5000) {
                deauth(15);
                return fail(WLAN_STEP_KEYS, -15, "%s", sta.wpa.have_anonce ?
                            "Handshake nicht abgeschlossen - Passwort falsch?" : "AP startet den Handshake nicht");
            }
            thread_sleep_ms(1);
        }
    }
    sta.st.step = WLAN_STEP_DONE;
    sta.st.connect_ms = (uint32_t)(time_ms() - t0);
    sta.state = WLAN_ST_CONNECTED;
    say("verbunden mit '%s' nach %u ms (%s, %u Mbit/s)", sta.net.ssid, sta.st.connect_ms,
        sta.secure ? "WPA2" : "offen", sta.st.rate_kbps / 1000);
    if (!worker_started) {
        worker_started = 1;
        thread_create("wlan", wlan_worker, 0);
    }
    return 0;
}

int iwl_connect(const WlanConnect *c)
{
    if (!iwl_state()->present)
        return -1;
    if (!c->ssid[0] || strlen(c->ssid) > 32)
        return -11;
    if (sta.state == WLAN_ST_CONNECTING)
        return -12;
    mutex_lock(&iwl_op_lock);
    int r = connect_op(c);
    mutex_unlock(&iwl_op_lock);
    return r;
}

int iwl_disconnect(void)
{
    if (!iwl_state()->present)
        return -1;
    mutex_lock(&iwl_op_lock);
    if (sta.state == WLAN_ST_CONNECTED) {
        deauth(3);
        thread_sleep_ms(20); /* den Rahmen noch hinausgehen lassen */
        say("Verbindung zu '%s' getrennt", sta.net.ssid);
    }
    mutex_lock(&iwl_ring_lock);
    sta.state = WLAN_ST_IDLE;
    sta.keys_on = 0;
    mutex_unlock(&iwl_ring_lock);
    mutex_unlock(&iwl_op_lock);
    return 0;
}

void iwl_status(WlanStatus *s)
{
    *s = sta.st;
    s->state = (uint32_t)sta.state;
}

/* ---------- im Betrieb: Gruppenschluessel, Fehler der Firmware ---------- */

static void wlan_worker(void *arg)
{
    (void)arg;
    for (;;) {
        event_wait(&worker_ev, 100);
        if (sta.state != WLAN_ST_CONNECTED)
            continue;
        mutex_lock(&iwl_op_lock);
        if (sta.state == WLAN_ST_CONNECTED) {
            iwl_poll();
            if (handle_eapol() < 0)
                link_lost("Schluesselwechsel gescheitert", 0);
            if (iwl_fw_failed()) {
                link_lost("Fehler der Firmware", 0);
                sta.joined = 1; /* beim naechsten Verbinden neu laden */
            }
        }
        mutex_unlock(&iwl_op_lock);
    }
}

/* ---------- wlan0 im Netzwerk-Stack ---------- */

static int wlan_send(NetDev *d, const void *frame, uint32_t len)
{
    (void)d;
    static const uint8_t llc[6] = {0xAA, 0xAA, 0x03, 0, 0, 0};
    const uint8_t *e = frame;
    if (len < 14)
        return -1;
    mutex_lock(&iwl_ring_lock);
    int r = -1;
    if (sta.state == WLAN_ST_CONNECTED) {
        uint8_t h[24], snap[8];
        memcpy(snap, llc, 6);
        snap[6] = e[12];
        snap[7] = e[13];
        int enc = sta.keys_on;
        hdr80211(h, 0x08, (uint8_t)(0x01 | (enc ? 0x40 : 0)), sta.net.bssid, e + 6, e); /* an den AP (ToDS) */
        r = txq_send(&dataq, h, 24, snap, 8, e + 14, len - 14, TX_FLAGS_CMD_RATE | (enc ? 0 : TX_FLAGS_ENCRYPT_DIS),
                     sta.rate_data);
    }
    mutex_unlock(&iwl_ring_lock);
    return r;
}

static int wlan_recv(NetDev *d, void *buf, uint32_t max)
{
    (void)d;
    if (sta.state != WLAN_ST_CONNECTED)
        return 0;
    if (!rxq_count)
        iwl_poll();
    mutex_lock(&iwl_ring_lock);
    int r = 0;
    if (rxq_count) {
        uint8_t *e = rxq + rxq_head * RXQ_SLOT;
        uint32_t len = get16le(e);
        r = (int)(len <= max ? len : max);
        memcpy(buf, e + 2, (uint32_t)r);
        rxq_head = (rxq_head + 1) % RXQ_LEN;
        rxq_count--;
    }
    mutex_unlock(&iwl_ring_lock);
    if (sta.eapol_len)
        event_signal(&worker_ev); /* Gruppenschluessel: erledigt der Thread "wlan" */
    return r;
}

static int wlan_link(NetDev *d, uint32_t *mbps, int *full_duplex)
{
    (void)d;
    *mbps = sta.st.rate_kbps / 1000;
    *full_duplex = 0;
    return sta.state == WLAN_ST_CONNECTED;
}

void iwl_sta_register(void)
{
    NetDev d;
    memset(&d, 0, sizeof(d));
    memcpy(d.name, "wlan0", 6);
    ksnprintf(d.model, sizeof(d.model), "Intel Wi-Fi 6 AX200");
    memcpy(d.mac, iwl_state()->mac, 6); /* die echte MAC kennt erst die Firmware; ersetzt beim Verbinden */
    d.send = wlan_send;
    d.recv = wlan_recv;
    d.link = wlan_link;
    d.irq = 0;
    if (net_register(&d) != 0)
        kprintf("wlan: kein Platz fuer wlan0 im Netzwerk-Stack\n");
}
