/* Selbsttests: Kryptografie (lib/crypto.c) und WPA2-Handshake (net/wpa.c) - der Test spielt den AP */

#include "lib/crypto.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "net/wpa.h"
#include "tests/selftest.h"

static int hex_eq(const uint8_t *p, const char *hex)
{
    for (int i = 0; hex[2 * i]; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[2 * i + k];
            v = v << 4 | (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        if (p[i] != v)
            return 0;
    }
    return 1;
}

/* EAPOL-Key-Rahmen des AP: Kopf, Key Information, Replay-Zaehler, Nonce, RSC, Schluesseldaten; MIC mit kck (falls da) */
static uint32_t ap_frame(uint8_t *f, uint16_t info, uint8_t replay, const uint8_t *nonce, const uint8_t *data,
                         uint32_t dlen, const uint8_t *kck)
{
    uint32_t len = EAPOL_KEY_HDR + dlen;
    memset(f, 0, len);
    f[0] = 2;
    f[1] = 3;
    f[2] = (uint8_t)((len - 4) >> 8);
    f[3] = (uint8_t)(len - 4);
    f[4] = 2;
    f[5] = (uint8_t)(info >> 8);
    f[6] = (uint8_t)info;
    f[8] = 16;
    f[16] = replay;
    if (nonce)
        memcpy(f + 17, nonce, 32);
    f[65] = 0x2A; /* RSC */
    f[97] = (uint8_t)(dlen >> 8);
    f[98] = (uint8_t)dlen;
    memcpy(f + EAPOL_KEY_HDR, data, dlen);
    if (kck)
        wpa_mic(kck, f, len, f + 81);
    return len;
}

static int frame_mic_ok(const uint8_t *kck, const uint8_t *f, uint32_t len)
{
    static uint8_t t[512];
    memcpy(t, f, len);
    memset(t + 81, 0, 16);
    uint8_t mic[16];
    wpa_mic(kck, t, len, mic);
    return memcmp(mic, f + 81, 16) == 0;
}

/* Verschluesselte Schluesseldaten mit GTK-KDE (Nummer idx) und RSN-Element */
static uint32_t key_data(uint8_t *out, const uint8_t *kek, const uint8_t gtk[16], int idx)
{
    static const uint8_t rsn[22] = {0x30, 0x14, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4,
                                    1,    0,    0, 0x0F, 0xAC, 2, 0, 0};
    uint8_t plain[64];
    uint32_t n = 0;
    memcpy(plain, rsn, sizeof(rsn));
    n += sizeof(rsn);
    uint8_t kde[8] = {0xDD, 22, 0x00, 0x0F, 0xAC, 1, (uint8_t)idx, 0};
    memcpy(plain + n, kde, 8);
    memcpy(plain + n + 8, gtk, 16);
    n += 24;
    plain[n++] = 0xDD; /* auf ein Vielfaches von 8 auffuellen */
    while (n % 8)
        plain[n++] = 0;
    aes_wrap(kek, plain, n, out);
    return n + 8;
}

void test_crypto(void)
{
    title("Kryptografie und WPA2");
    uint8_t o[64];
    sha1("abc", 3, o);
    check("SHA-1(\"abc\")", hex_eq(o, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    uint8_t k20[20];
    memset(k20, 0x0B, 20);
    hmac_sha1(k20, 20, "Hi There", 8, o);
    check("HMAC-SHA1 (RFC 2202, Fall 1)", hex_eq(o, "b617318655057264e28bc0b6fb378c8ef146be00"));
    pbkdf2_sha1("password", 8, "salt", 4, 4096, o, 20);
    check("PBKDF2-SHA1 (RFC 6070, 4096 Runden)", hex_eq(o, "4b007901b765489abead49d926f721d065a429c1"));
    uint8_t pmk[32];
    int r = wpa_pmk("password", (const uint8_t *)"IEEE", 4, pmk);
    check("WPA-PSK aus Passphrase (802.11, Anhang J)",
          r == 0 && hex_eq(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
    check("Passphrase zu kurz abgelehnt", wpa_pmk("kurz", (const uint8_t *)"IEEE", 4, o) == -1);
    uint8_t key[16], pt[16];
    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)i;
        pt[i] = (uint8_t)(i * 0x11);
    }
    Aes128 aes;
    aes128_init(&aes, key);
    aes128_encrypt(&aes, pt, o);
    aes128_decrypt(&aes, o, o + 16);
    check("AES-128 (FIPS 197, C.1) hin und zurueck",
          hex_eq(o, "69c4e0d86a7b0430d8cdb78070b4c55a") && memcmp(o + 16, pt, 16) == 0);
    aes_wrap(key, pt, 16, o);
    r = aes_unwrap(key, o, 16, o + 32);
    check("AES Key Wrap (RFC 3394, 4.1) hin und zurueck",
          hex_eq(o, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5") && r == 0 && memcmp(o + 32, pt, 16) == 0);
    o[3] ^= 1;
    check("Key Unwrap erkennt Verfaelschung", aes_unwrap(key, o, 16, o + 32) == -1);

    /* PTK (Vergleichswert mit Python/hmac gerechnet) */
    static const uint8_t aa[6] = {2, 0, 0, 0, 0, 1}, spa[6] = {2, 0, 0, 0, 0, 2};
    uint8_t anonce[32], snonce[32], ptk[48];
    for (int i = 0; i < 32; i++) {
        anonce[i] = (uint8_t)i;
        snonce[i] = (uint8_t)(100 + i);
    }
    wpa_derive_ptk(pmk, aa, spa, anonce, snonce, ptk);
    check("PTK = PRF-384 (\"Pairwise key expansion\")",
          hex_eq(ptk, "74e37d6502340450f71d962d3772c3768de1b2e1ad4f5ad4a3116e1bb3fdfa23"
                      "1578e877c310dfe5ed99c23848a5c717"));

    /* 4-Wege-Handshake: der Test ist der AP */
    static Wpa w;
    static uint8_t f[512], rep[512], kd[128];
    static const uint8_t ie[22] = {0x30, 0x14, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0, 0};
    wpa_start(&w, pmk, aa, spa, ie, sizeof(ie), snonce);
    uint32_t n = ap_frame(f, 0x008A, 1, anonce, 0, 0, 0); /* Version 2, paarweise, Ack */
    int m2 = wpa_rx(&w, f, n, rep, sizeof(rep));
    check("Nachricht 1 -> Nachricht 2 mit SNonce und RSN-Element",
          m2 == EAPOL_KEY_HDR + 22 && memcmp(rep + 17, snonce, 32) == 0 && memcmp(rep + EAPOL_KEY_HDR, ie, 22) == 0 &&
              rep[5] == 0x01 && rep[6] == 0x0A && rep[16] == 1);
    check("Nachricht 2: MIC stimmt (KCK des AP)", m2 > 0 && frame_mic_ok(ptk, rep, (uint32_t)m2));
    uint8_t gtk[16];
    for (int i = 0; i < 16; i++)
        gtk[i] = (uint8_t)(0xA0 + i);
    uint32_t dl = key_data(kd, ptk + 16, gtk, 1);
    n = ap_frame(f, 0x13CA, 2, anonce, kd, dl, ptk); /* Installieren, Ack, MIC, Secure, verschluesselt */
    int m4 = wpa_rx(&w, f, n, rep, sizeof(rep));
    check("Nachricht 3 -> Nachricht 4 (Secure, MIC stimmt)",
          m4 == EAPOL_KEY_HDR && rep[5] == 0x03 && rep[6] == 0x0A && frame_mic_ok(ptk, rep, (uint32_t)m4));
    check("Schluessel: TK und GTK (Nummer 1, RSC) wie beim AP",
          w.events == (WPA_EV_PTK | WPA_EV_GTK) && memcmp(w.ptk + 32, ptk + 32, 16) == 0 && w.gtk_len == 16 &&
              memcmp(w.gtk, gtk, 16) == 0 && w.gtk_idx == 1 && w.gtk_rsc[0] == 0x2A && w.done);
    check("Nachricht 3 noch einmal (alter Replay-Zaehler) abgelehnt", wpa_rx(&w, f, n, rep, sizeof(rep)) == -1);
    w.events = 0;

    /* Gruppenschluessel-Wechsel, dann ein verfaelschter */
    gtk[0] ^= 0xFF;
    dl = key_data(kd, ptk + 16, gtk, 2);
    n = ap_frame(f, 0x1382, 3, 0, kd, dl, ptk);
    int g2 = wpa_rx(&w, f, n, rep, sizeof(rep));
    check("Gruppen-Nachricht 1 -> 2, neuer GTK (Nummer 2)",
          g2 == EAPOL_KEY_HDR && rep[5] == 0x03 && rep[6] == 0x02 && frame_mic_ok(ptk, rep, (uint32_t)g2) &&
              w.events == WPA_EV_GTK && w.gtk_idx == 2 && memcmp(w.gtk, gtk, 16) == 0);
    n = ap_frame(f, 0x1382, 4, 0, kd, dl, ptk);
    f[EAPOL_KEY_HDR + 5] ^= 1;
    check("Verfaelschte Gruppen-Nachricht abgelehnt (MIC)", wpa_rx(&w, f, n, rep, sizeof(rep)) == -1);

    /* Falsches Passwort: der AP rechnet mit einem anderen PMK */
    uint8_t pmk2[32], ptk2[48];
    wpa_pmk("falsches Passwort", (const uint8_t *)"IEEE", 4, pmk2);
    wpa_start(&w, pmk2, aa, spa, ie, sizeof(ie), snonce);
    n = ap_frame(f, 0x008A, 1, anonce, 0, 0, 0);
    wpa_rx(&w, f, n, rep, sizeof(rep));
    wpa_derive_ptk(pmk, aa, spa, anonce, snonce, ptk2);
    dl = key_data(kd, ptk2 + 16, gtk, 1);
    n = ap_frame(f, 0x13CA, 2, anonce, kd, dl, ptk2);
    check("Falsches Passwort: Nachricht 3 abgelehnt, keine Schluessel",
          wpa_rx(&w, f, n, rep, sizeof(rep)) == -1 && w.events == 0 && !w.done);
}
