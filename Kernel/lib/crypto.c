/* SHA-1, HMAC-SHA1, PBKDF2, AES-128 und AES Key Wrap fuer WPA2 (siehe crypto.h) */

#include "lib/crypto.h"
#include "lib/string.h"

/* ---------- SHA-1 (FIPS 180-4) ---------- */

static uint32_t rol(uint32_t x, int n)
{
    return x << n | x >> (32 - n);
}

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void sha1_init(Sha1 *c)
{
    c->h[0] = 0x67452301;
    c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->len = 0;
    c->n = 0;
}

void sha1_update(Sha1 *c, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        uint32_t k = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, k);
        c->n += k;
        p += k;
        len -= k;
        if (c->n == 64) {
            sha1_block(c->h, c->buf);
            c->n = 0;
        }
    }
}

void sha1_final(Sha1 *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56)
        sha1_update(c, &pad, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++)
        l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, l, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void sha1(const void *data, uint32_t len, uint8_t out[20])
{
    Sha1 c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* ---------- HMAC-SHA1 (RFC 2104) ---------- */

/* Zustand nach dem inneren und dem aeusseren Schluesselblock: PBKDF2 rechnet damit jede Runde nur zwei Bloecke */
static void hmac_start(const uint8_t *key, uint32_t key_len, Sha1 *inner, Sha1 *outer)
{
    uint8_t k[64], tk[20];
    if (key_len > 64) {
        sha1(key, key_len, tk);
        key = tk;
        key_len = 20;
    }
    memset(k, 0, 64);
    memcpy(k, key, key_len);
    for (int i = 0; i < 64; i++)
        k[i] ^= 0x36;
    sha1_init(inner);
    sha1_update(inner, k, 64);
    for (int i = 0; i < 64; i++)
        k[i] ^= 0x36 ^ 0x5C;
    sha1_init(outer);
    sha1_update(outer, k, 64);
}

static void hmac_finish(Sha1 *inner, Sha1 *outer, uint8_t out[20])
{
    uint8_t ih[20];
    sha1_final(inner, ih);
    sha1_update(outer, ih, 20);
    sha1_final(outer, out);
}

void hmac_sha1_v(const uint8_t *key, uint32_t key_len, int n, const uint8_t *const part[], const uint32_t part_len[],
                 uint8_t out[20])
{
    Sha1 inner, outer;
    hmac_start(key, key_len, &inner, &outer);
    for (int i = 0; i < n; i++)
        sha1_update(&inner, part[i], part_len[i]);
    hmac_finish(&inner, &outer, out);
}

void hmac_sha1(const uint8_t *key, uint32_t key_len, const void *data, uint32_t len, uint8_t out[20])
{
    const uint8_t *part[1] = {data};
    uint32_t plen[1] = {len};
    hmac_sha1_v(key, key_len, 1, part, plen, out);
}

/* ---------- PBKDF2 (RFC 2898) ---------- */

void pbkdf2_sha1(const void *pass, uint32_t pass_len, const void *salt, uint32_t salt_len, uint32_t rounds,
                 uint8_t *out, uint32_t out_len)
{
    Sha1 inner0, outer0;
    hmac_start(pass, pass_len, &inner0, &outer0);
    for (uint32_t block = 1; out_len; block++) {
        uint8_t cnt[4] = {(uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block};
        uint8_t u[20], t[20];
        Sha1 in = inner0, ou = outer0;
        sha1_update(&in, salt, salt_len);
        sha1_update(&in, cnt, 4);
        hmac_finish(&in, &ou, u);
        memcpy(t, u, 20);
        for (uint32_t r = 1; r < rounds; r++) {
            in = inner0;
            ou = outer0;
            sha1_update(&in, u, 20);
            hmac_finish(&in, &ou, u);
            for (int i = 0; i < 20; i++)
                t[i] ^= u[i];
        }
        uint32_t k = out_len < 20 ? out_len : 20;
        memcpy(out, t, k);
        out += k;
        out_len -= k;
    }
}

/* ---------- AES-128 (FIPS 197) ---------- */

static uint8_t sbox[256], inv_sbox[256];

static uint8_t rol8(uint8_t x, int n)
{
    return (uint8_t)(x << n | x >> (8 - n));
}

/* S-Box ausrechnen statt abschreiben: p laeuft mit dem Erzeuger 3 durch alle Elemente von GF(2^8), q mit dem Inversen
 * (1/3) rueckwaerts - so ist q immer das Inverse von p; S(p) ist die affine Abbildung von q */
static void aes_tables(void)
{
    if (sbox[0] == 0x63)
        return;
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80)
            q ^= 0x09;
        sbox[p] = (uint8_t)(q ^ rol8(q, 1) ^ rol8(q, 2) ^ rol8(q, 3) ^ rol8(q, 4) ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++)
        inv_sbox[sbox[i]] = (uint8_t)i;
}

static uint8_t xt(uint8_t x)
{
    return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1B : 0));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

