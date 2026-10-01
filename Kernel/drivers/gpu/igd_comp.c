/* Intel-Grafik Gen9, Stufe 6: Zusammensetzen auf der GPU (SYS_GPUCOMP). Der Desktop legt Hintergrund, Bildschirmbild,
 * Fensterbilder und Schatten in geteiltem Speicher (SYS_SHM) an und meldet sie hier an; der Kernel blendet sie fest in
 * die GGTT ein (eigener Bereich in der oberen Haelfte) und haelt eine Referenz, solange sie angemeldet sind. Danach
 * schickt der Desktop je Bild eine Liste von Auftraegen (kopieren, mischen), die die Render-Engine mit den Kernels aus
 * igd_rcs.c der Reihe nach ausfuehrt.
 *
 * Beim ersten Aufruf prueft ein Selbsttest die GPU gegen dieselbe Rechnung auf der CPU (ungerade Rechtecke, die sich
 * ueberlappen). Klappt er nur, wenn die CPU-Caches vorher zurueckgeschrieben werden, laeuft das Zusammensetzen so
 * weiter; klappt er gar nicht, meldet der Kernel "keins" und der Desktop setzt wie bisher mit der CPU zusammen.
 * Kommandozeile: gpucomp=off (aus), gpucomp=soft (dieselben Auftraege rechnet die CPU im Kernel - zum Testen in QEMU). */

#include "drivers/gpu/igd_internal.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/spinlock.h"
#include "core/cmdline.h"
#include "core/process.h"
#include "core/syscall.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"

#define COMP_SURFS   96
#define COMP_CHUNK   64          /* Auftraege, die auf einmal aus dem Programm geholt werden */
#define COMP_MAX_W   4096        /* Pixel: Breite in Bytes passt in den Surface State (16384) */
#define COMP_MAX_H   16384
#define SELF_PID     0xFFFFFFFFu /* Flaechen des Selbsttests */

typedef struct {
    uint32_t        pid;   /* 0 = frei */
    void           *shm;   /* Referenz auf das shm-Objekt (0 beim Selbsttest) */
    const uint64_t *frames;
    uint32_t        w, h;  /* Pixel */
    uint32_t        pages; /* belegte Seiten */
    uint32_t        ggtt;  /* erster GGTT-Eintrag, 0 = nicht eingeblendet (CPU-Ersatz) */
} CompSurf;

/* Auftrag aus dem Programm (gleiches Layout in Userland/include/user.h) */
typedef struct {
    uint16_t kind; /* 1 kopieren, 2 mischen */
    uint16_t dst, src;
    uint16_t pad;
    int32_t  dx, dy, sx, sy, w, h;
} GpuOp;

static CompSurf  surfs[COMP_SURFS];
static Spinlock  lock = SPINLOCK_INIT("igdcomp");
static int       mode = -1;     /* -1 = noch nicht geprueft, 0 keins, 1 GPU, 2 CPU-Ersatz */
static int       enabled = 1;   /* igdtest comp off/on */
static int       flush_mode;    /* CPU-Caches vor und nach den Auftraegen zurueckschreiben */
static int       serial_mode;   /* jeden Auftrag einzeln abschicken (falls Auftraege einer Liste sich ueberholen) */
/* Cache: 0 alles uncached, 1 Quellen im LLC (Ziel uncached: aus dem Bildschirmbild liest der Blitter), 2 alles im LLC */
static int       cache_mode = 1, pending_cache = -1;
static const char *const cache_names[3] = {"alles uncached", "Quellen im Cache", "alles im Cache"};
static int       fails;
static uint32_t  region_base, region_end;
static uint64_t  scratch_pte;

static struct {
    uint64_t jobs, ops, us, max_us, busy, errors;
    uint64_t n[2], px[2], t[2]; /* Meldungen des Desktops: 0 CPU, 1 GPU */
} st;

/* ---------- Flaechen ---------- */

static uint32_t region_alloc(uint32_t n)
{
    uint32_t cand = region_base;
    for (int again = 1; again;) {
        again = 0;
        for (int i = 0; i < COMP_SURFS; i++) {
            const CompSurf *s = &surfs[i];
            if (s->pid && s->ggtt && cand < s->ggtt + s->pages && s->ggtt < cand + n) {
                cand = s->ggtt + s->pages;
                again = 1;
            }
        }
    }
    return cand + n <= region_end ? cand : 0;
}

