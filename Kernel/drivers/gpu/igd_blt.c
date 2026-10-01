/* Intel-Grafik Gen9, Stufe 3: Blitter-Engine (BCS). Rechtecke fuellen und kopieren durch die GPU; im Dauerbetrieb
 * uebernimmt sie die Bild-Updates der Grafikprogramme (igd_blt_copy_user), igdtest blit prueft sie einzeln.
 *
 * Die Engine arbeitet Befehle aus einem Ringpuffer ab (klassischer Ring-Modus, keine Execlists): der Kernel schreibt
 * Befehle ans Ende und schiebt RING_TAIL weiter, die Engine liest ab RING_HEAD. Hinter jedem Auftrag steht MI_FLUSH_DW
 * mit einem Schreibbefehl: er leert die Caches der Engine und schreibt eine laufende Nummer in die Statusseite. Daran
 * sieht der Kernel, dass alles davor fertig ist. Alle Adressen in den Befehlen sind Adressen in der GGTT.
 * Vor jedem Zugriff muss der Grafikkern (GT) aus dem Stromsparzustand geholt werden (Forcewake).
 * Registerangaben nach Intels "Programmer's Reference Manual" (Skylake/Kaby Lake) und dem Linux-i915. */

#include "drivers/gpu/igd_internal.h"
#include "arch/x86_64/apic.h"
#include "console/console.h"
#include "core/cmdline.h"
#include "mm/paging.h"
#include "core/sched.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"

/* ---------- Register ---------- */

#define BCS                   0x22000                  /* Blitter-Engine */
#define RING_TAIL             (BCS + 0x30)
#define RING_HEAD             (BCS + 0x34)
#define RING_START            (BCS + 0x38)
#define RING_CTL              (BCS + 0x3C)             /* (Groesse - 4 KiB) | 1 = an */
#define RING_IPEIR            (BCS + 0x64)
#define RING_IPEHR            (BCS + 0x68)             /* zuletzt ausgefuehrter Befehl */
#define RING_INSTDONE         (BCS + 0x6C)
#define RING_ACTHD            (BCS + 0x74)             /* wo die Engine gerade liest */
#define RING_HWS_PGA          (BCS + 0x80)             /* Statusseite (GGTT) */
#define RING_MI_MODE          (BCS + 0x9C)
#define RING_EIR              (BCS + 0xB0)
#define RING_ESR              (BCS + 0xB8)
#define RING_GFX_MODE         (BCS + 0x29C)
#define STOP_RING             (1u << 8)
#define MODE_IDLE             (1u << 9)
#define RUN_LIST_ENABLE       (1u << 15)               /* Execlists: bleibt aus */
#define FORCEWAKE_RENDER      0xA278
#define FORCEWAKE_BLITTER     0xA188
#define FORCEWAKE_ACK_RENDER  0x0D84
#define FORCEWAKE_ACK_BLITTER 0x130044
#define FORCEWAKE_MEDIA       0xA270
#define FORCEWAKE_ACK_MEDIA   0x0D88
#define RP_STATE_CAP          0x145998                 /* Taktbereich: RP0 (max) 7:0, RP1 15:8, RPn (min) 23:16, je 50 MHz */
#define RPSTAT1               0xA01C                   /* aktueller Takt (CAGF) in Bits 31:23, Einheit 50/3 MHz */
#define RPNSWREQ              0xA008                   /* gewuenschter Takt in Bits 31:23, Einheit 50/3 MHz */
#define GDRST                 0x941C                   /* Reset einzelner Engines */
#define GRDOM_BLT             (1u << 3)
#define MASKED_ON(b)          (((b) << 16) | (b))       /* "maskierte" Register: obere 16 Bit waehlen die Bits aus */
#define MASKED_OFF(b)         ((b) << 16)

/* ---------- Befehle (Gen8+, 64-Bit-Adressen) ---------- */

#define MI_NOOP               0u
#define MI_FLUSH_DW_POSTSYNC  ((0x26u << 23) | 2)  /* MI_FLUSH_DW mit Schreibbefehl: 4 Dwords */
#define MI_FLUSH_DW_STOREDW   (1u << 14)
#define MI_FLUSH_DW_USE_GTT   (1u << 2)
#define MI_INVALIDATE_TLB     (1u << 18)               /* MI_FLUSH_DW: Adress-Cache der Engine verwerfen (braucht Post-Sync) */
#define HWS_SCRATCH           0x200                     /* Statusseite: Ziel des Post-Sync beim TLB-Verwerfen */
#define XY_COLOR_BLT          ((2u << 29) | (0x50u << 22) | (3u << 20) | 5) /* 7 Dwords, alle 4 Bytes schreiben */
#define XY_SRC_COPY_BLT       ((2u << 29) | (0x53u << 22) | (3u << 20) | 8) /* 10 Dwords */
#define BLT_DEPTH_32          (3u << 24)
#define ROP_PATCOPY           (0xF0u << 16)
#define ROP_SRCCOPY           (0xCCu << 16)

#define RING_BYTES            (16 * 1024)
#define HWS_SEQNO             0x100                     /* Offset der laufenden Nummer in der Statusseite */

static uint32_t *ring;           /* CPU-Adresse (RAM am Stueck) */
static uint32_t  ring_gtt, hws_gtt;
static volatile uint32_t *hws;
static uint32_t  tail;           /* Bytes */
static uint32_t  seqno;
static int       ring_ok;

/* ---------- Forcewake ---------- */

static int forcewake_get(void)
{
    igd_wr(FORCEWAKE_RENDER, MASKED_ON(1));
    igd_wr(FORCEWAKE_BLITTER, MASKED_ON(1));
    igd_wr(FORCEWAKE_MEDIA, MASKED_ON(1));
    return WAIT_UNTIL((igd_rd(FORCEWAKE_ACK_RENDER) & 1) && (igd_rd(FORCEWAKE_ACK_BLITTER) & 1) &&
                      (igd_rd(FORCEWAKE_ACK_MEDIA) & 1), 50);
}

static void forcewake_put(void)
{
    igd_wr(FORCEWAKE_MEDIA, MASKED_OFF(1));
    igd_wr(FORCEWAKE_BLITTER, MASKED_OFF(1));
    igd_wr(FORCEWAKE_RENDER, MASKED_OFF(1));
}

