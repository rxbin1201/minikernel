/* Intel-Grafik Gen9, Stufe 5 (Weg zur 3D-Beschleunigung), Schritt 1: die Render-Engine (RCS) starten.
 *
 * Wie beim Blitter (igd_blt.c) im klassischen Ring-Modus: der Kernel schreibt Befehle in einen Ringpuffer, die Engine
 * arbeitet sie ab. Geprueft wird nur der Befehlsprozessor (Command Streamer) der Render-Engine, noch nichts von der
 * 3D-Pipeline oder den Recheneinheiten (EUs):
 *   1. Ring starten, Leerauftrag mit Rueckmeldung (MI_STORE_DATA_IMM in die Statusseite)
 *   2. PIPE_CONTROL: wartet, bis die Engine alles erledigt hat, und schreibt dann einen 64-Bit-Wert (so wird spaeter
 *      das Ende von 3D-Arbeit gemeldet)
 *   3. Batch-Buffer: der Ring springt in einen eigenen Befehlspuffer und kommt zurueck (darueber laeuft spaeter alle
 *      3D-Arbeit); darin ein Wert und der Zeitstempel der Engine
 *   4. Zeitmessung: 1000 Speicherbefehle in einem Batch, gemessen mit dem Zeitstempel der GPU (12 MHz)
 * Haengt die Engine, wird ihr Zustand ausgegeben und sie zurueckgesetzt (GDRST). Nur auf Befehl: igdtest render.
 * Registerangaben nach Intels "Programmer's Reference Manual" (Skylake/Kaby Lake) und dem Linux-i915. */

#include "drivers/gpu/igd_internal.h"
#include "arch/x86_64/apic.h"
#include "core/sched.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"

/* ---------- Register der Render-Engine ---------- */

#define RCS              0x2000
#define R_TAIL           (RCS + 0x30)
#define R_HEAD           (RCS + 0x34)
#define R_START          (RCS + 0x38)
#define R_CTL            (RCS + 0x3C)
#define R_IPEIR          (RCS + 0x64)
#define R_IPEHR          (RCS + 0x68)           /* zuletzt ausgefuehrter Befehl */
#define R_INSTDONE       (RCS + 0x6C)
#define R_ACTHD          (RCS + 0x74)           /* wo die Engine gerade liest */
#define R_HWS_PGA        (RCS + 0x80)           /* Statusseite (GGTT) */
#define R_MI_MODE        (RCS + 0x9C)
#define R_EIR            (RCS + 0xB0)
#define R_ESR            (RCS + 0xB8)
#define R_BBADDR         (RCS + 0x140)          /* Batch-Buffer, in dem die Engine gerade ist */
#define R_BBSTATE        (RCS + 0x110)
#define R_GFX_MODE       (RCS + 0x29C)
#define R_TIMESTAMP      (RCS + 0x358)          /* Zeitstempel der Engine (Gen9: 12 MHz) */
#define STOP_RING        (1u << 8)
#define MODE_IDLE        (1u << 9)
#define RUN_LIST_ENABLE  (1u << 15)
#define GDRST            0x941C
#define GRDOM_RENDER     (1u << 1)
#define RPSTAT1          0xA01C
#define MASKED_ON(b)     (((b) << 16) | (b))
#define MASKED_OFF(b)    ((b) << 16)

/* ---------- Befehle (Gen8+, 64-Bit-Adressen, alle Adressen in der GGTT) ---------- */

#define MI_NOOP               0u
#define MI_BATCH_BUFFER_END   (0x0Au << 23)
#define MI_STORE_DATA_IMM     ((0x20u << 23) | (1u << 22) | 2)   /* 4 Dwords: Kopf, Adresse lo/hi, Wert */
#define MI_STORE_REG_MEM      ((0x24u << 23) | (1u << 22) | 2)   /* 4 Dwords: Kopf, Register, Adresse lo/hi */
#define MI_BATCH_BUFFER_START ((0x31u << 23) | 1)                /* 3 Dwords; Bit 8 = 0: Adresse in der GGTT */
#define PIPE_CONTROL          ((3u << 29) | (3u << 27) | (2u << 24) | 4) /* 6 Dwords */
#define PC_CS_STALL           (1u << 20)
#define PC_WRITE_QWORD        (1u << 14)                         /* nach getaner Arbeit 64 Bit schreiben */
#define PC_GLOBAL_GTT         (1u << 24)