/* Flaeche eintragen und (GPU) einblenden; Nummer + 1 oder 0 */
static int surf_add(uint32_t pid, void *shm, const uint64_t *frames, uint32_t w, uint32_t h)
{
    uint32_t pages = (uint32_t)(((uint64_t)w * h * 4 + 4095) / 4096);
    uint64_t fl = spin_lock(&lock);
    int idx = -1;
    for (int i = 0; i < COMP_SURFS && idx < 0; i++)
        if (!surfs[i].pid)
            idx = i;
    uint32_t base = 0;
    if (idx >= 0 && mode == 1 && !(base = region_alloc(pages)))
        idx = -1;
    if (idx >= 0) {
        CompSurf *s = &surfs[idx];
        s->pid = pid;
        s->shm = shm;
        s->frames = frames;
        s->w = w;
        s->h = h;
        s->pages = pages;
        s->ggtt = base;
        for (uint32_t i = 0; base && i < pages; i++)
            igd_ggtt[base + i] = (frames[i] & ~0xFFFULL) | IGD_PTE_VALID;
    }
    spin_unlock(&lock, fl);
    if (idx >= 0 && base)
        igd_ggtt_flush(); /* der Adress-Cache der Engine wird vor jedem Auftrag verworfen (igd_rcs.c) */
    return idx + 1;
}

static void surf_drop(CompSurf *s)
{
    uint64_t fl = spin_lock(&lock);
    void *shm = s->shm;
    for (uint32_t i = 0; s->ggtt && i < s->pages; i++)
        igd_ggtt[s->ggtt + i] = scratch_pte;
    int had = s->ggtt != 0;
    s->ggtt = 0;
    s->shm = 0;
    s->pid = 0;
    spin_unlock(&lock, fl);
    if (had)
        igd_ggtt_flush();
    shm_put(shm);
}

static CompSurf *surf_get(uint32_t pid, uint32_t handle)
{
    if (handle < 1 || handle > COMP_SURFS)
        return 0;
    CompSurf *s = &surfs[handle - 1];
    return s->pid == pid ? s : 0;
}

void igd_comp_release(uint32_t pid)
{
    for (int i = 0; i < COMP_SURFS; i++)
        if (pid && surfs[i].pid == pid)
            surf_drop(&surfs[i]);
}

/* ---------- Ausfuehren ---------- */

typedef struct {
    int       blend;
    CompSurf *d, *s;
    int       dx, dy, sx, sy, w, h;
} KOp;

/* Rechteck an beide Flaechen anpassen; 0 = leer */
static int clip_op(KOp *o)
{
    if (o->dx < 0) { o->sx -= o->dx; o->w += o->dx; o->dx = 0; }
    if (o->dy < 0) { o->sy -= o->dy; o->h += o->dy; o->dy = 0; }
    if (o->sx < 0) { o->dx -= o->sx; o->w += o->sx; o->sx = 0; }
    if (o->sy < 0) { o->dy -= o->sy; o->h += o->sy; o->sy = 0; }
    if (o->dx + o->w > (int)o->d->w) o->w = (int)o->d->w - o->dx;
    if (o->dy + o->h > (int)o->d->h) o->h = (int)o->d->h - o->dy;
    if (o->sx + o->w > (int)o->s->w) o->w = (int)o->s->w - o->sx;
    if (o->sy + o->h > (int)o->s->h) o->h = (int)o->s->h - o->sy;
    return o->w > 0 && o->h > 0;
}

static inline uint32_t *spx(const CompSurf *s, uint64_t off)
{
    return (uint32_t *)(s->frames[off >> 12] + (off & 0xFFF));
}

/* dieselbe Rechnung wie der Mischen-Kernel: je Byte (s * a + d * (255 - a)) / 255 gerundet, a = Byte 3 der Quelle */
static uint32_t blend_px(uint32_t s, uint32_t d)
{
    uint32_t a = s >> 24, r = 0;
    for (int c = 0; c < 32; c += 8) {
        uint32_t x = ((s >> c) & 0xFF) * a + ((d >> c) & 0xFF) * (255 - a) + 128;
        r |= ((x + (x >> 8)) >> 8) << c;
    }
    return r;
}

