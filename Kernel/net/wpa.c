/* WPA2-PSK: 4-Wege-Handshake und Gruppenschluessel-Wechsel (siehe wpa.h)
 *
 * EAPOL-Key-Rahmen (IEEE 802.1X + 802.11i), alle Zahlen Big-Endian:
 *   0 Version, 1 Typ (3 = Key), 2 Laenge des Rests, 4 Descriptor-Typ (2 = RSN), 5 Key Information, 7 Key Length,
 *   9 Replay-Zaehler (8), 17 Nonce (32), 49 IV (16), 65 RSC (8), 73 reserviert (8), 81 MIC (16),
 *   97 Laenge der Schluesseldaten, 99 Schluesseldaten
 * Ablauf: AP -> 1 (ANonce), Station -> 2 (SNonce, eigenes RSN-Element, MIC), AP -> 3 (Installieren, GTK verschluesselt,
 * MIC), Station -> 4 (MIC). Danach baut die Station Paar- und Gruppenschluessel ein. Spaeter schickt der AP neue
 * Gruppenschluessel (Gruppen-Nachricht 1), die Station bestaetigt (Gruppen-Nachricht 2). */

#include "net/wpa.h"
#include "lib/crypto.h"
#include "lib/kprintf.h"
#include "lib/string.h"

#define KI_VERSION   0x0007
#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ERROR     0x0400
#define KI_REQUEST   0x0800
#define KI_ENCRYPTED 0x1000

#define OFF_INFO   5
#define OFF_KLEN   7
#define OFF_REPLAY 9
#define OFF_NONCE  17
#define OFF_RSC    65
#define OFF_MIC    81
#define OFF_DLEN   97

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void set16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int wpa_pmk(const char *pass, const uint8_t *ssid, uint32_t ssid_len, uint8_t pmk[32])
{
    uint32_t n = (uint32_t)strlen(pass);
    if (n == 64) {
        for (int i = 0; i < 32; i++) {
            int h = hexval(pass[2 * i]), l = hexval(pass[2 * i + 1]);
            if (h < 0 || l < 0)
                return -1;
            pmk[i] = (uint8_t)(h << 4 | l);
        }
        return 0;
    }
    if (n < 8 || n > 63)
        return -1;
    pbkdf2_sha1(pass, n, ssid, ssid_len, 4096, pmk, 32);
    return 0;
}

void wpa_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
                    const uint8_t snonce[32], uint8_t ptk[48])
{
    static const char label[] = "Pairwise key expansion"; /* mit der abschliessenden 0 */
    uint8_t data[76];
    int a_first = memcmp(aa, spa, 6) < 0, n_first = memcmp(anonce, snonce, 32) < 0;
    memcpy(data, a_first ? aa : spa, 6);
    memcpy(data + 6, a_first ? spa : aa, 6);
    memcpy(data + 12, n_first ? anonce : snonce, 32);
    memcpy(data + 44, n_first ? snonce : anonce, 32);
    for (uint8_t i = 0; i < 3; i++) {
        const uint8_t *part[3] = {(const uint8_t *)label, data, &i};
        const uint32_t plen[3] = {sizeof(label), sizeof(data), 1};
        uint8_t h[20];
        hmac_sha1_v(pmk, 32, 3, part, plen, h);
        memcpy(ptk + 20 * i, h, i < 2 ? 20 : 8);
    }
}

void wpa_mic(const uint8_t kck[16], const uint8_t *frame, uint32_t len, uint8_t mic[16])
{
    uint8_t h[20];
    hmac_sha1(kck, 16, frame, len, h);
    memcpy(mic, h, 16);
}

void wpa_start(Wpa *w, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t *ie,
               uint32_t ie_len, const uint8_t snonce[32])
{
    memset(w, 0, sizeof(*w));
    memcpy(w->pmk, pmk, 32);
    memcpy(w->aa, aa, 6);
    memcpy(w->spa, spa, 6);
    memcpy(w->snonce, snonce, 32);
    w->ie_len = ie_len <= sizeof(w->ie) ? ie_len : sizeof(w->ie);
    memcpy(w->ie, ie, w->ie_len);
}