void aes128_init(Aes128 *k, const uint8_t key[16])
{
    aes_tables();
    memcpy(k->rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {k->rk[i - 4], k->rk[i - 3], k->rk[i - 2], k->rk[i - 1]};
        if (i % 16 == 0) {
            uint8_t a = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[a];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; j++)
            k->rk[i + j] = k->rk[i - 16 + j] ^ t[j];
    }
}

/* Zustand wie die Eingabe: s[4 * Spalte + Zeile] */
static void add_key(uint8_t s[16], const uint8_t *rk)
{
    for (int i = 0; i < 16; i++)
        s[i] ^= rk[i];
}

void aes128_encrypt(const Aes128 *k, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_key(s, k->rk);
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) /* SubBytes und ShiftRows: Zeile z wandert um z Spalten nach links */
            t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];
        if (r < 10) /* MixColumns */
            for (int c = 0; c < 16; c += 4) {
                uint8_t a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3], x = a0 ^ a1 ^ a2 ^ a3;
                t[c] ^= x ^ xt(a0 ^ a1);
                t[c + 1] ^= x ^ xt(a1 ^ a2);
                t[c + 2] ^= x ^ xt(a2 ^ a3);
                t[c + 3] ^= x ^ xt(a3 ^ a0);
            }
        memcpy(s, t, 16);
        add_key(s, k->rk + 16 * r);
    }
    memcpy(out, s, 16);
}

void aes128_decrypt(const Aes128 *k, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    for (int r = 10; r >= 1; r--) {
        add_key(s, k->rk + 16 * r);
        if (r < 10) /* InvMixColumns */
            for (int c = 0; c < 16; c += 4) {
                uint8_t a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
                s[c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                s[c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                s[c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                s[c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        for (int i = 0; i < 16; i++) /* InvShiftRows und InvSubBytes */
            t[(i + 4 * (i % 4)) % 16] = inv_sbox[s[i]];
        memcpy(s, t, 16);
    }
    add_key(s, k->rk);
    memcpy(out, s, 16);
}

/* ---------- AES Key Wrap (RFC 3394) ---------- */

void aes_wrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out)
{
    Aes128 k;
    aes128_init(&k, kek);
    uint32_t blocks = n / 8;
    uint8_t a[8], b[16];
    memset(a, 0xA6, 8);
    memmove(out + 8, in, n);
    for (uint32_t j = 0; j < 6; j++)
        for (uint32_t i = 1; i <= blocks; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            aes128_encrypt(&k, b, b);
            uint64_t t = (uint64_t)blocks * j + i;
            for (int x = 0; x < 8; x++)
                a[x] = b[x] ^ (uint8_t)(t >> (56 - 8 * x));
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, a, 8);
}

int aes_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out)
{
    Aes128 k;
    aes128_init(&k, kek);
    uint32_t blocks = n / 8;
    uint8_t a[8], b[16];
    memcpy(a, in, 8);
    memmove(out, in + 8, n);
    for (int j = 5; j >= 0; j--)
        for (uint32_t i = blocks; i >= 1; i--) {
            uint64_t t = (uint64_t)blocks * (uint32_t)j + i;
            for (int x = 0; x < 8; x++)
                b[x] = a[x] ^ (uint8_t)(t >> (56 - 8 * x));
            memcpy(b + 8, out + 8 * (i - 1), 8);
            aes128_decrypt(&k, b, b);
            memcpy(a, b, 8);
            memcpy(out + 8 * (i - 1), b + 8, 8);
        }
    for (int x = 0; x < 8; x++)
        if (a[x] != 0xA6)
            return -1;
    return 0;
}