/* CPU-Ersatz (gpucomp=soft, Selbsttest): direkt in den Frames */
static void exec_soft(const KOp *o)
{
    for (int r = 0; r < o->h; r++) {
        uint64_t doff = ((uint64_t)(o->dy + r) * o->d->w + (uint64_t)o->dx) * 4;
        uint64_t soff = ((uint64_t)(o->sy + r) * o->s->w + (uint64_t)o->sx) * 4;
        uint32_t left = (uint32_t)o->w;
        while (left) {
            uint32_t n = left, dn = (uint32_t)(4096 - (doff & 0xFFF)) / 4, sn = (uint32_t)(4096 - (soff & 0xFFF)) / 4;
            if (n > dn) n = dn;
            if (n > sn) n = sn;
            uint32_t *dp = spx(o->d, doff);
            const uint32_t *sp = spx(o->s, soff);
            if (!o->blend)
                memcpy(dp, sp, n * 4);
            else
                for (uint32_t i = 0; i < n; i++)
                    dp[i] = blend_px(sp[i], dp[i]);
            left -= n;
            doff += n * 4;
            soff += n * 4;
        }
    }
}

/* Zeilen eines Rechtecks aus dem CPU-Cache zurueckschreiben (nur wenn der Selbsttest es verlangt hat) */
static void flush_rect(const CompSurf *s, int x, int y, int w, int h)
{
    for (int r = 0; r < h; r++) {
        uint64_t off = ((uint64_t)(y + r) * s->w + (uint64_t)x) * 4, end = off + (uint64_t)w * 4;
        while (off < end) {
            uint64_t n = 4096 - (off & 0xFFF);
            if (n > end - off)
                n = end - off;
            igd_clflush((uint64_t)spx(s, off), n);
            off += n;
        }
    }
}

/* Ganze Flaeche ab ihrem Anfang (wie bei igdtest gpgpu). Gemessen: die GPU rundet die Basisadresse auf 32 Byte ab
 * und die Zeilenlaenge auf 64 Byte - deshalb nur Flaechen mit Breite in Vielfachen von 16 Pixeln (siehe anmelden) */
static IgdSurf gsurf(const CompSurf *s, uint32_t mocs)
{
    IgdSurf g = {s->ggtt << 12, s->w * 4, s->h, s->w * 4, mocs};
    return g;
}

/* Auftrag in Stuecke zerlegen, in denen die Bloecke der Threads genau aufgehen (die Hardware schneidet Bloecke am Rand
 * nicht ab): innen 8 x 8 Pixel, rechts ein Streifen mit 1 x 8, unten mit 8 x 1, die Ecke mit 1 x 1. Liefert die Zahl
 * der Stuecke (hoechstens 4). */
static int split_op(const KOp *o, IgdCompOp *g)
{
    int w8 = o->w & ~7, h8 = o->h & ~7, wr = o->w & 7, hr = o->h & 7, n = 0;
    const int part[4][5] = { /* x, y, w, h, Form */
        {0, 0, w8, h8, IGD_BLK_8X8}, {w8, 0, wr, h8, IGD_BLK_1X8}, {0, h8, w8, hr, IGD_BLK_8X1}, {w8, h8, wr, hr, IGD_BLK_1X1},
    };
    for (int i = 0; i < 4; i++) {
        int x = part[i][0], y = part[i][1], w = part[i][2], h = part[i][3], sh = part[i][4];
        if (w <= 0 || h <= 0)
            continue;
        IgdCompOp *p = &g[n++];
        p->blend = o->blend;
        p->shape = sh;
        p->gx = (uint32_t)(sh == IGD_BLK_8X8 || sh == IGD_BLK_8X1 ? w / 8 : w);
        p->gy = (uint32_t)(sh == IGD_BLK_8X8 || sh == IGD_BLK_1X8 ? h / 8 : h);
        p->dst = gsurf(o->d, cache_mode == 2 ? IGD_MOCS_WB : 0);
        p->src = gsurf(o->s, cache_mode >= 1 ? IGD_MOCS_WB : 0);
        p->dx = (uint32_t)(o->dx + x) * 4;
        p->dy = (uint32_t)(o->dy + y);
        p->sx = (uint32_t)(o->sx + x) * 4;
        p->sy = (uint32_t)(o->sy + y);
    }
    return n;
}