/* auch fuer die anderen Engines (igd_rcs.c) */
int igd_forcewake_get(void)
{
    return forcewake_get();
}

void igd_forcewake_put(void)
{
    forcewake_put();
}

/* ---------- Ring ---------- */

static void dump_engine(const char *why)
{
    kprintf("igdblt: %s: HEAD %#x TAIL %#x CTL %#x START %#x ACTHD %#x\n", why, igd_rd(RING_HEAD), igd_rd(RING_TAIL),
            igd_rd(RING_CTL), igd_rd(RING_START), igd_rd(RING_ACTHD));
    kprintf("igdblt:   IPEHR %#x IPEIR %#x INSTDONE %#x EIR %#x ESR %#x MI_MODE %#x GFX_MODE %#x\n", igd_rd(RING_IPEHR),
            igd_rd(RING_IPEIR), igd_rd(RING_INSTDONE), igd_rd(RING_EIR), igd_rd(RING_ESR), igd_rd(RING_MI_MODE),
            igd_rd(RING_GFX_MODE));
}

static int ring_start(void)
{
    igd_wr(RING_HWS_PGA, hws_gtt);
    (void)igd_rd(RING_HWS_PGA);
    igd_wr(RING_GFX_MODE, MASKED_OFF(RUN_LIST_ENABLE)); /* klassischer Ring statt Execlists */
    igd_wr(RING_MI_MODE, MASKED_ON(STOP_RING));
    WAIT_UNTIL(igd_rd(RING_MI_MODE) & MODE_IDLE, 50);
    igd_wr(RING_CTL, 0);
    igd_wr(RING_HEAD, 0);
    igd_wr(RING_TAIL, 0);
    igd_wr(RING_START, ring_gtt);
    igd_wr(RING_CTL, ((RING_BYTES - 4096) & 0x1FF000) | 1);
    int ok = WAIT_UNTIL((igd_rd(RING_CTL) & 1) && igd_rd(RING_START) == ring_gtt && (igd_rd(RING_HEAD) & 0x1FFFFC) == 0, 50);
    igd_wr(RING_MI_MODE, MASKED_OFF(STOP_RING));
    tail = 0;
    return ok;
}

static void ring_stop(void)
{
    igd_wr(RING_MI_MODE, MASKED_ON(STOP_RING));
    WAIT_UNTIL(igd_rd(RING_MI_MODE) & MODE_IDLE, 50);
    igd_wr(RING_CTL, 0);
    igd_wr(RING_HEAD, 0);
    igd_wr(RING_TAIL, 0);
    igd_wr(RING_START, 0);
}

static uint32_t emit_pos;

static void emit(uint32_t v)
{
    ring[emit_pos / 4] = v;
    emit_pos += 4;
}

/* Platz fuer n Dwords am Stueck; am Ende des Rings wird mit MI_NOOP aufgefuellt und vorne weitergemacht.
 * Der Ring ist vor jedem Auftrag leer (wir warten jeden Auftrag ab). */
static void begin(uint32_t dwords)
{
    emit_pos = tail;
    if (emit_pos + dwords * 4 + 64 > RING_BYTES) {
        while (emit_pos < RING_BYTES)
            emit(MI_NOOP);
        emit_pos = 0;
    }
}

/* Auftrag abschliessen: Flush + laufende Nummer, abschicken; Ergebnis: die Nummer */
static uint32_t submit(void)
{
    uint32_t n = ++seqno;
    emit(MI_FLUSH_DW_POSTSYNC | MI_FLUSH_DW_STOREDW);
    emit((hws_gtt + HWS_SEQNO) | MI_FLUSH_DW_USE_GTT);
    emit(0);
    emit(n);
    if (emit_pos & 7) /* TAIL muss auf 8 Byte ausgerichtet sein */
        emit(MI_NOOP);
    uint32_t start = tail > emit_pos ? 0 : tail;
    igd_clflush((uint64_t)ring + start, emit_pos - start);
    if (tail > emit_pos) /* umgebrochen: auch das Stueck bis zum Ende */
        igd_clflush((uint64_t)ring + tail, RING_BYTES - tail);
    tail = emit_pos % RING_BYTES;
    igd_wr(RING_TAIL, tail);
    return n;
}

static int wait_seqno(uint32_t n, int timeout_ms)
{
    uint64_t end = time_ms() + (uint64_t)timeout_ms;
    for (;;) {
        igd_clflush((uint64_t)&hws[HWS_SEQNO / 4], 4); /* die GPU hat in den Speicher geschrieben */
        if ((int32_t)(hws[HWS_SEQNO / 4] - n) >= 0)
            return 1;
        if (time_ms() > end)
            return 0;
        __asm__ __volatile__("pause");
    }
}

static void emit_fill(uint32_t dst, uint32_t pitch, int x, int y, int w, int h, uint32_t color)
{
    emit(XY_COLOR_BLT);
    emit(BLT_DEPTH_32 | ROP_PATCOPY | pitch);
    emit(((uint32_t)y << 16) | (uint32_t)x);
    emit(((uint32_t)(y + h) << 16) | (uint32_t)(x + w));
    emit(dst);
    emit(0);
    emit(color);
}

static void emit_copy(uint32_t dst, uint32_t dpitch, int dx, int dy, uint32_t src, uint32_t spitch, int sx, int sy,
                      int w, int h)
{
    emit(XY_SRC_COPY_BLT);
    emit(BLT_DEPTH_32 | ROP_SRCCOPY | dpitch);
    emit(((uint32_t)dy << 16) | (uint32_t)dx);
    emit(((uint32_t)(dy + h) << 16) | (uint32_t)(dx + w));
    emit(dst);
    emit(0);
    emit(((uint32_t)sy << 16) | (uint32_t)sx);
    emit(spitch);
    emit(src);
    emit(0);
}

