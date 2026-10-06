/* SBC-Encoder nach der A2DP-Spezifikation (Anhang B), nur Ganzzahlen (der Kernel rechnet ohne FPU).
 *
 * Analyse je Block und Kanal (8 Teilbaender): die 8 neuen Samples kommen vorne in den Verlauf X (X[7] das aelteste,
 * X[0] das neueste), Z = C * X mit dem Prototyp-Fenster C (80 Werte), Y[i] = Summe Z[i + 16 j] (j = 0..4),
 * S[k] = Summe M[k][i] Y[i] mit M[k][i] = cos((k + 1/2)(i - 4) pi / 8). Die Spezifikation listet C mit dem Vorzeichen
 * (-1)^(n / 16) der Modulation eingerechnet; hier steht der glatte, symmetrische Prototyp p(n) = p(80 - n), daraus
 * wird C beim Start gebildet. Festkomma: C in Q31, Y und S in Q15 (Samples mit 15 Nachkommabits), M in Q14.
 *
 * Danach je Kanal und Band ein Skalenfaktor (|S| < 2^(sf+1)), die Bitzuteilung "Loudness" (beide Kanaele teilen sich
 * den Bitpool, wie es der Decoder nachrechnet), Quantisierung q = (S / 2^(sf+1) + 1) * (2^bits - 1) / 2 und der Frame:
 * 0x9C, Format, Bitpool, CRC-8 (ueber Format, Bitpool und Skalenfaktoren), Skalenfaktoren (je 4 Bit), Samples. */

#include "drivers/bt/sbc.h"
#include "lib/string.h"

#define Q31(x) ((int32_t)((x) * 2147483648.0 + ((x) < 0 ? -0.5 : 0.5)))

/* Prototyp p(0..40) (Betraege aus Proto_8_80 der Spezifikation, Vorzeichen des glatten Tiefpasses) */
static const int32_t proto[41] = {
    Q31(0.00000000E+00),  Q31(1.56575398E-04),  Q31(3.43256425E-04),  Q31(5.54620202E-04),  Q31(8.23919506E-04),
    Q31(1.13992507E-03),  Q31(1.47640169E-03),  Q31(1.78371725E-03),  Q31(2.01182542E-03),  Q31(2.10371989E-03),
    Q31(1.99454554E-03),  Q31(1.61656283E-03),  Q31(9.02154502E-04),  Q31(-1.78805361E-04), Q31(-1.64973098E-03),
    Q31(-3.49717454E-03), Q31(-5.65949473E-03), Q31(-8.02941163E-03), Q31(-1.04584443E-02), Q31(-1.27472335E-02),
    Q31(-1.46525263E-02), Q31(-1.59045603E-02), Q31(-1.62208471E-02), Q31(-1.53184106E-02), Q31(-1.29371806E-02),
    Q31(-8.85757540E-03), Q31(-2.92408442E-03), Q31(4.91578024E-03),  Q31(1.46404076E-02),  Q31(2.61098752E-02),
    Q31(3.90751381E-02),  Q31(5.31873032E-02),  Q31(6.79989431E-02),  Q31(8.29847578E-02),  Q31(9.75753918E-02),
    Q31(1.11196689E-01),  Q31(1.23264548E-01),  Q31(1.33264415E-01),  Q31(1.40753505E-01),  Q31(1.45389847E-01),
    Q31(1.46955068E-01),
};

/* cos(n pi / 16) fuer n = 0..8 in Q14 */
static const int32_t cos16[9] = {16384, 16069, 15137, 13623, 11585, 9102, 6270, 3196, 0};

/* Loudness-Offsets fuer 8 Teilbaender (Spezifikation, Anhang B) je Abtastrate */
static const int8_t offset8[4][8] = {
    {-2, 0, 0, 0, 0, 0, 0, 1}, {-3, 0, 0, 0, 0, 0, 1, 2}, {-4, 0, 0, 0, 0, 0, 1, 2}, {-4, 0, 0, 0, 0, 0, 1, 2}};