/* Stuecke an die Render-Engine; 0 = fertig */
static int submit_gpu(const IgdCompOp *g, int m)
{
    uint64_t us = 0;
    int rc = igd_rcs_comp(g, m, &us);
    if (rc == -1) {
        st.busy++;
        return -1;
    }
    if (rc != 0) {
        st.errors++;
        if (++fails >= 3) {
            kprintf("igdcomp: dreimal Fehler der Render-Engine - Zusammensetzen auf der GPU aus\n");
            enabled = 0;
        }
        return -1;
    }
    st.jobs++;
    st.ops += (uint64_t)m;
    st.us += us;
    if (us > st.max_us)
        st.max_us = us;
    return 0;
}

/* Auftraege ausfuehren; 0 = fertig. serial_mode: jeder Auftrag fuer sich abgeschickt und abgewartet (falls die GPU
 * Auftraege einer Liste trotz PIPE_CONTROL dazwischen ueberlappend ausfuehrt) */
static int exec_ops(const KOp *ops, int n)
{
    if (mode == 2) {
        for (int i = 0; i < n; i++)
            exec_soft(&ops[i]);
        st.jobs++;
        st.ops += (uint64_t)n;
        return 0;
    }
    static IgdCompOp g[IGD_COMP_MAX_OPS];
    int m = 0;
    for (int i = 0; i < n; i++) {
        const KOp *o = &ops[i];
        if (flush_mode) {
            flush_rect(o->s, o->sx, o->sy, o->w, o->h);
            flush_rect(o->d, o->dx, o->dy, o->w, o->h);
        }
        if (m + 4 > IGD_COMP_MAX_OPS) {
            if (submit_gpu(g, m) != 0)
                return -1;
            m = 0;
        }
        m += split_op(o, g + m);
        if (serial_mode) {
            if (submit_gpu(g, m) != 0)
                return -1;
            m = 0;
        }
    }
    if (m && submit_gpu(g, m) != 0)
        return -1;
    if (flush_mode)
        for (int i = 0; i < n; i++)
            flush_rect(ops[i].d, ops[i].dx, ops[i].dy, ops[i].w, ops[i].h);
    return 0;
}

/* ---------- Selbsttest: GPU gegen CPU, ungerade und ueberlappende Rechtecke ---------- */

/* Zeilenlaenge 1088 Byte: Vielfaches von 64, aber keine Zweierpotenz */
enum { TW = 272, TH = 64, TPAGES = (TW * TH * 4 + 4095) / 4096 };

static const int test_ops[][7] = { /* mischen, dx, dy, sx, sy, w, h - alle Blockformen, Raender, Ueberlappungen */
    {0, 13, 5, 7, 3, 77, 29},   {1, 3, 17, 21, 2, 101, 37}, {0, 200, 40, 0, 0, 53, 24},
    {1, 0, 0, 128, 1, 9, 7},    {1, 61, 30, 30, 11, 133, 21}, {0, 247, 0, 3, 50, 6, 14},
    {0, 16, 8, 24, 16, 64, 32}, {1, 40, 24, 8, 8, 32, 16}, /* nur ganze 8 x 8-Bloecke */
};
#define TEST_OPS ((int)(sizeof(test_ops) / sizeof(test_ops[0])))

static uint32_t test_pattern(uint32_t i, uint32_t seed)
{
    uint32_t v = (i + seed) * 2654435761u;
    v ^= v >> 13;
    uint32_t a = (i % 7 == 0) ? 0 : (i % 5 == 0) ? 255 : (v >> 24); /* auch ganz durchsichtig und ganz deckend */
    return (v & 0xFFFFFF) | a << 24;
}