#define RING_BYTES  (16 * 1024)
#define HWS_SEQNO   0x100

static uint32_t *ring;
static uint32_t  ring_gtt, hws_gtt, tail, pos, seqno;
static volatile uint32_t *hws;

static void dump(const char *why)
{
    kprintf("igdrcs: %s: HEAD %#x TAIL %#x CTL %#x START %#x ACTHD %#x BBADDR %#x\n", why, igd_rd(R_HEAD), igd_rd(R_TAIL),
            igd_rd(R_CTL), igd_rd(R_START), igd_rd(R_ACTHD), igd_rd(R_BBADDR));
    kprintf("igdrcs:   IPEHR %#x IPEIR %#x INSTDONE %#x EIR %#x ESR %#x MI_MODE %#x GFX_MODE %#x\n", igd_rd(R_IPEHR),
            igd_rd(R_IPEIR), igd_rd(R_INSTDONE), igd_rd(R_EIR), igd_rd(R_ESR), igd_rd(R_MI_MODE), igd_rd(R_GFX_MODE));
}

static int ring_start(void)
{
    igd_wr(R_HWS_PGA, hws_gtt);
    (void)igd_rd(R_HWS_PGA);
    igd_wr(R_GFX_MODE, MASKED_OFF(RUN_LIST_ENABLE)); /* klassischer Ring statt Execlists */
    igd_wr(R_MI_MODE, MASKED_ON(STOP_RING));
    WAIT_UNTIL(igd_rd(R_MI_MODE) & MODE_IDLE, 50);
    igd_wr(R_CTL, 0);
    igd_wr(R_HEAD, 0);
    igd_wr(R_TAIL, 0);
    igd_wr(R_START, ring_gtt);
    igd_wr(R_CTL, ((RING_BYTES - 4096) & 0x1FF000) | 1);
    int ok = WAIT_UNTIL((igd_rd(R_CTL) & 1) && igd_rd(R_START) == ring_gtt && (igd_rd(R_HEAD) & 0x1FFFFC) == 0, 50);
    igd_wr(R_MI_MODE, MASKED_OFF(STOP_RING));
    tail = 0;
    return ok;
}

static void ring_stop(void)
{
    igd_wr(R_MI_MODE, MASKED_ON(STOP_RING));
    WAIT_UNTIL(igd_rd(R_MI_MODE) & MODE_IDLE, 50);
    igd_wr(R_CTL, 0);
    igd_wr(R_HEAD, 0);
    igd_wr(R_TAIL, 0);
    igd_wr(R_START, 0);
}

static void emit(uint32_t v)
{
    ring[pos / 4] = v;
    pos += 4;
}

/* Auftrag beginnen (der Ring ist vor jedem Auftrag leer: jeder wird abgewartet) */
static void begin(uint32_t dwords)
{
    pos = tail;
    if (pos + dwords * 4 + 64 > RING_BYTES) {
        while (pos < RING_BYTES)
            emit(MI_NOOP);
        pos = 0;
    }
}

/* Abschliessen: laufende Nummer in die Statusseite, abschicken; Ergebnis die Nummer */
static uint32_t submit(void)
{
    uint32_t n = ++seqno;
    emit(MI_STORE_DATA_IMM);
    emit(hws_gtt + HWS_SEQNO);
    emit(0);
    emit(n);
    if (pos & 7) /* TAIL auf 8 Byte */
        emit(MI_NOOP);
    uint32_t start = tail > pos ? 0 : tail;
    igd_clflush((uint64_t)ring + start, pos - start);
    if (tail > pos)
        igd_clflush((uint64_t)ring + tail, RING_BYTES - tail);
    tail = pos % RING_BYTES;
    igd_wr(R_TAIL, tail);
    return n;
}