/* Auftrag abschicken und abwarten; bei Zeitueberschreitung Zustand ausgeben und die Engine zuruecksetzen */
static int run(const char *what, int timeout_ms)
{
    uint64_t t0 = time_us();
    uint32_t n = submit();
    if (wait_seqno(n, timeout_ms)) {
        kprintf("igdblt: %s: fertig nach %lu us\n", what, (unsigned long)(time_us() - t0));
        return 0;
    }
    kprintf("igdblt: %s: KEINE Rueckmeldung nach %d ms\n", what, timeout_ms);
    dump_engine("Zustand");
    igd_wr(GDRST, GRDOM_BLT);
    kprintf("igdblt: Blitter-Reset %s\n", WAIT_UNTIL(!(igd_rd(GDRST) & GRDOM_BLT), 100) ? "ok" : "haengt");
    ring_ok = 0;
    return -1;
}

/* ---------- GPU-Takt ---------- */

static uint32_t cur_mhz(void)
{
    return ((igd_rd(RPSTAT1) >> 23) & 0x1FF) * 50 / 3;
}

/* Misst Fuellen und Kopieren ganzer Bilder im verdeckten Puffer B (Kopie: aus A); Ergebnis in MB/s */
static int bench(const char *label)
{
    uint64_t bytes = (uint64_t)igd_scr_stride * igd_scr_h * 10;
    uint64_t t0 = time_us();
    begin(10 * 7);
    for (int i = 0; i < 10; i++)
        emit_fill(igd_surf_b, igd_scr_stride, 0, 0, (int)igd_scr_w, (int)igd_scr_h, 0x00102030u * (uint32_t)i);
    if (run("10 Bilder fuellen", 3000) != 0)
        return -1;
    uint64_t fill_us = time_us() - t0;
    t0 = time_us();
    begin(10 * 10);
    for (int i = 0; i < 10; i++)
        emit_copy(igd_surf_b, igd_scr_stride, 0, 0, igd_surf_a, igd_scr_stride, 0, 0, (int)igd_scr_w, (int)igd_scr_h);
    if (run("10 Bilder kopieren", 3000) != 0)
        return -1;
    uint64_t copy_us = time_us() - t0;
    kprintf("igdblt: %s (GPU-Takt %u MHz): fuellen %lu MB/s (%lu ms je Bild), kopieren %lu MB/s (%lu ms je Bild)\n",
            label, cur_mhz(), (unsigned long)(bytes / (fill_us ? fill_us : 1)), (unsigned long)(fill_us / 10000),
            (unsigned long)(bytes / (copy_us ? copy_us : 1)), (unsigned long)(copy_us / 10000));
    return 0;
}

/* ---------- Dauerbetrieb: Bild-Updates der Grafikprogramme per Blitter ----------
 *
 * Statt die CPU jedes fertige Bild (bzw. jeden geaenderten Ausschnitt) in den Bildspeicher kopieren zu lassen, kopiert
 * der Blitter. Dafuer wird der Bildpuffer des Programms (normaler Speicher im Adressraum des Programms) Seite fuer
 * Seite in ein Fenster der GGTT eingeblendet; das passiert nur, wenn sich der Puffer aendert. Teil-Updates werden nur
 * in Auftrag gegeben (der Aufruf kehrt sofort zurueck, die GPU kopiert im Hintergrund); ganze Bilder kopiert der
 * Blitter in den verdeckten Puffer, der Kernel wartet darauf (und laesst dabei andere Threads laufen) und schaltet um.
 *
 * Caching: entscheidend ist die Cache-Steuerung der Blitter-Engine (MOCS, 0xCC00..). Mit den Werten der Firmware
 * (und auch mit Write-Through) blieben Schreibzugriffe teils im Last-Level-Cache, die Display-Engine las dort noch
 * alten Inhalt aus dem RAM: Striche im Bild (auf dem Test-PC ausprobiert, igdtest bltmode). Mit "uncached" gehen sie
 * gleich in den RAM; das Programmbild liest die GPU trotzdem richtig (auch ohne clflush der CPU). Standard ist daher
 * Modus 5. Bei jedem Umschalten prueft ein Selbsttest, ob die GPU frisch von der CPU geschriebene Daten sieht; wenn
 * nicht, schreibt die CPU das Programmbild vor dem Kopieren aus ihrem Cache zurueck (wie Modus 6).
 * Die PPAT (Eintrag 0: Write-Through/LLC) setzt der Treiber ebenfalls, da die Firmware sie nicht einrichtet.
 * "noblt" in der Kommandozeile schaltet das ab. */

#define PPAT_LO     0x40E0
#define PPAT_HI     0x40E4
#define PPAT_SET_LO 0x000A0906u /* 0: Write-Through/LLC (alles ueber die GGTT), 1: WC, 2: WT/LLC+eLLC, 3: UC */
#define PPAT_SET_HI 0x3B2B1B0Bu /* 4-7: WB/LLC+eLLC mit Alter 0-3 (wie i915) */
#define WIN_PAGES   10240   /* Fenster fuer Programmbilder: 40 MiB (3840x2160x4 passt) */
#define SMALL_PX    16384   /* kleinere Ausschnitte kopiert die CPU (der Auftrag kostet mehr als das Kopieren) */

static int       blt_on;                        /* Bild-Updates per Blitter (bereit und Modus != 0) */
static int       blt_ready;                     /* Engine laeuft, Selbsttest bestanden */
static int       blt_mode;                      /* igdtest bltmode N (Standard 0 = aus) */
static int       src_flush;                     /* Programmbild vor dem Kopieren aus dem CPU-Cache schreiben */
static uint32_t  mocs_orig[62];
static int       mocs_saved;
static int       reserve(uint32_t dwords);
static uint32_t  kick(void);
static uint64_t  st_phys;                       /* Selbsttest: Quelle (Seite 0) und Ziel (Seite 1) */
static uint32_t  st_src, st_dst;                /* ihre GGTT-Adressen */

/* Die CPU schreibt ein Muster (ohne clflush, es liegt noch in ihrem Cache), der Blitter kopiert es, die CPU vergleicht.
 * Ergebnis: falsche Pixel (0 = ok), -1 = keine Rueckmeldung */