/* Muster fuellen, Auftraege [first, first + count) auf GPU und CPU, vergleichen: falsche Pixel, -1 = GPU haengt */
static int selftest_run(CompSurf *a, CompSurf *b, CompSurf *ra, CompSurf *rb, uint32_t seed, int first, int count)
{
    for (uint32_t i = 0; i < TW * TH; i++) {
        *spx(a, i * 4) = *spx(ra, i * 4) = test_pattern(i, seed);
        *spx(b, i * 4) = *spx(rb, i * 4) = test_pattern(i, seed * 7 + 3);
    }
    KOp ops[TEST_OPS];
    for (int i = 0; i < count; i++) {
        const int *t = test_ops[first + i];
        ops[i] = (KOp){t[0], b, a, t[1], t[2], t[3], t[4], t[5], t[6]};
        KOp ref = ops[i];
        ref.d = rb;
        ref.s = ra;
        exec_soft(&ref);
    }
    if (exec_ops(ops, count) != 0)
        return -1;
    int bad = 0, firstbad = -1;
    for (uint32_t i = 0; i < TW * TH; i++) /* ohne Zurueckschreiben lesen: sieht die CPU, was die GPU schrieb? */
        if (*spx(b, i * 4) != *spx(rb, i * 4)) {
            if (firstbad < 0)
                firstbad = (int)i;
            bad++;
        }
    if (bad)
        kprintf("igdcomp:   Auftraege %d-%d: %d Pixel falsch, erstes bei (%d, %d): GPU %#x, CPU %#x\n", first,
                first + count - 1, bad, firstbad % TW, firstbad / TW, *spx(b, (uint64_t)firstbad * 4),
                *spx(rb, (uint64_t)firstbad * 4));
    return bad;
}

static int selftest(void)
{
    uint64_t mem = pmm_alloc_frames(4 * TPAGES);
    if (!mem) {
        kprintf("igdcomp: kein Speicher fuer den Selbsttest\n");
        return -1;
    }
    static uint64_t fr[4][TPAGES];
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < TPAGES; i++)
            fr[k][i] = mem + ((uint64_t)k * TPAGES + (uint64_t)i) * 4096;
    int ia = surf_add(SELF_PID, 0, fr[0], TW, TH), ib = surf_add(SELF_PID, 0, fr[1], TW, TH);
    CompSurf ra = {SELF_PID, 0, fr[2], TW, TH, TPAGES, 0}, rb = {SELF_PID, 0, fr[3], TW, TH, TPAGES, 0};
    CompSurf *a = ia ? &surfs[ia - 1] : 0, *b = ib ? &surfs[ib - 1] : 0;
    int rc = -1;
    /* Stufen: jeder Auftrag allein, dann alle in einer Liste, sonst alle einzeln nacheinander abgeschickt; das Ganze
     * ohne und (falls noetig) mit Zurueckschreiben der CPU-Caches. Zwei Durchgaenge mit verschiedenen Mustern. */
    for (flush_mode = 0; a && b && rc != 0 && flush_mode < 2; flush_mode++) {
        int single = 0;
        serial_mode = 0;
        for (int i = 0; i < TEST_OPS; i++) {
            int r = selftest_run(a, b, &ra, &rb, 11 + (uint32_t)i, i, 1);
            single += r < 0 ? 100000 : r;
        }
        int list = -1, serial = -1;
        if (!single) {
            list = selftest_run(a, b, &ra, &rb, 41, 0, TEST_OPS);
            if (list == 0)
                list = selftest_run(a, b, &ra, &rb, 97, 0, TEST_OPS);
            if (list != 0) {
                serial_mode = 1;
                serial = selftest_run(a, b, &ra, &rb, 43, 0, TEST_OPS);
                if (serial == 0)
                    serial = selftest_run(a, b, &ra, &rb, 99, 0, TEST_OPS);
            }
        }
        kprintf("igdcomp: Selbsttest %s Zurueckschreiben: einzeln %s, als Liste %s%s\n", flush_mode ? "mit" : "ohne",
                single ? "FALSCH" : "ok", list == 0 ? "ok" : list < 0 && single ? "-" : "FALSCH",
                serial < 0 && list != 0 && !single ? ", nacheinander haengt" : serial == 0 ? ", nacheinander ok"
                                                                                       : serial > 0 ? ", nacheinander FALSCH" : "");
        if (!single && (list == 0 || serial == 0)) {
            rc = 0;
            break;
        }
    }
    if (!a || !b)
        kprintf("igdcomp: Selbsttest: Flaechen nicht eingeblendet\n");
    if (rc != 0)
        flush_mode = serial_mode = 0;
    fails = 0; /* Haenger einzelner Stufen zaehlen nicht fuer den Betrieb */
    enabled = 1;
    if (a)
        surf_drop(a);
    if (b)
        surf_drop(b);
    pmm_free_frames(mem, 4 * TPAGES);
    return rc;
}

