#include "libc.h"

/* play DATEI.wav     spielt eine WAV-Datei ab (PCM 8/16/24/32 Bit oder 32-Bit-Gleitkomma, Mono/Stereo/mehr Kanaele,
 *                    beliebige Abtastrate: was der Codec nicht kann, wird auf 48 kHz umgerechnet)
 * play -t [HZ]       Testton: 2 s Sinus (Standard 440 Hz), abwechselnd links und rechts
 * play -v N          Lautstaerke 0-100 setzen (mit Datei: vor dem Abspielen); ohne alles: Lautstaerke anzeigen
 * Abbrechen mit Strg+C. Der Kernel spielt immer 16 Bit Stereo; alles andere rechnet play um (ohne Gleitkomma). */

#define CHUNK_FRAMES 4096

static short out[CHUNK_FRAMES * 2 * 6]; /* umgerechnete Stereo-Abtastwerte (Platz fuer Hochrechnen bis Faktor 6) */
static unsigned char in[CHUNK_FRAMES * 8 * 4];

typedef struct {
    unsigned rate, channels, bits, fmt; /* fmt: 1 = PCM, 3 = Gleitkomma */
    unsigned long long data_bytes;
} WavInfo;

static unsigned rd16le(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned rd32le(const unsigned char *p) { return rd16le(p) | rd16le(p + 2) << 16; }

static const char *audio_err(s64 e)
{
    return e == ERR_NOSYS ? "keine Soundkarte (oder kein Ausgang gefunden)" : e == ERR_AGAIN ? "ein anderes Programm spielt gerade"
         : e == ERR_INVAL ? "dieses Format kann der Codec nicht" : "Fehler";
}

/* 32-Bit-Gleitkomma (IEEE 754, als Bits) -> 16 Bit, ohne FPU */
static int float_to_s16(unsigned bits)
{
    int sign = (bits >> 31) & 1, exp = (int)((bits >> 23) & 0xFF) - 127;
    unsigned mant = (bits & 0x7FFFFF) | 0x800000;
    if (((bits >> 23) & 0xFF) == 0 || exp < -16)
        return 0;
    int v;
    if (exp >= 0)
        v = 32767; /* |x| >= 1: begrenzen */
    else {
        /* x = mant * 2^(exp-23); * 32767 */
        long long t = (long long)mant * 32767;
        int shift = 23 - exp;
        v = (int)(t >> shift);
        if (v > 32767)
            v = 32767;
    }
    return sign ? -v : v;
}

static int sample_at(const unsigned char *p, const WavInfo *w)
{
    switch (w->bits) {
    case 8: return ((int)p[0] - 128) << 8;
    case 16: return (short)rd16le(p);
    case 24: return (int)(((unsigned)p[0] << 8 | (unsigned)p[1] << 16 | (unsigned)p[2] << 24)) >> 16;
    case 32: return w->fmt == 3 ? float_to_s16(rd32le(p)) : (int)rd32le(p) >> 16;
    default: return 0;
    }
}

static int parse_wav(int fd, WavInfo *w)
{
    unsigned char h[12];
    if (sys_read(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return -1;
    int have_fmt = 0;
    for (;;) {
        unsigned char c[8];
        if (sys_read(fd, c, 8) != 8)
            return -2;
        unsigned len = rd32le(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            unsigned char f[40];
            unsigned n = len < sizeof(f) ? len : sizeof(f);
            if (sys_read(fd, f, n) != (s64)n)
                return -2;
            if (len > n)
                sys_lseek(fd, (s64)(len - n), 1);
            w->fmt = rd16le(f);
            w->channels = rd16le(f + 2);
            w->rate = rd32le(f + 4);
            w->bits = rd16le(f + 14);
            if (w->fmt == 0xFFFE && n >= 26) /* WAVE_FORMAT_EXTENSIBLE: eigentliches Format in der GUID */
                w->fmt = rd16le(f + 24);
            have_fmt = 1;
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt)
                return -3;
            w->data_bytes = len;
            return 0;
        } else {
            sys_lseek(fd, (s64)(len + (len & 1)), 1); /* anderer Block (LIST, fact, ...) */
        }
        if (len & 1 && memcmp(c, "fmt ", 4) == 0)
            sys_lseek(fd, 1, 1);
    }
}

static void print_time(unsigned long long frames, unsigned rate)
{
    unsigned s = rate ? (unsigned)(frames / rate) : 0;
    printf("%u:%02u", s / 60, s % 60);
}

static int write_out(const short *buf, unsigned frames)
{
    unsigned long long bytes = (unsigned long long)frames * 4, done = 0;
    while (done < bytes) {
        s64 r = sys_audio(1, (u64)((const char *)buf + done), bytes - done);
        if (r <= 0)
            return -1;
        done += (unsigned long long)r;
    }
    return 0;
}

/* Sinus ohne FPU: phase 0..65535 = eine Periode; Ergebnis -32767..32767 (Naeherung nach Bhaskara, < 0,2 % Fehler) */
static int isin(unsigned phase)
{
    long long t = phase & 0x7FFF, p = 0x8000;
    long long num = 16 * t * (p - t), den = 5 * p * p - 4 * t * (p - t);
    int v = (int)(num * 32767 / den);
    return (phase & 0x8000) ? -v : v;
}

static int test_tone(unsigned hz)
{
    unsigned rate = 48000;
    s64 r = sys_audio(0, rate, 2);
    if (r < 0) {
        fprintf(2, "play: %s\n", audio_err(r));
        return 1;
    }
    printf("Testton %u Hz, 2 s (erst links, dann rechts, dann beide) ...\n", hz);
    unsigned phase = 0, step = (unsigned)(((unsigned long long)hz << 16) / rate);
    for (unsigned f = 0; f < rate * 2;) {
        unsigned n = 0;
        for (; n < CHUNK_FRAMES && f < rate * 2; n++, f++) {
            int v = isin(phase) / 3; /* etwa -10 dB */
            phase = (phase + step) & 0xFFFF;
            unsigned part = f * 3 / (rate * 2);
            out[2 * n] = (short)(part == 1 ? 0 : v);
            out[2 * n + 1] = (short)(part == 0 ? 0 : v);
        }
        if (write_out(out, n))
            break;
    }
    sys_audio(2, 0, 0);
    sys_audio(3, 0, 0);
    return 0;
}

void _start(int argc, char **argv)
{
    const char *file = 0;
    int vol = -1, tone = 0;
    unsigned tone_hz = 440;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") && i + 1 < argc)
            vol = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t")) {
            tone = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                tone_hz = (unsigned)atoi(argv[++i]);
        } else if (argv[i][0] != '-')
            file = argv[i];
        else {
            fprintf(2, "Aufruf: play [-v 0-100] DATEI.wav | play -t [HZ] | play -v N\n");
            sys_exit(2);
        }
    }
    if (vol >= 0 || (!file && !tone)) {
        s64 v = sys_audio(4, vol >= 0 ? (u64)vol : (u64)-1, 0);
        if (v < 0) {
            fprintf(2, "play: %s\n", audio_err(v));
            sys_exit(1);
        }
        if (!file && !tone) {
            printf("Lautstaerke %lld %%\n", (long long)v);
            sys_exit(0);
        }
    }
    if (tone)
        sys_exit(test_tone(tone_hz ? tone_hz : 440));

    int fd = (int)sys_open(file, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "play: %s nicht gefunden\n", file);
        sys_exit(1);
    }
    WavInfo w = {0};
    int pr = parse_wav(fd, &w);
    if (pr || !w.channels || !w.rate || (w.fmt != 1 && w.fmt != 3) ||
        (w.bits != 8 && w.bits != 16 && w.bits != 24 && w.bits != 32) || (w.fmt == 3 && w.bits != 32)) {
        fprintf(2, "play: %s ist keine unterstuetzte WAV-Datei (PCM 8/16/24/32 Bit oder Gleitkomma 32 Bit)\n", file);
        sys_exit(1);
    }
    if (w.rate < 8000 || w.rate > 192000) {
        fprintf(2, "play: Abtastrate %u Hz wird nicht unterstuetzt (8000-192000)\n", w.rate);
        sys_exit(1);
    }
    unsigned rate = w.rate;
    s64 r = sys_audio(0, rate, 2);
    if (r == ERR_INVAL) { /* Rate kann der Codec nicht: auf 48 kHz umrechnen */
        rate = 48000;
        r = sys_audio(0, rate, 2);
    }
    if (r < 0) {
        fprintf(2, "play: %s\n", audio_err(r));
        sys_exit(1);
    }
    unsigned frame = w.channels * (w.bits / 8);
    unsigned long long total = w.data_bytes / frame;
    int tty = sys_isatty(1) != 0;
    printf("%s: %u Hz, %u Bit%s, %s", file, w.rate, w.bits, w.fmt == 3 ? " Gleitkomma" : "",
           w.channels == 1 ? "Mono" : w.channels == 2 ? "Stereo" : "mehrkanalig (nur links/rechts)");
    if (rate != w.rate)
        printf(", umgerechnet auf %u Hz", rate);
    printf(", Laenge ");
    print_time(total, w.rate);
    printf("\n");

    /* Umrechnen: pos = Position in Quell-Frames als 32.32-Festkomma, step = Quellrate / Zielrate */
    unsigned long long step = ((unsigned long long)w.rate << 32) / rate, pos = 0, done_frames = 0;
    int prev_l = 0, prev_r = 0, last_sec = -1;
    unsigned long long left = total;
    while (left) {
        unsigned want = left < CHUNK_FRAMES ? (unsigned)left : CHUNK_FRAMES;
        s64 got = sys_read(fd, in, want * frame);
        if (got <= 0)
            break;
        unsigned frames = (unsigned)got / frame;
        left -= frames;
        unsigned n = 0;
        if (rate == w.rate) {
            for (unsigned i = 0; i < frames; i++) {
                const unsigned char *p = in + i * frame;
                int l = sample_at(p, &w), rr = w.channels > 1 ? sample_at(p + w.bits / 8, &w) : l;
                out[2 * n] = (short)l;
                out[2 * n + 1] = (short)rr;
                n++;
            }
        } else {
            /* lineare Interpolation zwischen dem vorigen und dem aktuellen Quell-Frame */
            for (unsigned i = 0; i < frames; i++) {
                const unsigned char *p = in + i * frame;
                int l = sample_at(p, &w), rr = w.channels > 1 ? sample_at(p + w.bits / 8, &w) : l;
                while ((pos >> 32) == 0 && n < sizeof(out) / 4) {
                    unsigned frac = (unsigned)(pos >> 16) & 0xFFFF;
                    out[2 * n] = (short)(prev_l + (int)(((long long)(l - prev_l) * frac) >> 16));
                    out[2 * n + 1] = (short)(prev_r + (int)(((long long)(rr - prev_r) * frac) >> 16));
                    n++;
                    pos += step;
                }
                pos -= 1ULL << 32;
                prev_l = l;
                prev_r = rr;
            }
        }
        if (write_out(out, n)) {
            fprintf(2, "\nplay: Schreiben fehlgeschlagen\n");
            break;
        }
        done_frames += frames;
        int sec = (int)(done_frames / w.rate);
        if (tty && sec != last_sec) {
            last_sec = sec;
            printf("\r  ");
            print_time(done_frames, w.rate);
            printf(" / ");
            print_time(total, w.rate);
        }
    }
    sys_audio(2, 0, 0);
    sys_audio(3, 0, 0);
    sys_close(fd);
    if (tty)
        printf("\n");
    sys_exit(0);
}