static int selftest(uint64_t *us)
{
    uint32_t *s = (uint32_t *)st_phys, *d = (uint32_t *)(st_phys + 4096);
    int bad = 0;
    for (int round = 0; round < 2 && !bad; round++) {
        memset(d, 0, 4096);
        igd_clflush((uint64_t)d, 4096);
        for (int i = 0; i < 1024; i++)
            s[i] = (round ? 0x005A3C1Eu : 0x00A5C3E1u) ^ ((uint32_t)i * 2654435761u);
        uint64_t t0 = time_us();
        if (!reserve(10 + 6))
            return -1;
        emit_copy(st_dst, 128, 0, 0, st_src, 128, 0, 0, 32, 32);
        uint32_t n = kick();
        if (!wait_seqno(n, 100))
            return -1;
        if (us)
            *us = time_us() - t0;
        igd_clflush((uint64_t)d, 4096);
        for (int i = 0; i < 1024; i++)
            bad += d[i] != s[i];
    }
    return bad;
}
#define BLT_MOCS(i) (0xCC00 + 4u * (uint32_t)(i)) /* Cache-Steuerung der Blitter-Engine (62 Eintraege) */
static int       part_wait;                     /* "bltwait": auch Teil-Updates abwarten (zur Fehlersuche) */
static int       tlb_stale;                     /* GGTT-Fenster geaendert: vor dem naechsten Auftrag TLB verwerfen */
static uint32_t  pte_wb, pte_uc;                /* PAT-Bits im GGTT-Eintrag: Programmbilder bzw. Bildpuffer */
static uint32_t  win_base;                      /* GGTT-Index des Fensters */
static uint64_t *win_phys;                      /* eingeblendete Seiten */
static void     *win_as;                        /* Adressraum, Startadresse und Seitenzahl der Einblendung */
static uint64_t  win_va;
static uint32_t  win_n;
static uint32_t  last_n;                        /* Nummer des zuletzt abgeschickten Auftrags */

static struct {
    uint64_t part_n, part_px, part_us;          /* Teil-Updates: Anzahl, Pixel, CPU-Zeit fuers Abschicken */
    uint64_t full_n, full_us, full_max_us;      /* ganze Bilder: Zeit bis der Blitter fertig ist */
    uint64_t remaps, cpu_fallback;
} bs;

static uint64_t old_pat;
static int      pat_set; /* 1: der Treiber hat die PPAT umgeschrieben */

static void restore_pat(void)
{
    if (!pat_set)
        return;
    forcewake_get();
    igd_wr(PPAT_LO, (uint32_t)old_pat);
    igd_wr(PPAT_HI, (uint32_t)(old_pat >> 32));
    forcewake_put();
    pat_set = 0;
    kprintf("igdblt: alte PPAT wiederhergestellt\n");
}

static void blt_fail(const char *why)
{
    kprintf("igdblt: %s - Bild-Updates wieder per CPU\n", why);
    dump_engine("Zustand");
    forcewake_get();
    igd_wr(GDRST, GRDOM_BLT);
    WAIT_UNTIL(!(igd_rd(GDRST) & GRDOM_BLT), 100);
    forcewake_put();
    blt_on = blt_ready = 0;
    win_as = 0;
    restore_pat();
}

/* Auftrag mit n Dwords beginnen; am Ende des Rings erst warten, bis alles fertig ist (dann ist Umbrechen sicher) */
static int reserve(uint32_t dwords)
{
    emit_pos = tail;
    if (emit_pos + dwords * 4 + 64 > RING_BYTES) {
        if (!wait_seqno(last_n, 200))
            return 0;
        while (emit_pos < RING_BYTES)
            emit(MI_NOOP);
        emit_pos = 0;
    }
    return 1;
}

static uint32_t kick(void)
{
    forcewake_get();
    uint32_t n = submit();
    forcewake_put();
    last_n = n;
    return n;
}

int igd_blt_on(void)
{
    return blt_on;
}

/* Wartet, bis der Blitter alles erledigt hat (vor jedem Schreiben der CPU in die Bildpuffer). 0 = ok */
int igd_blt_sync(void)
{
    if (!blt_ready)
        return 0;
    if (wait_seqno(last_n, 200))
        return 0;
    blt_fail("Blitter antwortet nicht");
    return -1;
}

/* Programmbild (Adressraum des laufenden Prozesses) ins Fenster einblenden; 1 = Bereich [va, va+bytes) ist drin */
static int window_map(uint64_t va, uint64_t bytes)
{
    void *as = as_current();
    uint64_t first = va & ~0xFFFULL, last = (va + bytes - 1) & ~0xFFFULL;
    uint32_t need = (uint32_t)((last - first) / 4096 + 1);
    if (need > WIN_PAGES)
        return 0;
    if (win_as == as && first >= win_va && last < win_va + (uint64_t)win_n * 4096) {
        /* noch dieselben Seiten? (Erste und letzte pruefen: ein neuer Puffer an derselben Stelle hat andere) */
        uint64_t p0, p1;
        uint32_t i0 = (uint32_t)((first - win_va) / 4096), i1 = (uint32_t)((last - win_va) / 4096);
        if (as_translate(as, first, &p0, 0) && as_translate(as, last, &p1, 0) && (p0 & ~0xFFFULL) == win_phys[i0] &&
            (p1 & ~0xFFFULL) == win_phys[i1])
            return 1;
    }
    if (igd_blt_sync() != 0) /* die GPU liest vielleicht noch aus der alten Einblendung */
        return 0;
    /* Auch Seiten davor (hoechstens die Haelfte des freien Platzes) und danach: spaetere Ausschnitte desselben Bildes
     * liegen dann meist schon im Fenster */
    uint64_t phys;
    for (uint32_t back = 0; back < (WIN_PAGES - need) / 2 && first >= 4096 && as_translate(as, first - 4096, &phys, 0); back++)
        first -= 4096;
    uint32_t n = 0;
    for (; n < WIN_PAGES; n++) {
        if (!as_translate(as, first + (uint64_t)n * 4096, &phys, 0))
            break;
        phys &= ~0xFFFULL;
        win_phys[n] = phys;
        igd_ggtt[win_base + n] = phys | IGD_PTE_VALID | pte_wb;
    }
    igd_ggtt_flush();
    bs.remaps++;
    tlb_stale = 1; /* der Blitter hat die alten Eintraege vielleicht noch in seinem TLB */
    win_va = first;
    win_n = n;
    int ok = (va & ~0xFFFULL) + (uint64_t)need * 4096 <= first + (uint64_t)n * 4096;
    win_as = ok ? as : 0;
    return ok;
}