static const char *why_off = "-"; /* warum keins (igdtest comp) */

/* Tempo der Cache-Modi messen (Flaeche 2048 x 1024, kopieren und mischen); waehlt 1, wenn es schneller ist als 0 */
static void benchmark(void)
{
    enum { BW = 2048, BH = 1024, BP = BW * BH * 4 / 4096 };
    static uint64_t bf[2][BP];
    uint64_t m0 = pmm_alloc_frames(BP), m1 = m0 ? pmm_alloc_frames(BP) : 0;
    int i0 = 0, i1 = 0;
    if (m1) {
        for (int i = 0; i < BP; i++) {
            bf[0][i] = m0 + (uint64_t)i * 4096;
            bf[1][i] = m1 + (uint64_t)i * 4096;
        }
        for (uint32_t i = 0; i < BW * BH; i++)
            ((uint32_t *)m0)[i] = test_pattern(i, 5);
        i0 = surf_add(SELF_PID, 0, bf[0], BW, BH);
        i1 = surf_add(SELF_PID, 0, bf[1], BW, BH);
    }
    uint64_t mbs[3][2] = {{0}};
    for (int m = 0; i0 && i1 && m < 3; m++) {
        cache_mode = m;
        for (int bl = 0; bl < 2; bl++) {
            KOp o = {bl, &surfs[i1 - 1], &surfs[i0 - 1], 0, 0, 0, 0, BW, BH};
            uint64_t us = 0;
            for (int rep = 0; rep < 2; rep++) { /* der zweite Lauf zaehlt */
                uint64_t t0 = time_us();
                if (exec_ops(&o, 1) != 0)
                    break;
                us = time_us() - t0;
            }
            mbs[m][bl] = us ? (uint64_t)BW * BH * 4 / us : 0;
        }
    }
    kprintf("igdcomp: Tempo %dx%d in MB/s (kopieren/mischen): uncached %lu/%lu, Quellen im Cache %lu/%lu, alles im "
            "Cache %lu/%lu\n", BW, BH, (unsigned long)mbs[0][0], (unsigned long)mbs[0][1], (unsigned long)mbs[1][0],
            (unsigned long)mbs[1][1], (unsigned long)mbs[2][0], (unsigned long)mbs[2][1]);
    /* Zeit fuer kopieren + mischen vergleichen (1 / MB/s) */
    uint64_t t0 = mbs[0][0] && mbs[0][1] ? 1000000 / mbs[0][0] + 1000000 / mbs[0][1] : ~0ULL;
    uint64_t t1 = mbs[1][0] && mbs[1][1] ? 1000000 / mbs[1][0] + 1000000 / mbs[1][1] : ~0ULL;
    cache_mode = t1 <= t0 ? 1 : 0;
    if (i0)
        surf_drop(&surfs[i0 - 1]);
    if (i1)
        surf_drop(&surfs[i1 - 1]);
    if (m0)
        pmm_free_frames(m0, BP);
    if (m1)
        pmm_free_frames(m1, BP);
    memset(&st, 0, sizeof(st));
}

/* Cache-Modus wechseln (igdtest comp cache N; ausgefuehrt beim naechsten Bild des Desktops): erst Selbsttest */
static void apply_cache(int m)
{
    int old = cache_mode;
    cache_mode = m;
    if (selftest() != 0) {
        kprintf("igdcomp: Selbsttest mit \"%s\" falsch - es bleibt bei \"%s\"\n", cache_names[m], cache_names[old]);
        cache_mode = old;
        return;
    }
    memset(&st, 0, sizeof(st));
    kprintf("igdcomp: jetzt %s (Selbsttest ok), Messwerte zurueckgesetzt\n", cache_names[m]);
}