static int32_t C[80];    /* Fenster mit Vorzeichen der Modulation */
static int32_t M[8][16]; /* Modulation, Q14 */
static int     tables_ready;

static int32_t cosq(int n) /* cos(n pi / 16) in Q14 fuer beliebige ganze n */
{
    n %= 32;
    if (n < 0)
        n += 32;
    if (n > 16)
        n = 32 - n;
    return n <= 8 ? cos16[n] : -cos16[16 - n];
}

static void tables(void)
{
    if (tables_ready)
        return;
    for (int n = 0; n < 80; n++) {
        int32_t p = proto[n <= 40 ? n : 80 - n];
        C[n] = (n / 16) & 1 ? -p : p;
    }
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < 16; i++)
            M[k][i] = cosq((2 * k + 1) * (i - 4));
    tables_ready = 1;
}

void sbcenc_init(SbcEnc *e, int freq, int bitpool)
{
    tables();
    memset(e, 0, sizeof(*e));
    e->freq = freq & 3;
    e->bitpool = bitpool;
}

uint32_t sbcenc_frame_len(const SbcEnc *e)
{
    return 4 + 8 + (16u * (uint32_t)e->bitpool + 7) / 8; /* Kopf, Skalenfaktoren (2 x 8 x 4 Bit), Samples */
}

/* Ein Block eines Kanals: 8 neue Samples (Abstand stride im PCM), Ergebnis S[8] in Q15 */
static void analyze(int32_t *x, const int16_t *pcm, int stride, int64_t *s)
{
    memmove(x + 8, x, 72 * sizeof(int32_t));
    for (int i = 0; i < 8; i++)
        x[7 - i] = pcm[i * stride];
    int64_t y[16];
    for (int i = 0; i < 16; i++) {
        int64_t acc = 0;
        for (int j = 0; j < 5; j++)
            acc += (int64_t)C[i + 16 * j] * x[i + 16 * j];
        y[i] = acc >> 16; /* Q31 * Sample -> Q15 */
    }
    for (int k = 0; k < 8; k++) {
        int64_t acc = 0;
        for (int i = 0; i < 16; i++)
            acc += y[i] * M[k][i];
        s[k] = acc >> 14;
    }
}

/* Bitzuteilung "Loudness" fuer Stereo (beide Kanaele aus einem Bitpool), wie Spezifikation 12.6.3 */
static void allocate(int freq, int bitpool, const int sf[2][8], int bits[2][8])
{
    int need[2][8], max_need = 0;
    for (int ch = 0; ch < 2; ch++)
        for (int sb = 0; sb < 8; sb++) {
            int n;
            if (sf[ch][sb] == 0) {
                n = -5;
            } else {
                int loud = sf[ch][sb] - offset8[freq][sb];
                n = loud > 0 ? loud / 2 : loud;
            }
            need[ch][sb] = n;
            if (n > max_need)
                max_need = n;
        }
    int bitcount = 0, slicecount = 0, slice = max_need + 1;
    do {
        slice--;
        bitcount += slicecount;
        slicecount = 0;
        for (int ch = 0; ch < 2; ch++)
            for (int sb = 0; sb < 8; sb++) {
                if (need[ch][sb] > slice + 1 && need[ch][sb] < slice + 16)
                    slicecount++;
                else if (need[ch][sb] == slice + 1)
                    slicecount += 2;
            }
    } while (bitcount + slicecount < bitpool);
    if (bitcount + slicecount == bitpool) {
        bitcount += slicecount;
        slice--;
    }
    for (int ch = 0; ch < 2; ch++)
        for (int sb = 0; sb < 8; sb++) {
            if (need[ch][sb] < slice + 2)
                bits[ch][sb] = 0;
            else
                bits[ch][sb] = need[ch][sb] - slice < 16 ? need[ch][sb] - slice : 16;
        }
    int ch = 0, sb = 0;
    while (bitcount < bitpool && sb < 8) {
        if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
            bits[ch][sb]++;
            bitcount++;
        } else if (need[ch][sb] == slice + 1 && bitpool > bitcount + 1) {
            bits[ch][sb] = 2;
            bitcount += 2;
        }
        if (ch == 1) {
            ch = 0;
            sb++;
        } else {
            ch = 1;
        }
    }
    ch = 0;
    sb = 0;
    while (bitcount < bitpool && sb < 8) {
        if (bits[ch][sb] < 16) {
            bits[ch][sb]++;
            bitcount++;
        }
        if (ch == 1) {
            ch = 0;
            sb++;
        } else {
            ch = 1;
        }
    }
}