/* Rechteck (w x h) aus dem Programmbild src (Zeilenlaenge pitch Pixel) nach (x, y) in den Bildpuffer dst (GGTT-Adresse)
 * kopieren lassen. 1 = in Auftrag gegeben, 0 = geht nicht (dann kopiert die CPU; vorher igd_blt_sync). */
int igd_blt_copy_user(uint32_t dst, const uint32_t *src, uint32_t pitch, int x, int y, int w, int h, int wait)
{
    if (!blt_on)
        return 0;
    if (!wait && (uint64_t)w * (uint64_t)h < SMALL_PX)
        return 0;
    uint64_t t0 = time_us(), va = (uint64_t)src, bytes = ((uint64_t)(h - 1) * pitch + (uint64_t)w) * 4;
    if (pitch * 4 > 32767 || !window_map(va, bytes)) {
        bs.cpu_fallback++;
        return 0;
    }
    /* Quelle: Seitenanfang im Fenster, Lage des Rechtecks darin als Koordinaten */
    if (src_flush) /* Modus mit Zurueckschreiben: was noch im CPU-Cache liegt, in den RAM */
        for (int r = 0; r < h; r++)
            igd_clflush(va + (uint64_t)r * pitch * 4, (uint64_t)w * 4);
    uint64_t off = va - win_va, page_off = off & ~0xFFFULL, in = off & 0xFFF;
    uint32_t sgtt = (win_base << 12) + (uint32_t)page_off, spitch = pitch * 4;
    int sx = (int)((in % spitch) / 4), sy = (int)(in / spitch);
    if (!reserve(4 + 10 + 6)) {
        blt_fail("Ring laeuft nicht leer");
        return 0;
    }
    if (tlb_stale) { /* wie i915 nach GGTT-Aenderungen: Flush mit TLB-Verwerfen (und Post-Sync in die Statusseite) */
        emit(MI_FLUSH_DW_POSTSYNC | MI_FLUSH_DW_STOREDW | MI_INVALIDATE_TLB);
        emit((hws_gtt + HWS_SCRATCH) | MI_FLUSH_DW_USE_GTT);
        emit(0);
        emit(0);
        tlb_stale = 0;
    }
    emit_copy(dst, igd_scr_stride, x, y, sgtt, spitch, sx, sy, w, h);
    uint32_t n = kick();
    if (!wait && !part_wait) {
        bs.part_n++;
        bs.part_px += (uint64_t)w * (uint64_t)h;
        bs.part_us += time_us() - t0;
        return 1;
    }
    /* ganzes Bild: auf den Blitter warten, dabei andere Threads laufen lassen */
    uint64_t end = time_ms() + 200;
    for (;;) {
        igd_clflush((uint64_t)&hws[HWS_SEQNO / 4], 4);
        if ((int32_t)(hws[HWS_SEQNO / 4] - n) >= 0)
            break;
        if (time_ms() > end) {
            blt_fail("Blitter wird mit einem Bild nicht fertig");
            return 0;
        }
        thread_yield();
    }
    uint64_t d = time_us() - t0;
    if (!wait) { /* Teil-Update mit "bltwait" */
        bs.part_n++;
        bs.part_px += (uint64_t)w * (uint64_t)h;
        bs.part_us += d;
        return 1;
    }
    bs.full_n++;
    bs.full_us += d;
    if (d > bs.full_max_us)
        bs.full_max_us = d;
    return 1;
}

void igd_blt_report(void)
{
    forcewake_get();
    uint32_t mhz = cur_mhz();
    forcewake_put();
    if (!blt_on) {
        kprintf("igdinfo: Blitter: aus (Bild-Updates per CPU)%s\n", blt_ready ? ", bereit: igdtest bltmode N" : "");
        return;
    }
    kprintf("igdinfo: Blitter an (Modus %d), GPU-Takt jetzt %u MHz: %lu Teil-Updates (je %lu Pixel, Abschicken %lu us), "
            "%lu ganze Bilder (je %lu us, max %lu us)\n",
            blt_mode, mhz, (unsigned long)bs.part_n, (unsigned long)(bs.part_n ? bs.part_px / bs.part_n : 0),
            (unsigned long)(bs.part_n ? bs.part_us / bs.part_n : 0), (unsigned long)bs.full_n,
            (unsigned long)(bs.full_n ? bs.full_us / bs.full_n : 0), (unsigned long)bs.full_max_us);
    kprintf("igdinfo: Blitter: %lu Mal Programmbild neu eingeblendet, %lu Mal doch per CPU\n", (unsigned long)bs.remaps,
            (unsigned long)bs.cpu_fallback);
}

