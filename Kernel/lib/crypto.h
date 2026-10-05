#ifndef LIB_CRYPTO_H
#define LIB_CRYPTO_H

#include <stdint.h>

/* Kryptografie fuer WPA2 (net/wpa.c): SHA-1, HMAC-SHA1, PBKDF2 (Passwort -> PMK), AES-128 und AES Key Wrap
 * (RFC 3394). Nur Ganzzahlen; kurz statt schnell - gerechnet wird nur beim Verbinden und beim Schluesselwechsel. */

typedef struct {
    uint32_t h[5];
    uint64_t len;     /* Bytes bisher */
    uint8_t  buf[64];
    uint32_t n;       /* davon in buf */
} Sha1;

void sha1_init(Sha1 *c);
void sha1_update(Sha1 *c, const void *data, uint32_t len);
void sha1_final(Sha1 *c, uint8_t out[20]);
void sha1(const void *data, uint32_t len, uint8_t out[20]);

/* HMAC-SHA1 ueber mehrere Teile (n Stueck), wie hintereinander geschrieben */
void hmac_sha1_v(const uint8_t *key, uint32_t key_len, int n, const uint8_t *const part[], const uint32_t part_len[],
                 uint8_t out[20]);
void hmac_sha1(const uint8_t *key, uint32_t key_len, const void *data, uint32_t len, uint8_t out[20]);

/* PBKDF2 mit HMAC-SHA1 (RFC 2898); WPA: Passphrase, SSID als Salz, 4096 Runden, 32 Byte */
void pbkdf2_sha1(const void *pass, uint32_t pass_len, const void *salt, uint32_t salt_len, uint32_t rounds,
                 uint8_t *out, uint32_t out_len);

typedef struct {
    uint8_t rk[176]; /* 11 Rundenschluessel */
} Aes128;

void aes128_init(Aes128 *k, const uint8_t key[16]);
void aes128_encrypt(const Aes128 *k, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const Aes128 *k, const uint8_t in[16], uint8_t out[16]);

/* AES Key Wrap (RFC 3394) mit 128-Bit-KEK: n = Laenge des Klartexts (Vielfaches von 8, >= 16), out n + 8 Byte.
 * unwrap: in n + 8 Byte -> out n Byte; 0 = ok, -1 = Pruefwert falsch (falscher Schluessel oder verfaelscht) */
void aes_wrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out);
int  aes_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out);

#endif