static void comp_init(void)
{
    const char *c = cmdline_get("gpucomp");
    if (c && strcmp(c, "off") == 0) {
        mode = 0;
        why_off = "gpucomp=off in der Kommandozeile";
        kprintf("igdcomp: aus (gpucomp=off)\n");
        return;
    }
    if (c && strcmp(c, "soft") == 0) {
        mode = 2;
        kprintf("igdcomp: CPU-Ersatz im Kernel (gpucomp=soft), Selbsttest %s\n", selftest() == 0 ? "ok" : "FEHLER");
        return;
    }
    mode = 0;
    why_off = "keine passende Intel-GPU (Gen9) bzw. GGTT kleiner als 2 GiB";
    if (!igd_state.gen9 || !igd_regs || !igd_ggtt || igd_ggtt_entries < 0x80000)
        return;
    region_base = igd_ggtt_entries / 2 + 0x58000;
    region_end = igd_ggtt_entries;
    if (region_base + 0x10000 > region_end) {
        kprintf("igdcomp: GGTT zu klein\n");
        why_off = "GGTT zu klein";
        return;
    }
    uint64_t probe;
    for (uint32_t e = region_base; e < region_end; e += 0x4000) /* Stichproben: nichts davon zeigt ins Stolen Memory */
        if (igd_ggtt_claim(e, 1, &probe) != 0) {
            why_off = "GGTT-Bereich ist belegt (zeigt ins Stolen Memory)";
            return;
        }
    scratch_pte = igd_ggtt[region_base];
    mode = 1;
    cache_mode = 1;
    if (selftest() != 0) { /* mit Cache falsch: noch einmal ganz ohne */
        kprintf("igdcomp: Selbsttest mit Quellen im Cache falsch, noch einmal uncached\n");
        cache_mode = 0;
    } else {
        benchmark();
    }
    if (cache_mode == 0 && selftest() != 0) {
        kprintf("igdcomp: Selbsttest fehlgeschlagen - der Desktop setzt weiter mit der CPU zusammen\n");
        why_off = "Selbsttest fehlgeschlagen (Einzelheiten: dmesg | grep igdcomp)";
        mode = 0;
        return;
    }
    kprintf("igdcomp: Selbsttest ok, %s%s%s, GGTT-Bereich %#x-%#x (%u MiB)\n", cache_names[cache_mode],
            serial_mode ? ", Auftraege einzeln" : "", flush_mode ? ", mit Zurueckschreiben der CPU-Caches" : "",
            region_base, region_end, (region_end - region_base) / 256);
}

/* ---------- Systemaufruf ---------- */

int64_t igd_comp_sys(uint32_t pid, uint64_t op, uint64_t a, uint64_t b)
{
    if (mode < 0)
        comp_init();
    if (op == 0)
        return enabled ? mode : 0;
    if (op == 4) { /* Messwert des Desktops */
        int k = a ? 1 : 0;
        st.n[k]++;
        st.t[k] += b & 0xFFFFFFFFu;
        st.px[k] += b >> 32;
        return 0;
    }
    if (mode <= 0)
        return ERR_NOSYS;
    if (op == 1) { /* anmelden: shm-Nummer, breite | hoehe << 16 */
        uint32_t w = (uint32_t)(b & 0xFFFF), h = (uint32_t)((b >> 16) & 0xFFFF);
        if (!w || !h || w > COMP_MAX_W || h > COMP_MAX_H || (w & 15)) /* Zeilenlaenge: Vielfaches von 64 Byte */
            return ERR_INVAL;
        uint64_t npages;
        const uint64_t *frames;
        void *shm = shm_get((uint32_t)a, &npages, &frames);
        if (!shm)
            return ERR_NOENT;
        if ((uint64_t)w * h * 4 > npages * 4096) {
            shm_put(shm);
            return ERR_INVAL;
        }
        int hnd = surf_add(pid, shm, frames, w, h);
        if (!hnd) {
            shm_put(shm);
            return ERR_NOMEM;
        }
        return hnd;
    }
    if (op == 2) { /* abmelden */
        CompSurf *s = surf_get(pid, (uint32_t)a);
        if (!s)
            return ERR_INVAL;
        surf_drop(s);
        return 0;
    }
    if (op == 3) { /* ausfuehren: GpuOp[b] */
        if (!enabled)
            return ERR_AGAIN;
        if (pending_cache >= 0) {
            int m = pending_cache;
            pending_cache = -1;
            apply_cache(m);
        }
        if (b > 4096 || !process_user_range_ok(process_current(), a, b * sizeof(GpuOp), 0))
            return ERR_FAULT;
        static KOp kops[IGD_COMP_MAX_OPS * 4];
        int n = 0;
        for (uint64_t i = 0; i < b; i++) {
            GpuOp u;
            memcpy(&u, (const void *)(a + i * sizeof(GpuOp)), sizeof(u));
            CompSurf *d = surf_get(pid, u.dst), *s = surf_get(pid, u.src);
            if (!d || !s || d == s || (u.kind != 1 && u.kind != 2))
                return ERR_INVAL;
            KOp o = {u.kind == 2, d, s, u.dx, u.dy, u.sx, u.sy, u.w, u.h};
            if (!clip_op(&o))
                continue;
            kops[n++] = o;
            if (n == (int)(sizeof(kops) / sizeof(kops[0])) || i + 1 == b) {
                if (exec_ops(kops, n) != 0)
                    return ERR_AGAIN;
                n = 0;
            }
        }
        if (n && exec_ops(kops, n) != 0)
            return ERR_AGAIN;
        return 0;
    }
    return ERR_INVAL;
}