/* Beim Start (nach der Doppelpufferung): Ring, Statusseite, Fenster einrichten, PPAT pruefen, Selbsttest */
int igd_blt_init(void)
{
    if (!igd_flip_ready || cmdline_has("noblt")) {
        if (igd_flip_ready)
            kprintf("igdblt: 'noblt': Bild-Updates per CPU\n");
        return -1;
    }
    if (!forcewake_get()) {
        forcewake_put();
        kprintf("igdblt: Grafikkern wacht nicht auf (Forcewake): Bild-Updates per CPU\n");
        return -1;
    }
    uint64_t pat = igd_rd(PPAT_LO) | ((uint64_t)igd_rd(PPAT_HI) << 32);
    forcewake_put();
    kprintf("igdblt: PPAT der Firmware %#lx\n", (unsigned long)pat);
    old_pat = pat;
    pte_wb = pte_uc = 0; /* alles ueber Eintrag 0 */

    /* GGTT: Ring (4) + Statusseite (1) + Selbsttest (2) + Fenster, oberhalb von Puffer B */
    uint32_t base = igd_ggtt_entries / 4 + 0x8000, pages = 4 + 1 + 2 + WIN_PAGES;
    uint64_t *saved = kmalloc(sizeof(uint64_t) * pages);
    win_phys = kmalloc(sizeof(uint64_t) * WIN_PAGES);
    uint64_t ring_phys = pmm_alloc_frames(4), hws_phys = pmm_alloc_frame(), t_phys = pmm_alloc_frames(2);
    if (!saved || !win_phys || !ring_phys || !hws_phys || !t_phys || igd_ggtt_claim(base, pages, saved)) {
        kprintf("igdblt: kein Speicher bzw. GGTT belegt: Bild-Updates per CPU\n");
        kfree(saved);
        return -1;
    }
    kfree(saved); /* die Eintraege gehoeren ab jetzt dauerhaft uns */
    memset((void *)ring_phys, 0, RING_BYTES);
    memset((void *)hws_phys, 0, 4096);
    igd_clflush(ring_phys, RING_BYTES);
    igd_clflush(hws_phys, 4096);
    for (uint32_t i = 0; i < 4; i++)
        igd_ggtt[base + i] = (ring_phys + i * 4096) | IGD_PTE_VALID;
    igd_ggtt[base + 4] = hws_phys | IGD_PTE_VALID;
    igd_ggtt[base + 5] = t_phys | IGD_PTE_VALID | pte_wb;            /* Selbsttest: Quelle wie ein Programmbild */
    igd_ggtt[base + 6] = (t_phys + 4096) | IGD_PTE_VALID | pte_uc;   /* Ziel wie ein Bildpuffer */
    /* Bildpuffer A (Firmware) und B: ebenfalls Eintrag 0 (PAT-Bits loeschen) */
    uint32_t fb_pages = (uint32_t)(((uint64_t)igd_scr_stride * igd_scr_h + 4095) / 4096);
    for (uint32_t s = 0; s < 2; s++) {
        uint32_t first = (s ? igd_surf_b : igd_surf_a) >> 12;
        for (uint32_t i = 0; i < fb_pages; i++) {
            uint64_t e = igd_ggtt[first + i];
            igd_ggtt[first + i] = (e & ~(uint64_t)0x98) | pte_uc; /* PAT-Bits ersetzen, Adresse bleibt */
        }
    }
    igd_ggtt_flush();
    {
        forcewake_get();
        igd_wr(PPAT_LO, PPAT_SET_LO);
        igd_wr(PPAT_HI, PPAT_SET_HI);
        uint64_t now = igd_rd(PPAT_LO) | ((uint64_t)igd_rd(PPAT_HI) << 32);
        forcewake_put();
        pat_set = 1;
        kprintf("igdblt: PPAT jetzt %#lx (Eintrag 0: Write-Through im LLC)%s\n", (unsigned long)now,
                now == (((uint64_t)PPAT_SET_HI << 32) | PPAT_SET_LO) ? "" : " - NICHT uebernommen");
    }
    ring = (uint32_t *)ring_phys;
    hws = (volatile uint32_t *)hws_phys;
    ring_gtt = base << 12;
    hws_gtt = (base + 4) << 12;
    win_base = base + 7;
    tlb_stale = 1;
    seqno = last_n = 0;

    forcewake_get();
    ring_ok = ring_start();
    uint32_t cap = igd_rd(RP_STATE_CAP), rp0 = cap & 0xFF;
    if (rp0)
        igd_wr(RPNSWREQ, (rp0 * 3) << 23); /* hoechster Takt, solange der GT wach ist (im Leerlauf schlaeft er trotzdem) */
    forcewake_put();
    if (!ring_ok) {
        kprintf("igdblt: Ring startet nicht: Bild-Updates per CPU\n");
        restore_pat();
        return -1;
    }

    /* Selbsttest mit den Cache-Werten der Firmware: laeuft die Engine ueberhaupt? */
    st_phys = t_phys;
    st_src = (base + 5) << 12;
    st_dst = (base + 6) << 12;
    blt_ready = 1;
    uint64_t us = 0;
    int bad = selftest(&us);
    if (bad < 0) {
        blt_fail("Selbsttest: keine Rueckmeldung");
        return -1;
    }
    if (bad) {
        kprintf("igdblt: Selbsttest: %d von 1024 Pixeln falsch (GPU sieht die CPU-Daten nicht) - Bild-Updates per CPU\n", bad);
        blt_on = blt_ready = 0;
        restore_pat();
        return -1;
    }
    part_wait = cmdline_has("bltwait");
    forcewake_get();
    kprintf("igdblt: Blitter bereit (Selbsttest ok, %lu us; GPU-Takt bis %u MHz), MOCS[0-3] %#x %#x %#x %#x\n",
            (unsigned long)us, rp0 * 50, igd_rd(BLT_MOCS(0)), igd_rd(BLT_MOCS(1)), igd_rd(BLT_MOCS(2)), igd_rd(BLT_MOCS(3)));
    forcewake_put();
    const char *m = cmdline_get("bltmode");
    igd_blt_set_mode(m && m[0] >= '0' && m[0] <= '6' ? m[0] - '0' : 5);
    return 0;
}

/* Modi (igdtest bltmode N, beim Start bltmode=N); 1-4 zeigen auf dem Test-PC Striche, 5 ist der Standard:
 *   0 aus (CPU kopiert)
 *   1 Blitter, Cache-Steuerung (MOCS) wie vorgefunden      2 wie 1, Programmbild vorher aus dem CPU-Cache schreiben
 *   3 MOCS Write-Through/LLC                                4 wie 3, Programmbild vorher zurueckschreiben
 *   5 MOCS uncached                                         6 wie 5, Programmbild vorher zurueckschreiben */