/* MIC eines empfangenen Rahmens mit dem KCK pruefen */
static int mic_ok(const uint8_t kck[16], const uint8_t *frame, uint32_t len)
{
    static uint8_t tmp[2048];
    if (len > sizeof(tmp))
        return 0;
    memcpy(tmp, frame, len);
    memset(tmp + OFF_MIC, 0, 16);
    uint8_t mic[16];
    wpa_mic(kck, tmp, len, mic);
    return memcmp(mic, frame + OFF_MIC, 16) == 0;
}

/* Antwort ohne Schluesseldaten bzw. mit data (Nachricht 2: eigenes RSN-Element), MIC mit dem KCK */
static int reply(Wpa *w, const uint8_t *in, uint16_t info, const uint8_t *nonce, const uint8_t *data, uint32_t dlen,
                 uint8_t *out, uint32_t max)
{
    uint32_t len = EAPOL_KEY_HDR + dlen;
    if (len > max)
        return -1;
    memset(out, 0, len);
    out[0] = 1; /* 802.1X-2001 wie wpa_supplicant: verstehen alle APs */
    out[1] = 3;
    set16(out + 2, (uint16_t)(len - 4));
    out[4] = 2;
    set16(out + OFF_INFO, info);
    memcpy(out + OFF_REPLAY, in + OFF_REPLAY, 8);
    if (nonce)
        memcpy(out + OFF_NONCE, nonce, 32);
    set16(out + OFF_DLEN, (uint16_t)dlen);
    memcpy(out + EAPOL_KEY_HDR, data, dlen);
    wpa_mic(w->ptk, out, len, out + OFF_MIC);
    return (int)len;
}

/* Schluesseldaten entschluesseln (AES Key Wrap mit dem KEK) und die GTK-KDE suchen (dd, Laenge, 00-0F-AC, 1:
 * Schluesselnummer, reserviert, GTK). 0 = GTK gefunden */
static int take_gtk(Wpa *w, const uint8_t *frame, uint32_t dlen)
{
    static uint8_t plain[512];
    if (dlen < 24 || dlen % 8 || dlen - 8 > sizeof(plain)) {
        ksnprintf(w->err, sizeof(w->err), "Schluesseldaten: Laenge %u", dlen);
        return -1;
    }
    if (aes_unwrap(w->ptk + 16, frame + EAPOL_KEY_HDR, dlen - 8, plain) != 0) {
        ksnprintf(w->err, sizeof(w->err), "Schluesseldaten: Entschluesseln fehlgeschlagen");
        return -1;
    }
    uint32_t n = dlen - 8;
    for (uint32_t o = 0; o + 2 <= n;) {
        uint8_t id = plain[o], l = plain[o + 1];
        if (id == 0xDD && l == 0)
            break; /* Auffuellung */
        if (o + 2 + l > n)
            break;
        const uint8_t *v = plain + o + 2;
        if (id == 0xDD && l >= 6 && v[0] == 0x00 && v[1] == 0x0F && v[2] == 0xAC && v[3] == 1) {
            uint32_t glen = l - 6u;
            if (glen != 16 && glen != 32) {
                ksnprintf(w->err, sizeof(w->err), "GTK mit %u Byte", glen);
                return -1;
            }
            w->gtk_idx = v[4] & 3;
            w->gtk_len = glen;
            memcpy(w->gtk, v + 6, glen);
            for (int i = 0; i < 6; i++)
                w->gtk_rsc[i] = frame[OFF_RSC + i]; /* RSC: niedrigstes Byte zuerst */
            return 0;
        }
        o += 2u + l;
    }
    ksnprintf(w->err, sizeof(w->err), "keine GTK in den Schluesseldaten");
    return -1;
}

