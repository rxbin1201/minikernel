#include "libc.h"

/* ramtest [MB] [Runden]: Arbeitsspeicher pruefen. Belegt MB Megabyte (Standard: alles Freie bis auf 128 MB Reserve)
 * in Bloecken zu 64 MB, schreibt nacheinander Muster hinein (Nullen, Einsen, Schachbrett, laufende Eins, Adresse,
 * Zufall), liest alles zurueck und meldet jede falsche Stelle. Dazu die Geschwindigkeit beim Schreiben und Lesen.
 * Mehrere gleichzeitig ("ramtest 1024 & ramtest 1024") pruefen mit mehreren Kernen. Strg+C bricht ab.
 * Getestet wird nur Speicher, den der Kernel gerade frei hat (nicht der von Kernel und laufenden Programmen). */

#define CHUNK    (64ULL << 20)
#define MAXCHUNK 4096
#define RESERVE  (128ULL << 20)

static u64 *chunk[MAXCHUNK];
static int  nchunk;
static u64  errors, shown;

enum { P_ZERO, P_ONES, P_CHECK, P_WALK, P_ADDR, P_RAND, NPAT };
static const char *pat_name[NPAT] = {"Nullen", "Einsen", "Schachbrett", "Laufende Eins", "Adresse", "Zufall"};

static inline u64 value(int pat, u64 i, u64 seed, u64 *rnd)
{
    switch (pat) {
    case P_ZERO: return 0;
    case P_ONES: return ~0ULL;
    case P_CHECK: return (i & 1) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
    case P_WALK: return 1ULL << ((i + seed) & 63);
    case P_ADDR: return i ^ seed;
    default: { /* xorshift64: beim Pruefen mit demselben Start wieder erzeugt */
        u64 x = *rnd;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return *rnd = x;
    }
    }
}

/* Megabyte pro Sekunde aus Bytes und Ticks (je 10 ms) */
static void speed(char *out, int max, u64 bytes, s64 ticks)
{
    if (ticks < 1)
        ticks = 1;
    u64 mbs = bytes / (1 << 20) * 100 / (u64)ticks;
    if (mbs >= 1024)
        snprintf(out, max, "%llu,%llu GB/s", (unsigned long long)(mbs / 1024), (unsigned long long)(mbs % 1024 * 10 / 1024));
    else
        snprintf(out, max, "%llu MB/s", (unsigned long long)mbs);
}

static void run_pattern(int pat, u64 seed)
{
    const u64 words = CHUNK / 8;
    u64 rnd = seed | 1;
    s64 t0 = sys_ticks();
    for (int c = 0; c < nchunk; c++) {
        u64 *p = chunk[c], base = (u64)c * words;
        for (u64 i = 0; i < words; i++)
            p[i] = value(pat, base + i, seed, &rnd);
    }
    __asm__ __volatile__("" ::: "memory"); /* erst alles schreiben, dann wirklich aus dem Speicher lesen */
    s64 t1 = sys_ticks();
    u64 bad = 0;
    rnd = seed | 1;
    for (int c = 0; c < nchunk; c++) {
        volatile u64 *p = chunk[c];
        u64 base = (u64)c * words;
        for (u64 i = 0; i < words; i++) {
            u64 want = value(pat, base + i, seed, &rnd), got = p[i];
            if (got != want) {
                bad++;
                if (shown < 10) {
                    shown++;
                    printf("  FEHLER bei %p (MB %llu): erwartet %016llx, gelesen %016llx (Bits %016llx)\n", (void *)&p[i],
                           (unsigned long long)((base + i) * 8 >> 20), (unsigned long long)want,
                           (unsigned long long)got, (unsigned long long)(want ^ got));
                }
            }
        }
    }
    s64 t2 = sys_ticks();
    errors += bad;
    char w[24], r[24];
    u64 bytes = (u64)nchunk * CHUNK;
    speed(w, sizeof(w), bytes, t1 - t0);
    speed(r, sizeof(r), bytes, t2 - t1);
    printf("  %-14s schreiben %-11s lesen %-11s %s", pat_name[pat], w, r, bad ? C_RED : C_GREEN);
    if (bad)
        printf("%llu Fehler%s\n", (unsigned long long)bad, C_RESET);
    else
        printf("OK%s\n", C_RESET);
}

void _start(int argc, char **argv)
{
    SysInfo si;
    if (sys_sysinfo(&si) != 0) {
        printf("ramtest: Kernel liefert keine Speicherangaben\n");
        sys_exit(1);
    }
    u64 want;
    if (argc > 1) {
        want = (u64)atoi(argv[1]) << 20;
    } else {
        want = si.mem_free > RESERVE + CHUNK ? si.mem_free - RESERVE : CHUNK;
    }
    want = want / CHUNK * CHUNK; /* ganze Bloecke, abgerundet: die Reserve bleibt */
    int rounds = argc > 2 ? atoi(argv[2]) : 1;
    if (want < CHUNK || rounds < 1) {
        printf("Aufruf: ramtest [MB, mindestens 64] [Runden]\n");
        sys_exit(1);
    }
    if (want > si.mem_free - (si.mem_free > RESERVE ? RESERVE / 2 : 0))
        printf("Achtung: %llu MB gew\xC3\xBCnscht, frei sind nur %llu MB - es wird belegt, was geht.\n",
               (unsigned long long)(want >> 20), (unsigned long long)(si.mem_free >> 20));

    printf("Belege %llu MB (frei: %llu MB von %llu MB) ...\n", (unsigned long long)(want >> 20),
           (unsigned long long)(si.mem_free >> 20), (unsigned long long)(si.mem_total >> 20));
    s64 t0 = sys_ticks();
    while ((u64)nchunk * CHUNK < want && nchunk < MAXCHUNK) {
        if (sys_sysinfo(&si) == 0 && si.mem_free < CHUNK + RESERVE / 2) /* dem Kernel etwas lassen */
            break;
        s64 a = sys_mmap(CHUNK);
        if (a < 0)
            break;
        chunk[nchunk++] = (u64 *)a;
    }
    if (!nchunk) {
        printf("ramtest: kein Speicher zu bekommen\n");
        sys_exit(1);
    }
    u64 bytes = (u64)nchunk * CHUNK;
    printf("%llu MB belegt in %lld ms. Pr\xC3\xBC" "fe %d Runde%s mit %d Mustern:\n", (unsigned long long)(bytes >> 20),
           (long long)(sys_ticks() - t0) * 10, rounds, rounds == 1 ? "" : "n", NPAT);

    s64 start = sys_ticks();
    for (int r = 0; r < rounds; r++) {
        if (rounds > 1)
            printf("Runde %d/%d\n", r + 1, rounds);
        for (int pat = 0; pat < NPAT; pat++)
            run_pattern(pat, 0x9E3779B97F4A7C15ULL * (u64)(r * NPAT + pat + 1) + (u64)sys_ticks());
    }
    s64 secs = (sys_ticks() - start) / 100;
    for (int c = 0; c < nchunk; c++)
        sys_munmap(chunk[c], CHUNK);
    if (errors)
        printf("%sErgebnis: %llu Fehler in %llu MB%s (%lld s) - der Arbeitsspeicher ist vermutlich defekt.\n", C_RED,
               (unsigned long long)errors, (unsigned long long)(bytes >> 20), C_RESET, (long long)secs);
    else
        printf("%sErgebnis: keine Fehler in %llu MB%s (%lld s)\n", C_GREEN, (unsigned long long)(bytes >> 20), C_RESET,
               (long long)secs);
    sys_exit(errors ? 2 : 0);
}