int igd_blt_set_mode(int mode)
{
    if (mode < 0 || mode > 6)
        return -1;
    if (!blt_ready) {
        kprintf("igdblt: Blitter nicht bereit (siehe dmesg | grep igdblt)\n");
        return -2;
    }
    igd_blt_sync();
    int mocs = mode <= 2 ? 0 : mode <= 4 ? 1 : 2; /* 0 wie vorgefunden, 1 WT/LLC, 2 uncached */
    forcewake_get();
    if (!mocs_saved) {
        for (int i = 0; i < 62; i++)
            mocs_orig[i] = igd_rd(BLT_MOCS(i));
        mocs_saved = 1;
    }
    for (int i = 0; i < 62; i++)
        igd_wr(BLT_MOCS(i), mocs == 0 ? mocs_orig[i] : mocs == 1 ? 0x36u : 0x09u); /* WT|LLC|LRU3 bzw. UC|LLC/eLLC */
    uint32_t now0 = igd_rd(BLT_MOCS(0));
    forcewake_put();
    tlb_stale = 1;
    src_flush = mode == 2 || mode == 4 || mode == 6;
    if (mode) { /* sieht die GPU mit diesen Einstellungen, was die CPU gerade geschrieben hat? */
        int bad = selftest(0);
        if (bad < 0) {
            blt_fail("Selbsttest: keine Rueckmeldung");
            return -3;
        }
        if (bad && !src_flush) {
            kprintf("igdblt: Selbsttest: %d Pixel falsch - das Programmbild wird vor dem Kopieren zurueckgeschrieben\n", bad);
            src_flush = 1;
        }
    }
    blt_mode = mode;
    blt_on = mode != 0;
    static const char *const names[3] = {"wie vorgefunden", "Write-Through/LLC", "uncached"};
    if (mode)
        kprintf("igdblt: Modus %d: Blitter an, MOCS %s (MOCS[0] jetzt %#x)%s\n", mode, names[mocs], now0,
                src_flush ? ", Programmbild vorher aus dem CPU-Cache zurueckschreiben" : "");
    else
        kprintf("igdblt: Modus 0: Bild-Updates per CPU\n");
    return 0;
}

/* ---------- Test (igdtest blit) ---------- */

#define TEST_W     512
#define TEST_H     512
#define TEST_PITCH (TEST_W * 4)
#define TEST_PAGES (TEST_PITCH * TEST_H / 4096)