int igd_comp_cache(int m)
{
    if (m < 0 || m > 2)
        return ERR_INVAL;
    if (mode != 1) {
        kprintf("igdcomp: es wird nicht auf der GPU zusammengesetzt\n");
        return ERR_NOSYS;
    }
    pending_cache = m;
    kprintf("igdcomp: wechselt mit dem naechsten Bild des Desktops zu \"%s\" (vorher Selbsttest)\n", cache_names[m]);
    return 0;
}

int igd_comp_set(int on)
{
    enabled = on != 0;
    if (on)
        fails = 0;
    kprintf("igdcomp: Zusammensetzen auf der GPU %s\n", on ? "an" : "aus (der Desktop setzt mit der CPU zusammen)");
    return 0;
}

static uint64_t per_mpx(uint64_t us, uint64_t px)
{
    return px ? us * 1000000 / px : 0;
}

void igd_comp_report(void)
{
    static const char *names[] = {"keins", "GPU", "CPU-Ersatz (gpucomp=soft)"};
    if (mode < 0) {
        kprintf("igdcomp: noch nicht benutzt (startet mit dem Desktop)\n");
        return;
    }
    int n = 0;
    uint64_t pages = 0;
    for (int i = 0; i < COMP_SURFS; i++)
        if (surfs[i].pid) {
            n++;
            pages += surfs[i].pages;
        }
    kprintf("igdcomp: %s, %s%s%s%s%s; %d Flaechen (%lu MiB)\n", names[mode], enabled ? "an" : "aus",
            mode == 1 ? ", " : "", mode == 1 ? cache_names[cache_mode] : "",
            flush_mode ? ", mit Zurueckschreiben der CPU-Caches" : "", serial_mode ? ", Auftraege einzeln" : "", n,
            (unsigned long)(pages / 256));
    if (mode == 0)
        kprintf("igdcomp: Grund: %s\n", why_off);
    kprintf("igdcomp: %lu Auftragslisten mit %lu Auftraegen, je %lu us auf der GPU (max %lu us); %lu belegt, %lu Fehler\n",
            (unsigned long)st.jobs, (unsigned long)st.ops, (unsigned long)(st.jobs ? st.us / st.jobs : 0),
            (unsigned long)st.max_us, (unsigned long)st.busy, (unsigned long)st.errors);
    for (int k = 1; k >= 0; k--)
        kprintf("igdcomp: Desktop mit %s: %lu Bilder, %lu Mpx, %lu us je Bild, %lu us je Mpx\n", k ? "GPU" : "CPU",
                (unsigned long)st.n[k], (unsigned long)(st.px[k] / 1000000), (unsigned long)(st.n[k] ? st.t[k] / st.n[k] : 0),
                (unsigned long)per_mpx(st.t[k], st.px[k]));
}