static int wait_seqno(uint32_t n, int timeout_ms)
{
    uint64_t end = time_ms() + (uint64_t)timeout_ms;
    for (;;) {
        igd_clflush((uint64_t)&hws[HWS_SEQNO / 4], 4);
        if ((int32_t)(hws[HWS_SEQNO / 4] - n) >= 0)
            return 1;
        if (time_ms() > end)
            return 0;
        __asm__ __volatile__("pause");
    }
}

/* Abschicken und abwarten; haengt die Engine: Zustand ausgeben, zuruecksetzen */
static int run(const char *what, int timeout_ms, uint64_t *us)
{
    uint64_t t0 = time_us();
    uint32_t n = submit();
    if (wait_seqno(n, timeout_ms)) {
        uint64_t d = time_us() - t0;
        if (us)
            *us = d;
        kprintf("igdrcs: %s: fertig nach %lu us\n", what, (unsigned long)d);
        return 0;
    }
    kprintf("igdrcs: %s: KEINE Rueckmeldung nach %d ms\n", what, timeout_ms);
    dump("Zustand");
    igd_wr(GDRST, GRDOM_RENDER);
    kprintf("igdrcs: Render-Engine zurueckgesetzt: %s\n", WAIT_UNTIL(!(igd_rd(GDRST) & GRDOM_RENDER), 100) ? "ok" : "haengt");
    return -1;
}

static uint32_t rd_scratch(volatile uint32_t *p)
{
    igd_clflush((uint64_t)p, 4);
    return *p;
}