int igd_blit_test(void)
{
    int pre = igd_preflight("Stufe 3 - Blitter");
    if (pre)
        return pre;

    /* GGTT: Ring (4 Seiten) + Statusseite (1) + Testflaeche (256), oberhalb der igdtest-Bereiche */
    uint32_t base = igd_ggtt_entries / 2 + 0x40000, pages = 4 + 1 + TEST_PAGES;
    uint64_t *saved = kmalloc(sizeof(uint64_t) * pages);
    if (!saved)
        return -6;
    int rc = igd_ggtt_claim(base, pages, saved);
    if (rc) {
        kfree(saved);
        return rc;
    }
    /* Laeuft der Blitter schon fuer die Bild-Updates: anhalten, der Test nimmt einen eigenen Ring; danach weiter */
    int resume = blt_ready;
    uint32_t *keep_ring = ring, keep_rg = ring_gtt, keep_hg = hws_gtt, keep_seq = seqno;
    volatile uint32_t *keep_hws = hws;
    if (resume) {
        igd_blt_sync();
        blt_on = blt_ready = 0;
        forcewake_get();
        ring_stop();
        forcewake_put();
    }
    uint64_t ring_phys = pmm_alloc_frames(4), hws_phys = pmm_alloc_frame(), test_phys = pmm_alloc_frames(TEST_PAGES);
    if (!ring_phys || !hws_phys || !test_phys) {
        kprintf("igdblt: kein Speicher\n");
        rc = -6;
        goto out_free;
    }
    memset((void *)ring_phys, 0, RING_BYTES);
    memset((void *)hws_phys, 0, 4096);
    memset((void *)test_phys, 0, TEST_PAGES * 4096);
    igd_clflush(ring_phys, RING_BYTES);
    igd_clflush(hws_phys, 4096);
    igd_clflush(test_phys, TEST_PAGES * 4096);
    for (uint32_t i = 0; i < 4; i++)
        igd_ggtt[base + i] = (ring_phys + i * 4096) | IGD_PTE_VALID;
    igd_ggtt[base + 4] = hws_phys | IGD_PTE_VALID;
    for (uint32_t i = 0; i < TEST_PAGES; i++)
        igd_ggtt[base + 5 + i] = (test_phys + i * 4096) | IGD_PTE_VALID;
    igd_ggtt_flush();
    ring = (uint32_t *)ring_phys;
    hws = (volatile uint32_t *)hws_phys;
    ring_gtt = base << 12;
    hws_gtt = (base + 4) << 12;
    uint32_t test_gtt = (base + 5) << 12;
    seqno = 0;

    /* 1. Grafikkern wecken, Engine vorher ansehen, Ring starten */
    int fw = forcewake_get();
    kprintf("igdblt: Forcewake %s (ACK Render %#x, Blitter %#x)\n", fw ? "ok" : "NICHT bestaetigt",
            igd_rd(FORCEWAKE_ACK_RENDER), igd_rd(FORCEWAKE_ACK_BLITTER));
    if (!fw) {
        rc = -9;
        goto out_fw;
    }
    dump_engine("vorher");
    ring_ok = ring_start();
    dump_engine(ring_ok ? "Ring gestartet" : "Ring startet NICHT");
    if (!ring_ok) {
        rc = -10;
        goto out_ring;
    }

    /* 2. Unsichtbar: Rechteck fuellen, an eine andere Stelle kopieren, per CPU nachpruefen */
    const uint32_t color = 0x00ABCDEF;
    const int fx = 64, fy = 64, fw_ = 128, fh = 96, cx = 256, cy = 300;
    begin(8);
    emit(MI_NOOP);
    emit(MI_NOOP);
    if (run("Leerauftrag (nur Flush + Nummer)", 500) != 0) {
        rc = -11;
        goto out_ring;
    }
    begin(7 + 10);
    emit_fill(test_gtt, TEST_PITCH, fx, fy, fw_, fh, color);
    emit_copy(test_gtt, TEST_PITCH, cx, cy, test_gtt, TEST_PITCH, fx, fy, fw_, fh);
    if (run("Fuellen + Kopieren (Testflaeche 512x512)", 500) != 0) {
        rc = -12;
        goto out_ring;
    }
    igd_clflush(test_phys, TEST_PAGES * 4096); /* alte Zeilen aus dem CPU-Cache werfen, dann lesen */
    const uint32_t *px = (const uint32_t *)test_phys;
    int bad = 0, first_x = -1, first_y = -1;
    uint32_t first_v = 0;
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++) {
            int in = (x >= fx && x < fx + fw_ && y >= fy && y < fy + fh) || (x >= cx && x < cx + fw_ && y >= cy && y < cy + fh);
            uint32_t want = in ? color : 0, got = px[y * TEST_W + x];
            if (got != want) {
                if (!bad) {
                    first_x = x;
                    first_y = y;
                    first_v = got;
                }
                bad++;
            }
        }
    if (bad)
        kprintf("igdblt: Pruefung: %d falsche Pixel (erstes bei %d,%d: %#x)\n", bad, first_x, first_y, first_v);
    else
        kprintf("igdblt: Pruefung: alle 262144 Pixel richtig (Rechteck gefuellt, Kopie stimmt, Rand unveraendert)\n");
    if (bad) {
        rc = -13;
        goto out_ring;
    }

    /* 3. Geschwindigkeit bei dem Takt, den die Firmware hinterlassen hat, dann beim hoechsten; zum Vergleich die CPU */
    if (igd_flip_ready) {
        uint32_t cap = igd_rd(RP_STATE_CAP), req0 = igd_rd(RPNSWREQ);
        uint32_t rp0 = cap & 0xFF, rp1 = (cap >> 8) & 0xFF, rpn = (cap >> 16) & 0xFF;
        kprintf("igdblt: GPU-Takt: moeglich %u-%u MHz (effizient %u MHz), jetzt %u MHz, angefordert %u MHz (RPNSWREQ %#x)\n",
                rpn * 50, rp0 * 50, rp1 * 50, cur_mhz(), ((req0 >> 23) & 0x1FF) * 50 / 3, req0);
        if (bench("wie vorgefunden") != 0) {
            rc = -14;
            goto out_ring;
        }
        if (rp0) {
            igd_wr(RPNSWREQ, (rp0 * 3) << 23); /* Einheit 50/3 MHz */
            thread_sleep_ms(20);
            kprintf("igdblt: hoechsten Takt angefordert: jetzt %u MHz\n", cur_mhz());
            int b = bench("hoechster Takt");
            igd_wr(RPNSWREQ, req0);
            if (b != 0) {
                rc = -14;
                goto out_ring;
            }
        }
        uint64_t bytes = (uint64_t)igd_scr_stride * igd_scr_h * 10;
        uint64_t t0 = time_us();
        for (int i = 0; i < 10; i++) {
            uint32_t *b = (uint32_t *)igd_buf_b, c = 0x00102030u * (uint32_t)i;
            for (uint64_t k = 0; k < (uint64_t)igd_scr_stride / 4 * igd_scr_h; k++)
                b[k] = c;
        }
        uint64_t cpu_us = time_us() - t0;
        t0 = time_us();
        console_repaint(); /* das, was die Konsole beim Scrollen macht: ganzes Abbild in den Framebuffer */
        uint64_t rep_us = time_us() - t0;
        kprintf("igdblt: CPU: fuellen %lu MB/s (RAM), Konsole ganzes Abbild -> Framebuffer %lu ms (%lu MB/s)\n",
                (unsigned long)(bytes / (cpu_us ? cpu_us : 1)), (unsigned long)(rep_us / 1000),
                (unsigned long)(bytes / 10 / (rep_us ? rep_us : 1)));
    }

    /* 4. Sichtbar im angezeigten Framebuffer A: farbige Rechtecke, dann fuenfmal nach oben scrollen */
    static const uint32_t colors[6] = {0xE04040, 0x40C040, 0x4060E0, 0xE0C040, 0xC040C0, 0x40C0C0};
    int W = (int)igd_scr_w, H = (int)igd_scr_h;
    for (int i = 0; i < 6; i++) {
        begin(7);
        emit_fill(igd_surf_a, igd_scr_stride, W / 8 + i * W / 10, H / 6 + i * H / 12, W / 5, H / 4, colors[i]);
        if (run("Rechteck auf dem Bildschirm", 500) != 0) {
            rc = -15;
            goto out_ring;
        }
        thread_sleep_ms(300);
    }
    int band = H / 8;
    for (int i = 0; i < 5; i++) {
        begin(10 + 7);
        emit_copy(igd_surf_a, igd_scr_stride, 0, 0, igd_surf_a, igd_scr_stride, 0, band, W, H - band);
        emit_fill(igd_surf_a, igd_scr_stride, 0, H - band, W, band, 0x00202020);
        if (run("Bildschirm nach oben scrollen", 500) != 0) {
            rc = -16;
            goto out_ring;
        }
        thread_sleep_ms(300);
    }
    kprintf("igdblt: Blitter funktioniert\n");

out_ring:
    if (ring_ok)
        ring_stop();
    dump_engine("danach");
out_fw:
    forcewake_put();
    console_repaint(); /* was der Test auf den Bildschirm gezeichnet hat, verschwindet wieder */
out_free:
    for (uint32_t i = 0; i < pages; i++)
        igd_ggtt[base + i] = saved[i];
    igd_ggtt_flush();
    if (ring_phys)
        pmm_free_frames(ring_phys, 4);
    if (hws_phys)
        pmm_free_frame(hws_phys);
    if (test_phys)
        pmm_free_frames(test_phys, TEST_PAGES);
    kfree(saved);
    if (resume) { /* Dauerbetrieb mit seinem Ring wieder aufnehmen */
        ring = keep_ring;
        hws = keep_hws;
        ring_gtt = keep_rg;
        hws_gtt = keep_hg;
        seqno = last_n = keep_seq;
        hws[HWS_SEQNO / 4] = seqno;
        igd_clflush((uint64_t)&hws[HWS_SEQNO / 4], 4);
        forcewake_get();
        ring_ok = ring_start();
        forcewake_put();
        blt_ready = ring_ok;
        blt_on = blt_ready && blt_mode;
        kprintf("igdblt: Blitter %s\n", blt_ready ? "wieder bereit" : "startet nicht, Bild-Updates per CPU");
    }
    kprintf("igdtest: %s\n", rc == 0 ? "Blitter funktioniert" : "Blitter mit Fehlern, bitte Log schicken");
    return rc;
}
