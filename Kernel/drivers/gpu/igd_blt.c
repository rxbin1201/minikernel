/* Intel-Grafik Gen9, Stufe 3: Blitter-Engine (BCS). Rechtecke fuellen und kopieren durch die GPU.
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
#define GDRST                 0x941C                   /* Reset einzelner Engines */
#define GRDOM_BLT             (1u << 3)
#define MASKED_ON(b)          (((b) << 16) | (b))       /* "maskierte" Register: obere 16 Bit waehlen die Bits aus */
#define MASKED_OFF(b)         ((b) << 16)

/* ---------- Befehle (Gen8+, 64-Bit-Adressen) ---------- */

#define MI_NOOP               0u
#define MI_FLUSH_DW_POSTSYNC  ((0x26u << 23) | 2)  /* MI_FLUSH_DW mit Schreibbefehl: 4 Dwords */
#define MI_FLUSH_DW_STOREDW   (1u << 14)
#define MI_FLUSH_DW_USE_GTT   (1u << 2)
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
    return WAIT_UNTIL((igd_rd(FORCEWAKE_ACK_RENDER) & 1) && (igd_rd(FORCEWAKE_ACK_BLITTER) & 1), 50);
}

static void forcewake_put(void)
{
    igd_wr(FORCEWAKE_BLITTER, MASKED_OFF(1));
    igd_wr(FORCEWAKE_RENDER, MASKED_OFF(1));
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

    /* 3. Geschwindigkeit: 10 ganze Bilder fuellen, GPU gegen CPU, im verdeckten Puffer B */
    if (igd_flip_ready) {
        uint64_t bytes = (uint64_t)igd_scr_stride * igd_scr_h * 10;
        uint64_t t0 = time_us();
        begin(10 * 7);
        for (int i = 0; i < 10; i++)
            emit_fill(igd_surf_b, igd_scr_stride, 0, 0, (int)igd_scr_w, (int)igd_scr_h, 0x00102030u * (uint32_t)i);
        int ok = run("10 ganze Bilder fuellen (GPU)", 2000) == 0;
        uint64_t gpu_us = time_us() - t0;
        t0 = time_us();
        for (int i = 0; i < 10; i++) {
            uint32_t *b = (uint32_t *)igd_buf_b, c = 0x00102030u * (uint32_t)i;
            for (uint64_t k = 0; k < (uint64_t)igd_scr_stride / 4 * igd_scr_h; k++)
                b[k] = c;
        }
        uint64_t cpu_us = time_us() - t0;
        if (ok)
            kprintf("igdblt: 10 x %ux%u fuellen: GPU %lu ms (%lu MB/s), CPU %lu ms (%lu MB/s)\n", igd_scr_w, igd_scr_h,
                    (unsigned long)(gpu_us / 1000), (unsigned long)(bytes / (gpu_us ? gpu_us : 1)),
                    (unsigned long)(cpu_us / 1000), (unsigned long)(bytes / (cpu_us ? cpu_us : 1)));
        if (!ok) {
            rc = -14;
            goto out_ring;
        }
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
    kprintf("igdtest: %s\n", rc == 0 ? "Blitter funktioniert" : "Blitter mit Fehlern, bitte Log schicken");
    return rc;
}