int igd_render_test(void)
{
    int pre = igd_preflight("Stufe 5 - Render-Engine");
    if (pre)
        return pre;

    /* GGTT: Ring (4) + Statusseite (1) + Batch (4) + Ergebnisse (1), getrennt von den anderen Tests */
    uint32_t base = igd_ggtt_entries / 2 + 0x50000, pages = 10;
    uint64_t saved[10];
    int rc = igd_ggtt_claim(base, pages, saved);
    if (rc)
        return rc;
    uint64_t mem = pmm_alloc_frames(pages);
    if (!mem) {
        kprintf("igdrcs: kein Speicher\n");
        return -6;
    }
    memset((void *)mem, 0, pages * 4096);
    igd_clflush(mem, pages * 4096);
    for (uint32_t i = 0; i < pages; i++)
        igd_ggtt[base + i] = (mem + i * 4096) | IGD_PTE_VALID;
    igd_ggtt_flush();
    ring = (uint32_t *)mem;
    hws = (volatile uint32_t *)(mem + 4 * 4096);
    uint32_t *batch = (uint32_t *)(mem + 5 * 4096);
    volatile uint32_t *res = (volatile uint32_t *)(mem + 9 * 4096);
    ring_gtt = base << 12;
    hws_gtt = (base + 4) << 12;
    uint32_t batch_gtt = (base + 5) << 12, res_gtt = (base + 9) << 12;
    seqno = 0;

    /* 1. Grafikkern wecken, Engine ansehen, Ring starten, Leerauftrag */
    if (!igd_forcewake_get()) {
        kprintf("igdrcs: Forcewake nicht bestaetigt\n");
        rc = -9;
        goto out_fw;
    }
    kprintf("igdrcs: GPU-Takt jetzt %u MHz\n", ((igd_rd(RPSTAT1) >> 23) & 0x1FF) * 50 / 3);
    dump("vorher");
    if (!ring_start()) {
        dump("Ring startet NICHT");
        rc = -10;
        goto out_ring;
    }
    dump("Ring gestartet");
    begin(8);
    if (run("1. Leerauftrag (nur die laufende Nummer)", 500, 0) != 0) {
        rc = -11;
        goto out_ring;
    }

    /* 2. PIPE_CONTROL: nach getaner Arbeit 64 Bit schreiben */
    begin(6 + 8);
    emit(PIPE_CONTROL);
    emit(PC_CS_STALL | PC_WRITE_QWORD | PC_GLOBAL_GTT);
    emit(res_gtt);
    emit(0);
    emit(0x55667788u);
    emit(0x11223344u);
    if (run("2. PIPE_CONTROL mit 64-Bit-Schreiben", 500, 0) != 0) {
        rc = -12;
        goto out_ring;
    }
    uint32_t lo = rd_scratch(&res[0]), hi = rd_scratch(&res[1]);
    kprintf("igdrcs: PIPE_CONTROL schrieb %#x%08x (%s)\n", hi, lo, lo == 0x55667788u && hi == 0x11223344u ? "richtig" : "FALSCH");
    if (lo != 0x55667788u || hi != 0x11223344u) {
        rc = -13;
        goto out_ring;
    }

    /* 3. Batch-Buffer: Wert schreiben, Zeitstempel ablegen, zurueck in den Ring */
    uint32_t k = 0;
    batch[k++] = MI_STORE_DATA_IMM;
    batch[k++] = res_gtt + 16;
    batch[k++] = 0;
    batch[k++] = 0x0BA7C4ED;
    batch[k++] = MI_STORE_REG_MEM;
    batch[k++] = R_TIMESTAMP;
    batch[k++] = res_gtt + 32;
    batch[k++] = 0;
    batch[k++] = MI_BATCH_BUFFER_END;
    batch[k++] = MI_NOOP;
    igd_clflush((uint64_t)batch, k * 4);
    begin(3 + 1 + 8);
    emit(MI_BATCH_BUFFER_START);
    emit(batch_gtt);
    emit(0);
    emit(MI_NOOP);
    if (run("3. Batch-Buffer", 500, 0) != 0) {
        rc = -14;
        goto out_ring;
    }
    uint32_t v = rd_scratch(&res[4]), ts0 = rd_scratch(&res[8]);
    kprintf("igdrcs: Batch schrieb %#x (%s), Zeitstempel der Engine %u\n", v, v == 0x0BA7C4ED ? "richtig" : "FALSCH", ts0);
    if (v != 0x0BA7C4ED) {
        rc = -15;
        goto out_ring;
    }

    /* 4. 1000 Speicherbefehle in einem Batch, mit Zeitstempeln davor und danach */
    k = 0;
    batch[k++] = MI_STORE_REG_MEM;
    batch[k++] = R_TIMESTAMP;
    batch[k++] = res_gtt + 64;
    batch[k++] = 0;
    for (int i = 0; i < 1000; i++) {
        batch[k++] = MI_STORE_DATA_IMM;
        batch[k++] = res_gtt + 128 + (uint32_t)(i % 64) * 4;
        batch[k++] = 0;
        batch[k++] = (uint32_t)i;
    }
    batch[k++] = MI_STORE_REG_MEM;
    batch[k++] = R_TIMESTAMP;
    batch[k++] = res_gtt + 68;
    batch[k++] = 0;
    batch[k++] = MI_BATCH_BUFFER_END;
    batch[k++] = MI_NOOP;
    igd_clflush((uint64_t)batch, k * 4); /* 4010 Dwords: passt in die vier Seiten */
    begin(3 + 1 + 8);
    emit(MI_BATCH_BUFFER_START);
    emit(batch_gtt);
    emit(0);
    emit(MI_NOOP);
    uint64_t cpu_us = 0;
    if (run("4. Batch mit 1000 Speicherbefehlen", 1000, &cpu_us) != 0) {
        rc = -16;
        goto out_ring;
    }
    uint32_t t1 = rd_scratch(&res[16]), t2 = rd_scratch(&res[17]), last = rd_scratch(&res[32 + 999 % 64]);
    uint32_t ticks = t2 - t1;
    kprintf("igdrcs: 1000 Befehle: %u Zeitstempel-Takte (~%u us bei 12 MHz), von aussen gemessen %lu us, letzter Wert %u (%s)\n",
            ticks, ticks / 12, (unsigned long)cpu_us, last, last == 999 ? "richtig" : "FALSCH");
    if (last != 999)
        rc = -17;
    else
        kprintf("igdrcs: Render-Engine funktioniert (Ring, PIPE_CONTROL, Batch-Buffer)\n");

out_ring:
    ring_stop();
    dump("danach");
out_fw:
    igd_forcewake_put();
    for (uint32_t i = 0; i < pages; i++)
        igd_ggtt[base + i] = saved[i];
    igd_ggtt_flush();
    pmm_free_frames(mem, pages);
    kprintf("igdtest: %s\n", rc == 0 ? "Render-Engine funktioniert" : "Render-Engine mit Fehlern, bitte Log schicken");
    return rc;
}