int wpa_rx(Wpa *w, const uint8_t *f, uint32_t len, uint8_t *out, uint32_t max)
{
    w->err[0] = 0;
    if (len < EAPOL_KEY_HDR || f[1] != 3) {
        ksnprintf(w->err, sizeof(w->err), "kein EAPOL-Key-Rahmen (Typ %u, %u Byte)", len >= 2 ? f[1] : 0, len);
        return -1;
    }
    uint32_t body = 4u + get16(f + 2);
    if (body < len)
        len = body; /* Auffuellung des Rahmens abschneiden */
    uint16_t info = get16(f + OFF_INFO);
    uint32_t dlen = get16(f + OFF_DLEN);
    if (f[4] != 2 || (info & KI_VERSION) != 2) {
        ksnprintf(w->err, sizeof(w->err), "Descriptor %u, Version %u (nur RSN mit HMAC-SHA1/AES)", f[4],
                  info & KI_VERSION);
        return -1;
    }
    if (EAPOL_KEY_HDR + dlen > len) {
        ksnprintf(w->err, sizeof(w->err), "Schluesseldaten laenger als der Rahmen");
        return -1;
    }
    uint64_t replay = 0;
    for (int i = 0; i < 8; i++)
        replay = replay << 8 | f[OFF_REPLAY + i];
    if (!(info & KI_ACK) || info & (KI_REQUEST | KI_ERROR)) {
        ksnprintf(w->err, sizeof(w->err), "Key Information %#x: nicht vom AP", info);
        return -1;
    }
    if (w->have_replay && replay <= w->replay && (info & KI_MIC)) {
        ksnprintf(w->err, sizeof(w->err), "Replay-Zaehler %lu alt (zuletzt %lu)", (unsigned long)replay,
                  (unsigned long)w->replay);
        return -1;
    }
    uint16_t ver = info & KI_VERSION;

    if ((info & KI_PAIRWISE) && !(info & KI_MIC)) { /* Nachricht 1: ANonce, PTK ausrechnen */
        memcpy(w->anonce, f + OFF_NONCE, 32);
        w->have_anonce = 1;
        wpa_derive_ptk(w->pmk, w->aa, w->spa, w->anonce, w->snonce, w->ptk);
        return reply(w, f, (uint16_t)(ver | KI_PAIRWISE | KI_MIC), w->snonce, w->ie, w->ie_len, out, max);
    }
    if ((info & KI_PAIRWISE) && (info & KI_MIC)) { /* Nachricht 3: pruefen, GTK auspacken, bestaetigen */
        if (!w->have_anonce || memcmp(w->anonce, f + OFF_NONCE, 32) != 0) {
            ksnprintf(w->err, sizeof(w->err), "Nachricht 3 mit anderer ANonce als Nachricht 1");
            return -1;
        }
        if (!mic_ok(w->ptk, f, len)) {
            ksnprintf(w->err, sizeof(w->err), "MIC von Nachricht 3 falsch - Passwort falsch?");
            return -1;
        }
        if (!(info & KI_INSTALL) || !(info & KI_ENCRYPTED)) {
            ksnprintf(w->err, sizeof(w->err), "Nachricht 3 ohne Installieren/verschluesselte Daten (%#x)", info);
            return -1;
        }
        if (take_gtk(w, f, dlen) != 0)
            return -1;
        w->replay = replay;
        w->have_replay = 1;
        int r = reply(w, f, (uint16_t)(ver | KI_PAIRWISE | KI_MIC | (info & KI_SECURE)), 0, 0, 0, out, max);
        if (r > 0) {
            w->events |= WPA_EV_PTK | WPA_EV_GTK;
            w->done = 1;
        }
        return r;
    }
    if (!(info & KI_PAIRWISE) && (info & KI_MIC)) { /* Gruppen-Nachricht 1: neuer GTK */
        if (!w->done) {
            ksnprintf(w->err, sizeof(w->err), "Gruppenschluessel vor dem 4-Wege-Handshake");
            return -1;
        }
        if (!mic_ok(w->ptk, f, len)) {
            ksnprintf(w->err, sizeof(w->err), "MIC der Gruppen-Nachricht falsch");
            return -1;
        }
        if (!(info & KI_ENCRYPTED) || take_gtk(w, f, dlen) != 0) {
            if (!w->err[0])
                ksnprintf(w->err, sizeof(w->err), "Gruppen-Nachricht ohne verschluesselte Daten");
            return -1;
        }
        w->replay = replay;
        w->have_replay = 1;
        int r = reply(w, f, (uint16_t)(ver | KI_MIC | KI_SECURE), 0, 0, 0, out, max);
        if (r > 0)
            w->events |= WPA_EV_GTK;
        return r;
    }
    ksnprintf(w->err, sizeof(w->err), "unerwartete Key Information %#x", info);
    return -1;
}