typedef struct {
    uint8_t *p;
    uint32_t bit; /* naechstes Bit (0 = hoechstes von p[0]) */
} Bits;

static void put_bits(Bits *b, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (v >> i & 1)
            b->p[b->bit >> 3] |= (uint8_t)(0x80 >> (b->bit & 7));
        b->bit++;
    }
}

/* CRC-8 der Spezifikation: x^8 + x^4 + x^3 + x^2 + 1, Startwert 0x0F, Bits von hoch nach niedrig */
static uint8_t crc8(uint8_t crc, const uint8_t *d, uint32_t nbits)
{
    for (uint32_t i = 0; i < nbits; i++) {
        int bit = d[i >> 3] >> (7 - (i & 7)) & 1;
        int top = (crc >> 7) ^ bit;
        crc = (uint8_t)(crc << 1);
        if (top)
            crc ^= 0x1D;
    }
    return crc;
}

uint32_t sbcenc_encode(SbcEnc *e, const int16_t *pcm, uint8_t *out)
{
    static int64_t s[16][2][8];
    for (int blk = 0; blk < 16; blk++)
        for (int ch = 0; ch < 2; ch++)
            analyze(e->x[ch], pcm + blk * 16 + ch, 2, s[blk][ch]);

    int sf[2][8], bits[2][8];
    for (int ch = 0; ch < 2; ch++)
        for (int sb = 0; sb < 8; sb++) {
            int64_t m = 0;
            for (int blk = 0; blk < 16; blk++) {
                int64_t v = s[blk][ch][sb] < 0 ? -s[blk][ch][sb] : s[blk][ch][sb];
                if (v > m)
                    m = v;
            }
            int f = 0;
            while (f < 15 && m >= (int64_t)1 << (f + 1 + 15))
                f++;
            sf[ch][sb] = f;
        }
    allocate(e->freq, e->bitpool, sf, bits);

    uint32_t len = sbcenc_frame_len(e);
    memset(out, 0, len);
    out[0] = 0x9C;
    out[1] = (uint8_t)(e->freq << 6 | 3 << 4 | 2 << 2 | 0 << 1 | 1); /* 16 Bloecke, Stereo, Loudness, 8 Baender */
    out[2] = (uint8_t)e->bitpool;
    Bits b = {out, 4 * 8};
    for (int ch = 0; ch < 2; ch++)
        for (int sb = 0; sb < 8; sb++)
            put_bits(&b, (uint32_t)sf[ch][sb], 4);
    uint8_t crc = crc8(0x0F, out + 1, 16);
    out[3] = crc8(crc, out + 4, 64);
    for (int blk = 0; blk < 16; blk++)
        for (int ch = 0; ch < 2; ch++)
            for (int sb = 0; sb < 8; sb++) {
                int nb = bits[ch][sb];
                if (!nb)
                    continue;
                int64_t levels = ((int64_t)1 << nb) - 1, scale = (int64_t)1 << (sf[ch][sb] + 1 + 15);
                int64_t q = ((s[blk][ch][sb] + scale) * levels) >> (sf[ch][sb] + 2 + 15);
                if (q < 0)
                    q = 0;
                if (q > levels)
                    q = levels;
                put_bits(&b, (uint32_t)q, nb);
            }
    return len;
}
