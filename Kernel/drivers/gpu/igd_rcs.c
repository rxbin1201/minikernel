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
#include "console/console.h"
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
#define R_RESET_CTL      (RCS + 0xD0)           /* Reset anmelden / bereit */
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

/* Engine zuruecksetzen (wie i915): Reset anmelden, warten bis sie bereit ist, dann GDRST. Danach ist ihr Zustand wie
 * nach dem Einschalten - nichts aus einem frueheren Lauf (Pipeline, Zustaende, Caches) bleibt haengen. */
static int engine_reset(void)
{
    igd_wr(R_RESET_CTL, MASKED_ON(1u));
    int ready = WAIT_UNTIL(igd_rd(R_RESET_CTL) & 2u, 20);
    igd_wr(GDRST, GRDOM_RENDER);
    int ok = WAIT_UNTIL(!(igd_rd(GDRST) & GRDOM_RENDER), 100);
    igd_wr(R_RESET_CTL, MASKED_OFF(1u));
    kprintf("igdrcs: Render-Engine zurueckgesetzt: %s%s\n", ok ? "ok" : "haengt", ready ? "" : " (nicht bereit gemeldet)");
    return ok;
}

static int ring_start(void)
{
    engine_reset();
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

#define PC_TLB_INV     (1u << 18)
#define PC_INSTR_INV   (1u << 11)
#define PC_TEX_INV     (1u << 10)
#define PC_VF_INV      (1u << 4)
#define PC_CONST_INV   (1u << 3)
#define PC_STATE_INV   (1u << 2)
#define HWS_SCRATCH    0x200

/* Auftrag beginnen (der Ring ist vor jedem Auftrag leer: jeder wird abgewartet). Wie bei i915 zuerst ein PIPE_CONTROL,
 * das den Adress-Cache (TLB) und die Caches fuer Befehle, Zustaende, Konstanten und Texturen verwirft: die Engine soll
 * keine Zuordnung oder keinen Kernel eines frueheren Auftrags mehr benutzen. */
static void begin(uint32_t dwords)
{
    pos = tail;
    if (pos + (dwords + 6) * 4 + 64 > RING_BYTES) {
        while (pos < RING_BYTES)
            emit(MI_NOOP);
        pos = 0;
    }
    emit(PIPE_CONTROL);
    emit(PC_CS_STALL | PC_TLB_INV | PC_INSTR_INV | PC_TEX_INV | PC_VF_INV | PC_CONST_INV | PC_STATE_INV | PC_WRITE_QWORD |
         PC_GLOBAL_GTT);
    emit(hws_gtt + HWS_SCRATCH);
    emit(0);
    emit(0);
    emit(0);
}

/* Fester Speicher der Render-Engine: beim ersten Test angelegt und danach behalten (wie beim Blitter). So bleiben die
 * Zuordnungen in der GGTT gleich - neu angelegte Seiten an alten Adressen sah die Engine beim zweiten Lauf noch mit
 * der alten Zuordnung. */
enum { C_RING = 0, C_HWS = 4, C_BATCH = 5, C_STATE = 9, C_RES = 10, C_KERN = 11, C_CKERN = 13, C_CSTATE = 17, C_PAGES = 25 };

/* Die Engine gehoert entweder einem Test (igdtest render/gpgpu) oder dem Zusammensetzen; nach einem Test richtet das
 * Zusammensetzen sie neu ein (comp_ready = 0) */
static volatile int rcs_busy;
static int comp_ready;

static int rcs_acquire(int wait)
{
    while (__sync_lock_test_and_set(&rcs_busy, 1)) {
        if (!wait)
            return 0;
        thread_yield();
    }
    return 1;
}

static void rcs_release(void)
{
    __sync_lock_release(&rcs_busy);
}
static uint64_t core_mem;
static uint32_t core_base;

static uint32_t core_gtt(int page) { return (core_base + (uint32_t)page) << 12; }
static void *core_ptr(int page) { return (void *)(core_mem + (uint64_t)page * 4096); }

static int core_setup(void)
{
    if (!core_mem) {
        uint32_t base = igd_ggtt_entries / 2 + 0x50000;
        uint64_t saved[C_PAGES];
        int rc = igd_ggtt_claim(base, C_PAGES, saved);
        if (rc)
            return rc;
        uint64_t m = pmm_alloc_frames(C_PAGES);
        if (!m) {
            kprintf("igdrcs: kein Speicher\n");
            return -6;
        }
        for (uint32_t i = 0; i < C_PAGES; i++)
            igd_ggtt[base + i] = (m + i * 4096) | IGD_PTE_VALID;
        igd_ggtt_flush();
        core_mem = m;
        core_base = base;
    }
    memset((void *)core_mem, 0, C_PAGES * 4096);
    igd_clflush(core_mem, C_PAGES * 4096);
    ring = (uint32_t *)core_ptr(C_RING);
    hws = (volatile uint32_t *)core_ptr(C_HWS);
    ring_gtt = core_gtt(C_RING);
    hws_gtt = core_gtt(C_HWS);
    seqno = 0;
    return 0;
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
    ring_stop();
    engine_reset();
    return -1;
}

static uint32_t rd_scratch(volatile uint32_t *p)
{
    igd_clflush((uint64_t)p, 4);
    return *p;
}

static int render_test(void)
{
    int pre = igd_preflight("Stufe 5 - Render-Engine");
    if (pre)
        return pre;

    /* Fester Speicher: Ring (4) + Statusseite (1) + Batch (4) + ... + Ergebnisse (1) */
    int rc = core_setup();
    if (rc)
        return rc;
    uint32_t *batch = (uint32_t *)core_ptr(C_BATCH);
    volatile uint32_t *res = (volatile uint32_t *)core_ptr(C_RES);
    uint32_t batch_gtt = core_gtt(C_BATCH), res_gtt = core_gtt(C_RES);

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
    kprintf("igdtest: %s\n", rc == 0 ? "Render-Engine funktioniert" : "Render-Engine mit Fehlern, bitte Log schicken");
    return rc;
}

/* ---------- Stufe 5: Programme auf den Recheneinheiten (igdtest gpgpu) ----------
 *
 * GPGPU-Pipeline der Render-Engine: ein Kernel in EU-Maschinensprache fuellt eine Flaeche mit einem Wert aus dem
 * Konstantenspeicher (CURBE). Grundlage ist der Fuelltest des Linux-Testwerkzeugs IGT (gpgpu_fill, Gen8/Gen9): jeder
 * Hardware-Thread (SIMD16) bekommt seine Gruppen-Nummer (x, y) in r0 und schreibt per "Media Block Write" (Data Port 1)
 * einen Block an die passende Stelle. Der Batch:
 *   PIPELINE_SELECT (GPGPU), STATE_BASE_ADDRESS, MEDIA_VFE_STATE, MEDIA_CURBE_LOAD, MEDIA_INTERFACE_DESCRIPTOR_LOAD,
 *   GPGPU_WALKER (eine Thread-Gruppe je Block), MEDIA_STATE_FLUSH, PIPE_CONTROL (Caches leeren)
 * Zustaende, Kernel und Konstanten liegen in einer Seite; alle Basisadressen zeigen auf sie. Die Cache-Steuerung der
 * Render-Engine (MOCS) steht waehrend des Tests auf uncached (wie beim Blitter: sonst sieht die Anzeige nicht alles).
 *
 * Die Kernel erzeugt ein kleiner Assembler (eu_*): Befehlsformat Gen8/Gen9, 128 Bit je Befehl, nur "align1" und direkte
 * Registeradressen. Er baut den IGT-Kernel zur Probe bitgenau nach. */

/* ---------- EU-Assembler ---------- */

enum { EU_ARF = 0, EU_GRF = 1, EU_IMM = 3 };                                /* Registerdatei */
enum { EU_UD = 0, EU_D = 1, EU_UW = 2, EU_W = 3, EU_UB = 4, EU_B = 5, EU_F = 7 }; /* Datentyp */
enum { EU_MOV = 0x01, EU_ADD = 0x40, EU_MUL = 0x41, EU_SEND = 0x31 };
enum { SFID_SPAWNER = 0x7, SFID_DP1 = 0xC };                               /* Ziele von send */

typedef struct {
    int file, type, reg, sub;    /* sub: Byte im Register */
    int vs, w, hs;               /* Region <vs;w,hs> als Kodierung (0=0/1, 1=1, 2=2, 3=4, 4=8, ...) */
    uint32_t imm;
} EuOp;

static EuOp eu_r(int type, int reg, int sub_bytes, int vs, int w, int hs) /* Register mit Region */
{
    EuOp o = {EU_GRF, type, reg, sub_bytes, vs, w, hs, 0};
    return o;
}
static EuOp eu_d(int type, int reg, int sub_bytes) /* Ziel: Schrittweite 1 */
{
    EuOp o = {EU_GRF, type, reg, sub_bytes, 0, 0, 1, 0};
    return o;
}
static EuOp eu_imm(int type, uint32_t v)
{
    if (type == EU_UW || type == EU_W) /* Wort-Immediates stehen in beiden Haelften */
        v = (v & 0xFFFF) | (v & 0xFFFF) << 16;
    EuOp o = {EU_IMM, type, 0, 0, 0, 0, 0, v};
    return o;
}
static EuOp eu_null(void)
{
    EuOp o = {EU_ARF, EU_UW, 0, 0, 0, 0, 1, 0};
    return o;
}
#define SCALAR 0, 0, 0 /* <0;1,0> */

/* Ein Befehl: op, Ausfuehrungsbreite (log2: 0 = 1 Kanal ... 4 = 16), Bits 27:24 (SFID bei send), Ziel, Quelle 0,
 * Quelle 1 (nur Datei/Typ und das letzte Dword: Immediate bzw. Nachrichtenbeschreibung) */
static void eu_inst(uint32_t *o, int op, int exec, int ctrl, EuOp d, EuOp s0, EuOp s1)
{
    o[0] = (uint32_t)op | (uint32_t)exec << 21 | (uint32_t)ctrl << 24;
    o[1] = (uint32_t)d.file << 3 | (uint32_t)d.type << 5 | (uint32_t)s0.file << 9 | (uint32_t)s0.type << 11 |
           (uint32_t)d.sub << 16 | (uint32_t)d.reg << 21 | (uint32_t)d.hs << 29;
    if (s0.file == EU_IMM) { /* Quelle 0 unmittelbar: steht im letzten Dword */
        o[2] = 0;
        o[3] = s0.imm;
        return;
    }
    o[2] = (uint32_t)s0.sub | (uint32_t)s0.reg << 5 | (uint32_t)s0.hs << 16 | (uint32_t)s0.w << 18 |
           (uint32_t)s0.vs << 21 | (uint32_t)s1.file << 25 | (uint32_t)s1.type << 27;
    if (s1.file == EU_GRF) /* Quelle 1 als Register: gleiche Felder wie Quelle 0, im letzten Dword */
        o[3] = (uint32_t)s1.sub | (uint32_t)s1.reg << 5 | (uint32_t)s1.hs << 16 | (uint32_t)s1.w << 18 | (uint32_t)s1.vs << 21;
    else
        o[3] = s1.imm;
}

static EuOp eu_none(void)
{
    EuOp o = {0, 0, 0, 0, 0, 0, 0, 0};
    return o;
}

/* send: Nachricht ab Register src an sfid mit Beschreibung desc */
static void eu_send(uint32_t *o, int exec, int sfid, EuOp dst, int src, uint32_t desc)
{
    EuOp s0 = {EU_GRF, EU_D, src, 0, 0, 0, 0, 0};
    eu_inst(o, EU_SEND, exec, sfid, dst, s0, eu_imm(EU_D, desc));
}

/* Nachrichtenbeschreibung "Media Block Write" (Data Port 1): Laenge in Registern, mit Kopf, Binding-Table-Eintrag */
static uint32_t mbw_desc(int mlen, int bti)
{
    return (uint32_t)mlen << 25 | 1u << 19 | 0xAu << 14 | (uint32_t)bti;
}
#define EOT_DESC 0x82000010u /* Thread-Ende: 1 Register, EOT-Bit */

/* Der Fuell-Kernel von IGT: 16 Bytes x 1 Zeile je Thread, Fuellwert = ein Byte (r1.0) */
static int asm_fill16(uint32_t (*k)[4])
{
    int n = 0;
    eu_inst(k[n++], EU_MOV, 2, 0, (EuOp){EU_GRF, EU_UB, 1, 0, 0, 0, 1, 0}, eu_r(EU_UB, 1, 0, SCALAR), eu_none());
    eu_inst(k[n++], EU_MUL, 0, 0, eu_d(EU_UD, 2, 0), (EuOp){EU_GRF, EU_UD, 0, 4, 0, 0, 0, 0}, eu_imm(EU_UD, 16));
    eu_inst(k[n++], EU_MOV, 0, 0, eu_d(EU_UD, 2, 4), (EuOp){EU_GRF, EU_UD, 0, 24, 0, 0, 0, 0}, eu_none());
    eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_UD, 4, 0), eu_r(EU_UD, 0, 0, 4, 3, 1), eu_none());
    eu_inst(k[n++], EU_MOV, 1, 0, eu_d(EU_UD, 4, 0), eu_r(EU_UD, 2, 0, 2, 1, 1), eu_none());
    eu_inst(k[n++], EU_MOV, 0, 0, eu_d(EU_UD, 4, 8), eu_imm(EU_UD, 0xF), eu_none());
    eu_inst(k[n++], EU_MOV, 4, 0, eu_d(EU_UD, 5, 0), eu_r(EU_UD, 1, 0, SCALAR), eu_none());
    eu_send(k[n++], 4, SFID_DP1, (EuOp){EU_ARF, EU_UW, 32, 0, 0, 0, 1, 0}, 4, mbw_desc(3, 0));
    eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_UD, 112, 0), eu_r(EU_UD, 0, 0, 4, 3, 1), eu_none());
    eu_send(k[n++], 4, SFID_SPAWNER, eu_null(), 112, EOT_DESC);
    return n;
}

/* Schneller Fuell-Kernel: Block 32 Bytes x 8 Zeilen je Thread, Fuellwert = ein Dword (r1.0, z.B. eine Farbe) */
static int asm_fill32x8(uint32_t (*k)[4])
{
    int n = 0;
    eu_inst(k[n++], EU_MUL, 0, 0, eu_d(EU_UD, 2, 0), (EuOp){EU_GRF, EU_UD, 0, 4, 0, 0, 0, 0}, eu_imm(EU_UD, 32));
    eu_inst(k[n++], EU_MUL, 0, 0, eu_d(EU_UD, 2, 4), (EuOp){EU_GRF, EU_UD, 0, 24, 0, 0, 0, 0}, eu_imm(EU_UD, 8));
    eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_UD, 4, 0), eu_r(EU_UD, 0, 0, 4, 3, 1), eu_none());
    eu_inst(k[n++], EU_MOV, 1, 0, eu_d(EU_UD, 4, 0), eu_r(EU_UD, 2, 0, 2, 1, 1), eu_none());
    eu_inst(k[n++], EU_MOV, 0, 0, eu_d(EU_UD, 4, 8), eu_imm(EU_UD, (7u << 16) | 31), eu_none()); /* 32 x 8 */
    for (int r = 5; r < 13; r += 2) /* r5..r12: 8 Zeilen zu je 32 Bytes */
        eu_inst(k[n++], EU_MOV, 4, 0, eu_d(EU_UD, r, 0), eu_r(EU_UD, 1, 0, SCALAR), eu_none());
    eu_send(k[n++], 4, SFID_DP1, eu_null(), 4, mbw_desc(9, 0));
    eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_UD, 112, 0), eu_r(EU_UD, 0, 0, 4, 3, 1), eu_none());
    eu_send(k[n++], 4, SFID_SPAWNER, eu_null(), 112, EOT_DESC);
    return n;
}

/* Zum Vergleich: der Kernel, wie IGT ihn ausliefert (lib/gpgpu_fill.c) */
static const uint32_t igt_fill_kernel[10][4] = {
    {0x00400001, 0x20202288, 0x00000020, 0x00000000},
    {0x00000041, 0x20400208, 0x06000004, 0x00000010},
    {0x00000001, 0x20440208, 0x00000018, 0x00000000},
    {0x00600001, 0x20800208, 0x008d0000, 0x00000000},
    {0x00200001, 0x20800208, 0x00450040, 0x00000000},
    {0x00000001, 0x20880608, 0x00000000, 0x0000000f},
    {0x00800001, 0x20a00208, 0x00000020, 0x00000000},
    {0x0c800031, 0x24000a40, 0x0e000080, 0x060a8000},
    {0x00600001, 0x2e000208, 0x008d0000, 0x00000000},
    {0x07800031, 0x20000a40, 0x0e000e00, 0x82000010},
};

/* ---------- Kopieren und Mischen ----------
 * Konstanten (r1): Dword 0/1 = Ziel x (Bytes) / y, Dword 2/3 = Quelle x (Bytes) / y. Je Thread ein Block bw x bh Bytes
 * (32 x 8 = 8 x 8 Pixel; fuer Raender auch 4 x 8, 32 x 1, 4 x 1: die Hardware schneidet Bloecke am Rand der Flaeche
 * nicht ab, deshalb muessen sie genau aufgehen). Im Register liegen kleine Bloecke dicht hintereinander (4 Bytes je
 * Zeile bei bw = 4), also immer ganze Pixel. Binding Table: 0 = Ziel (lesen und schreiben), 1 = Quelle. */

enum { EU_OR = 0x06, EU_XOR = 0x07, EU_SHR = 0x08, EU_SHL = 0x09 };

/* Nachrichtenbeschreibung "Media Block Read": Kopf (1 Register), Antwort rlen Register */
static uint32_t mbr_desc(int rlen, int bti)
{
    return 1u << 25 | (uint32_t)rlen << 20 | 1u << 19 | 0x4u << 14 | (uint32_t)bti;
}

/* Kopf fuer einen Block bw x bh Bytes an (Register pos: x, y) in Register hdr */
static int asm_block_header(uint32_t (*k)[4], int n, int hdr, int pos, int bw, int bh)
{
    eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_UD, hdr, 0), eu_r(EU_UD, 0, 0, 4, 3, 1), eu_none());
    eu_inst(k[n++], EU_MOV, 1, 0, eu_d(EU_UD, hdr, 0), eu_r(EU_UD, pos, 0, 2, 1, 1), eu_none());
    eu_inst(k[n++], EU_MOV, 0, 0, eu_d(EU_UD, hdr, 8), eu_imm(EU_UD, (uint32_t)(bh - 1) << 16 | (uint32_t)(bw - 1)),
            eu_none());
    return n;
}

/* r2 = Zielposition, r3 = Quellposition (Gruppe * Blockgroesse + Versatz aus den Konstanten) */
static int asm_positions(uint32_t (*k)[4], int n, int bw, int bh)
{
    eu_inst(k[n++], EU_MUL, 0, 0, eu_d(EU_UD, 2, 0), (EuOp){EU_GRF, EU_UD, 0, 4, 0, 0, 0, 0}, eu_imm(EU_UD, (uint32_t)bw));
    eu_inst(k[n++], EU_MUL, 0, 0, eu_d(EU_UD, 2, 4), (EuOp){EU_GRF, EU_UD, 0, 24, 0, 0, 0, 0}, eu_imm(EU_UD, (uint32_t)bh));
    eu_inst(k[n++], EU_ADD, 1, 0, eu_d(EU_UD, 3, 0), eu_r(EU_UD, 2, 0, 2, 1, 1), eu_r(EU_UD, 1, 8, 2, 1, 1));
    eu_inst(k[n++], EU_ADD, 1, 0, eu_d(EU_UD, 2, 0), eu_r(EU_UD, 2, 0, 2, 1, 1), eu_r(EU_UD, 1, 0, 2, 1, 1));
    return n;
}

static int asm_end(uint32_t (*k)[4], int n)
{
    eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_UD, 112, 0), eu_r(EU_UD, 0, 0, 4, 3, 1), eu_none());
    eu_send(k[n++], 4, SFID_SPAWNER, eu_null(), 112, EOT_DESC);
    return n;
}

static int blk_regs(int bw, int bh) { return (bw * bh + 31) / 32; } /* Register fuer einen Block */

/* Kopieren: Quelle lesen (ab r21), mit Kopf r20 ans Ziel schreiben */
static int asm_copy_blk(uint32_t (*k)[4], int bw, int bh)
{
    int nr = blk_regs(bw, bh);
    int n = asm_positions(k, 0, bw, bh);
    n = asm_block_header(k, n, 4, 3, bw, bh);
    eu_send(k[n++], 4, SFID_DP1, eu_d(EU_UD, 21, 0), 4, mbr_desc(nr, 1));
    n = asm_block_header(k, n, 20, 2, bw, bh);
    eu_send(k[n++], 4, SFID_DP1, eu_null(), 20, mbw_desc(nr + 1, 0));
    return asm_end(k, n);
}

static int asm_copy32x8(uint32_t (*k)[4])
{
    return asm_copy_blk(k, 32, 8);
}

/* Mischen: Quelle (r21..r28) mit ihrem Alpha (Byte 3 jedes Pixels) ueber das Ziel (r29..r36), Ergebnis r51..r58 mit
 * Kopf r50. Je Kanal: x = s*a + d*(255-a); Ergebnis (x + 128 + ((x + 128) >> 8)) >> 8 = x / 255 gerundet.
 * Je halbe Zeile (16 Bytes = 4 Pixel) in Woertern: r40 = x, r41 = 255-a, r42/r43 Zwischenwerte, r44 zum Packen. */
static int asm_blend_blk(uint32_t (*k)[4], int bw, int bh)
{
    int nr = blk_regs(bw, bh);
    int n = asm_positions(k, 0, bw, bh);
    n = asm_block_header(k, n, 4, 3, bw, bh);
    eu_send(k[n++], 4, SFID_DP1, eu_d(EU_UD, 21, 0), 4, mbr_desc(nr, 1));
    n = asm_block_header(k, n, 5, 2, bw, bh);
    eu_send(k[n++], 4, SFID_DP1, eu_d(EU_UD, 29, 0), 5, mbr_desc(nr, 0));
    /* Bytes erst in Woerter umwandeln (mov kann das sicher), dann nur Woerter mit Woertern verrechnen */
    EuOp x = eu_r(EU_UW, 40, 0, 5, 4, 1), ia = eu_r(EU_UW, 41, 0, 5, 4, 1), t = eu_r(EU_UW, 42, 0, 5, 4, 1);
    EuOp u = eu_r(EU_UW, 43, 0, 5, 4, 1), sw = eu_r(EU_UW, 45, 0, 5, 4, 1), aw = eu_r(EU_UW, 46, 0, 5, 4, 1);
    EuOp dw = eu_r(EU_UW, 47, 0, 5, 4, 1);
    for (int row = 0; row < nr; row++)
        for (int half = 0; half < 2; half++) {
            int off = half * 16;
            EuOp s = eu_r(EU_UB, 21 + row, off, 5, 4, 1), d = eu_r(EU_UB, 29 + row, off, 5, 4, 1);
            EuOp a = eu_r(EU_UB, 21 + row, off + 3, 3, 2, 0); /* <4;4,0>: Alpha je Pixel viermal */
            eu_inst(k[n++], EU_MOV, 4, 0, eu_d(EU_UW, 45, 0), s, eu_none());
            eu_inst(k[n++], EU_MOV, 4, 0, eu_d(EU_UW, 46, 0), a, eu_none());
            eu_inst(k[n++], EU_MUL, 4, 0, eu_d(EU_UW, 46, 0), aw, eu_r(EU_UW, 1, 16, SCALAR)); /* a * Deckung */
            eu_inst(k[n++], EU_SHR, 4, 0, eu_d(EU_UW, 46, 0), aw, eu_imm(EU_UW, 8));             /* / 256 */
            eu_inst(k[n++], EU_MOV, 4, 0, eu_d(EU_UW, 47, 0), d, eu_none());
            eu_inst(k[n++], EU_MUL, 4, 0, eu_d(EU_UW, 40, 0), sw, aw);                       /* x = s * a */
            eu_inst(k[n++], EU_XOR, 4, 0, eu_d(EU_UW, 41, 0), aw, eu_imm(EU_UW, 0xFF));      /* 255 - a */
            eu_inst(k[n++], EU_MUL, 4, 0, eu_d(EU_UW, 42, 0), dw, ia);                       /* d * (255 - a) */
            eu_inst(k[n++], EU_ADD, 4, 0, eu_d(EU_UW, 40, 0), x, t);
            eu_inst(k[n++], EU_ADD, 4, 0, eu_d(EU_UW, 40, 0), x, eu_imm(EU_UW, 128));
            eu_inst(k[n++], EU_SHR, 4, 0, eu_d(EU_UW, 43, 0), x, eu_imm(EU_UW, 8));
            eu_inst(k[n++], EU_ADD, 4, 0, eu_d(EU_UW, 40, 0), x, u);
            eu_inst(k[n++], EU_SHR, 4, 0, eu_d(EU_UW, 40, 0), x, eu_imm(EU_UW, 8));
            /* 4 Pixel packen: Kanal c steht in Wort 4p + c; erst als Dword holen, dann schieben und odern */
            int out = 51 + row, osub = off;
            eu_inst(k[n++], EU_MOV, 2, 0, eu_d(EU_UD, out, osub), eu_r(EU_UW, 40, 0, 3, 0, 0), eu_none());
            for (int c = 1; c < 4; c++) {
                EuOp tmp = eu_r(EU_UD, 44, 0, 4, 4, 1);
                eu_inst(k[n++], EU_MOV, 2, 0, eu_d(EU_UD, 44, 0), eu_r(EU_UW, 40, 2 * c, 3, 0, 0), eu_none());
                eu_inst(k[n++], EU_SHL, 2, 0, eu_d(EU_UD, 44, 0), tmp, eu_imm(EU_UD, 8u * c));
                eu_inst(k[n++], EU_OR, 2, 0, eu_d(EU_UD, out, osub), eu_r(EU_UD, out, osub, 4, 4, 1), tmp);
            }
        }
    n = asm_block_header(k, n, 50, 2, bw, bh);
    eu_send(k[n++], 4, SFID_DP1, eu_null(), 50, mbw_desc(nr + 1, 0));
    return asm_end(k, n);
}

static int asm_blend32x8(uint32_t (*k)[4])
{
    return asm_blend_blk(k, 32, 8);
}

/* Skalieren (Animationen) in zwei Durchgaengen mit Bloecken, die die Hardware nachweislich richtig liest/schreibt.
 * Konstanten: r1.0/1 Ziel x (Bytes)/y, r1.2/3 Quelle x (Bytes)/y, r1.5 Schritt (8.8). Ein Thread = 8 x 8 Zielpixel.
 * Senkrecht: Zielzeile y kommt aus Quellzeile sy + (y * step >> 8): 8 Zeilen 32 x 1 lesen, als ein Block schreiben. */
static int asm_vscale(uint32_t (*k)[4])
{
    int n = asm_positions(k, 0, 32, 8); /* r2 = Ziel (x, y), r3.0 = Quelle x */
    eu_inst(k[n++], EU_MUL, 0, 0, eu_d(EU_UD, 6, 0), eu_r(EU_UD, 0, 24, SCALAR), eu_imm(EU_UD, 8)); /* Zeile ty * 8 */
    eu_inst(k[n++], EU_MOV, 0, 0, eu_d(EU_UD, 8, 0), eu_r(EU_UD, 3, 0, SCALAR), eu_none());
    for (int r = 0; r < 8; r++) {
        eu_inst(k[n++], EU_ADD, 0, 0, eu_d(EU_UD, 7, 0), eu_r(EU_UD, 6, 0, SCALAR), eu_imm(EU_UD, (uint32_t)r));
        eu_inst(k[n++], EU_MUL, 0, 0, eu_d(EU_UD, 7, 0), eu_r(EU_UD, 7, 0, SCALAR), eu_r(EU_UW, 1, 20, SCALAR));
        eu_inst(k[n++], EU_SHR, 0, 0, eu_d(EU_UD, 7, 0), eu_r(EU_UD, 7, 0, SCALAR), eu_imm(EU_UD, 8));
        eu_inst(k[n++], EU_ADD, 0, 0, eu_d(EU_UD, 8, 4), eu_r(EU_UD, 7, 0, SCALAR), eu_r(EU_UD, 1, 12, SCALAR));
        n = asm_block_header(k, n, 70 + r, 8, 32, 1);
        eu_send(k[n++], 4, SFID_DP1, eu_d(EU_UD, 21 + r, 0), 70 + r, mbr_desc(1, 1));
    }
    n = asm_block_header(k, n, 20, 2, 32, 8);
    eu_send(k[n++], 4, SFID_DP1, eu_null(), 20, mbw_desc(9, 0));
    return asm_end(k, n);
}

/* Waagerecht: Zielspalte x kommt aus Quellspalte sx + (x * step >> 8): 8 Spalten 4 x 8 lesen und einzeln schreiben
 * (Spalte c liegt in r51 + 2c, ihr Kopf zum Schreiben in r50 + 2c) */
static int asm_hscale(uint32_t (*k)[4])
{
    int n = asm_positions(k, 0, 32, 8); /* r2 = Ziel (x, y), r3.1 = Quelle y */
    eu_inst(k[n++], EU_MUL, 0, 0, eu_d(EU_UD, 6, 0), eu_r(EU_UD, 0, 4, SCALAR), eu_imm(EU_UD, 8)); /* Spalte tx * 8 */
    eu_inst(k[n++], EU_MOV, 0, 0, eu_d(EU_UD, 8, 4), eu_r(EU_UD, 3, 4, SCALAR), eu_none());
    eu_inst(k[n++], EU_MOV, 0, 0, eu_d(EU_UD, 9, 4), eu_r(EU_UD, 2, 4, SCALAR), eu_none());
    for (int c = 0; c < 8; c++) {
        eu_inst(k[n++], EU_ADD, 0, 0, eu_d(EU_UD, 7, 0), eu_r(EU_UD, 6, 0, SCALAR), eu_imm(EU_UD, (uint32_t)c));
        eu_inst(k[n++], EU_MUL, 0, 0, eu_d(EU_UD, 7, 0), eu_r(EU_UD, 7, 0, SCALAR), eu_r(EU_UW, 1, 20, SCALAR));
        eu_inst(k[n++], EU_SHR, 0, 0, eu_d(EU_UD, 7, 0), eu_r(EU_UD, 7, 0, SCALAR), eu_imm(EU_UD, 8));
        eu_inst(k[n++], EU_SHL, 0, 0, eu_d(EU_UD, 7, 0), eu_r(EU_UD, 7, 0, SCALAR), eu_imm(EU_UD, 2)); /* Bytes */
        eu_inst(k[n++], EU_ADD, 0, 0, eu_d(EU_UD, 8, 0), eu_r(EU_UD, 7, 0, SCALAR), eu_r(EU_UD, 1, 8, SCALAR));
        n = asm_block_header(k, n, 70 + c, 8, 4, 8);
        eu_send(k[n++], 4, SFID_DP1, eu_d(EU_UD, 51 + 2 * c, 0), 70 + c, mbr_desc(1, 1));
    }
    for (int c = 0; c < 8; c++) {
        eu_inst(k[n++], EU_ADD, 0, 0, eu_d(EU_UD, 9, 0), eu_r(EU_UD, 2, 0, SCALAR), eu_imm(EU_UD, 4u * (uint32_t)c));
        n = asm_block_header(k, n, 50 + 2 * c, 9, 4, 8);
        eu_send(k[n++], 4, SFID_DP1, eu_null(), 50 + 2 * c, mbw_desc(2, 0));
    }
    return asm_end(k, n);
}

/* Dieselbe Rechnung auf der CPU (zum Vergleich) */
static uint8_t blend_ref(uint8_t s, uint8_t d, uint8_t a)
{
    uint32_t x = (uint32_t)s * a + (uint32_t)d * (255u - a) + 128;
    return (uint8_t)((x + (x >> 8)) >> 8);
}

/* ---------- Batch und Zustaende ---------- */

#define PIPELINE_SELECT_GPGPU   (0x69040000u | (3u << 8) | 2) /* Gen9: Maske fuer die Auswahl-Bits */
#define STATE_BASE_ADDRESS      (0x61010000u | 17)            /* 19 Dwords (Gen9) */
#define MEDIA_VFE_STATE         (0x70000000u | 7)
#define MEDIA_CURBE_LOAD        (0x70010000u | 2)
#define MEDIA_IDD_LOAD          (0x70020000u | 2)
#define MEDIA_STATE_FLUSH       (0x70040000u | 0)
#define GPGPU_WALKER            (0x71050000u | 13)
#define PC_DC_FLUSH             (1u << 5)
#define PC_RT_FLUSH             (1u << 12)
#define GFX_MOCS(i)             (0xC800 + 4u * (uint32_t)(i))  /* Cache-Steuerung der Render-Engine */
#define SURF_R8_UNORM           0x140
#define EU_THREADS              168                            /* GT2: 24 EUs x 7 Threads */
#define KERN_MAX                512                            /* Befehle (2 Seiten) */

/* Lage in der Zustandsseite (Surface- und Dynamic-Basis = Anfang der Seite; Kernel in eigenen Seiten) */
#define ST_CURBE    0x100
#define ST_IDD      0x140
#define ST_BT       0x180   /* 2 Eintraege */
#define ST_SURF0    0x1C0
#define ST_SURF1    0x200

typedef IgdSurf GpuSurf; /* Flaeche: GGTT-Adresse, Breite (Bytes), Hoehe, Zeilenlaenge */

static void surf_state(uint32_t *ss, const GpuSurf *s)
{
    ss[1] = s->mocs << 24;
    ss[0] = (1u << 29) | (SURF_R8_UNORM << 18) | (1u << 16) | (1u << 14); /* 2D, R8, VALIGN4, HALIGN4, linear */
    ss[2] = (s->w - 1) | ((s->h - 1) << 16);
    ss[3] = s->pitch - 1;
    ss[7] = (4u << 25) | (5u << 22) | (6u << 19) | (7u << 16); /* Kanalzuordnung R, G, B, A */
    ss[8] = s->gtt;
}

/* Zustandsseite: Konstanten (8 Dwords), Ziel (Binding Table 0) und Quelle (1, darf fehlen) */
static void gpgpu_state(uint32_t *st, const uint32_t curbe[8], const GpuSurf *dst, const GpuSurf *src)
{
    memset(st, 0, 4096);
    memcpy(st + ST_CURBE / 4, curbe, 32);
    uint32_t *idd = st + ST_IDD / 4;
    idd[0] = 0;              /* Kernel am Anfang der Instruction-Basis */
    idd[2] = 1u << 18;       /* Single Program Flow */
    idd[4] = ST_BT;          /* Binding Table relativ zur Surface-State-Basis, 0 Eintraege vorladen */
    idd[5] = 1u << 16;       /* Konstanten: 1 Register (32 Byte) ab Offset 0 */
    idd[6] = 1;              /* 1 Thread je Gruppe */
    st[ST_BT / 4] = ST_SURF0;
    st[ST_BT / 4 + 1] = ST_SURF1;
    surf_state(st + ST_SURF0 / 4, dst);
    surf_state(st + ST_SURF1 / 4, src ? src : dst);
}

/* Batch: gx x gy Thread-Gruppen, threads = so viele duerfen gleichzeitig laufen */
static uint32_t gpgpu_batch(uint32_t *b, uint32_t st_gtt, uint32_t kern_gtt, uint32_t res_gtt, uint32_t gx, uint32_t gy,
                            uint32_t threads)
{
    uint32_t k = 0;
    b[k++] = PIPELINE_SELECT_GPGPU;
    b[k++] = STATE_BASE_ADDRESS; /* wie IGT: allgemein 0, Surface/Dynamic = Zustandsseite, Instruction = Kernel */
    b[k++] = 0 | 1;
    b[k++] = 0;
    b[k++] = 0 | 1;              /* Stateless Data Port: MOCS */
    b[k++] = st_gtt | 1;         /* Surface State */
    b[k++] = 0;
    b[k++] = st_gtt | 1;         /* Dynamic State */
    b[k++] = 0;
    b[k++] = 0;                  /* Indirect Object */
    b[k++] = 0;
    b[k++] = kern_gtt | 1;       /* Instruction */
    b[k++] = 0;
    b[k++] = 0xFFFFF000u | 1;    /* Groessen */
    b[k++] = (1u << 12) | 1;
    b[k++] = 0xFFFFF000u | 1;
    b[k++] = (2u << 12) | 1;     /* Kernel: 2 Seiten */
    b[k++] = 0 | 1;              /* Bindless Surface State */
    b[k++] = 0;
    b[k++] = 0xFFFFF000u;
    b[k++] = MEDIA_VFE_STATE;
    b[k++] = 0;                  /* kein Scratch */
    b[k++] = 0;
    b[k++] = (threads - 1) << 16 | (1u << 8); /* hoechstens so viele Threads, URB-Eintraege */
    b[k++] = 0;
    b[k++] = (0u << 16) | 1;     /* URB-Eintragsgroesse, CURBE-Groesse */
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = MEDIA_CURBE_LOAD;
    b[k++] = 0;
    b[k++] = 64;
    b[k++] = ST_CURBE;
    b[k++] = MEDIA_IDD_LOAD;
    b[k++] = 0;
    b[k++] = 32;
    b[k++] = ST_IDD;
    b[k++] = GPGPU_WALKER;
    b[k++] = 0;                  /* Interface Descriptor 0 */
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 1u << 30;           /* SIMD16, Thread-Breite/-Hoehe/-Tiefe 1 */
    b[k++] = 0;                  /* Gruppe x ab 0 */
    b[k++] = 0;
    b[k++] = gx;                 /* Gruppen in x */
    b[k++] = 0;                  /* Gruppe y ab 0 */
    b[k++] = 0;
    b[k++] = gy;                 /* Gruppen in y */
    b[k++] = 0;
    b[k++] = 1;
    b[k++] = 0xFFFF;             /* rechte Ausfuehrungsmaske (alle 16 Kanaele) */
    b[k++] = 0xFFFFFFFFu;
    b[k++] = MEDIA_STATE_FLUSH;
    b[k++] = 0;
    b[k++] = PIPE_CONTROL;       /* warten, Caches leeren, dann Merker schreiben */
    b[k++] = PC_CS_STALL | PC_DC_FLUSH | PC_RT_FLUSH | PC_WRITE_QWORD | PC_GLOBAL_GTT;
    b[k++] = res_gtt;
    b[k++] = 0;
    b[k++] = 0x600DF111u;
    b[k++] = 0;
    b[k++] = MI_BATCH_BUFFER_END;
    b[k++] = MI_NOOP;
    return k;
}

static uint32_t *g_st, *g_batch, *g_kern;
static uint32_t g_st_gtt, g_batch_gtt, g_res_gtt, g_kern_gtt;

/* Kernel auf gx x gy Gruppen ausfuehren; 0 = fertig (Zeit in *us) */
static int gpgpu_go(const char *what, const uint32_t (*kern)[4], int nk, const uint32_t curbe[8], const GpuSurf *dst,
                    const GpuSurf *src, uint32_t gx, uint32_t gy, uint32_t threads, uint64_t *us)
{
    memset(g_kern, 0, KERN_MAX * 16);
    memcpy(g_kern, kern, (uint64_t)nk * 16);
    gpgpu_state(g_st, curbe, dst, src);
    uint32_t k = gpgpu_batch(g_batch, g_st_gtt, g_kern_gtt, g_res_gtt, gx, gy, threads);
    igd_clflush((uint64_t)g_kern, KERN_MAX * 16);
    igd_clflush((uint64_t)g_st, 4096);
    igd_clflush((uint64_t)g_batch, k * 4);
    begin(4 + 8);
    emit(MI_BATCH_BUFFER_START);
    emit(g_batch_gtt);
    emit(0);
    emit(MI_NOOP);
    return run(what, 2000, us);
}

/* Fuellen: ganze Flaeche mit value (Kernel 16 x 1 mit Byte bzw. 32 x 8 mit Dword) */
static int gpgpu_fill(const char *what, const uint32_t (*kern)[4], int nk, int bw, int bh, const GpuSurf *s,
                      uint32_t value, uint32_t threads, uint64_t *us)
{
    uint32_t c[8] = {value, 0, 0, 0, 0, 0, 0, 0};
    return gpgpu_go(what, kern, nk, c, s, 0, s->w / (uint32_t)bw, s->h / (uint32_t)bh, threads, us);
}

/* Kopieren bzw. Mischen eines Rechtecks (w Bytes x h Zeilen, Vielfache von 32 bzw. 8) von src (sx, sy) nach dst (dx, dy) */
static int gpgpu_rect(const char *what, const uint32_t (*kern)[4], int nk, const GpuSurf *dst, uint32_t dx, uint32_t dy,
                      const GpuSurf *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, uint64_t *us)
{
    uint32_t c[8] = {dx, dy, sx, sy, 256, 0, 0, 0}; /* Deckung beim Mischen: unveraendert */
    return gpgpu_go(what, kern, nk, c, dst, src, w / 32, h / 8, EU_THREADS, us);
}

static uint64_t mbps(uint64_t bytes, uint64_t us)
{
    return bytes / (us ? us : 1);
}

/* Muster fuer die Tests: Farbe und Alpha je Pixel aus Position und Startwert */
static uint32_t pattern(uint32_t i, uint32_t seed)
{
    uint32_t v = (i + seed) * 2654435761u;
    return v ^ (v >> 13);
}

/* Bildschirmgrosse Flaeche Seite fuer Seite (die GPU sieht sie am Stueck, die CPU je Seite) */
static uint32_t big_rd(const uint64_t *pg, uint64_t off)
{
    volatile uint32_t *p = (volatile uint32_t *)(pg[off / 4096] + off % 4096);
    return *p;
}
static void big_wr(const uint64_t *pg, uint64_t off, uint32_t v)
{
    *(uint32_t *)(pg[off / 4096] + off % 4096) = v;
}

static int gpgpu_test(void)
{
    int pre = igd_preflight("Stufe 5 - Programme auf den Recheneinheiten");
    if (pre)
        return pre;

    /* 0. Assembler: baut er den IGT-Kernel bitgenau nach? Dann die eigenen Kernel */
    static uint32_t k16[16][4], k32[16][4], kcopy[32][4], kblend[KERN_MAX][4];
    int n16 = asm_fill16(k16), n32 = asm_fill32x8(k32), ncopy = asm_copy32x8(kcopy), nblend = asm_blend32x8(kblend);
    int diff = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 4; j++)
            if (k16[i][j] != igt_fill_kernel[i][j])
                diff++;
    if (n16 != 10 || diff || nblend > KERN_MAX) {
        kprintf("igdgpu: Assembler stimmt NICHT mit dem IGT-Kernel ueberein (%d Abweichungen) - Abbruch\n", diff);
        return -30;
    }
    kprintf("igdgpu: Assembler erzeugt den IGT-Kernel bitgenau; Kernel: Fuellen %d, Kopieren %d, Mischen %d Befehle\n",
            n32, ncopy, nblend);

    /* Fester Speicher (Ring, Status, Batch, Zustaende, Ergebnis, Kernel) und je Lauf im GGTT-Bereich dahinter: zwei
     * Testflaechen (je 16 Seiten = 1024 x 64 Bytes) und zwei bildschirmgrosse Flaechen (Seite fuer Seite) */
    int rc = core_setup();
    if (rc)
        return rc;
    enum { P_SA = 0, P_SB = 16, P_BIG = 32 };
    uint32_t big_w = igd_flip_ready ? igd_scr_stride : 13760, big_h = igd_flip_ready ? igd_scr_h : 1440;
    big_w &= ~31u;
    big_h &= ~7u;
    uint32_t big_pages = (uint32_t)(((uint64_t)big_w * big_h + 4095) / 4096), pages = P_BIG + 2 * big_pages;
    uint32_t base = core_base + 0x40;
    uint64_t *saved = kmalloc(sizeof(uint64_t) * pages), *big = kmalloc(sizeof(uint64_t) * 2 * big_pages);
    uint32_t mocs[62];
    rc = !saved || !big ? -6 : igd_ggtt_claim(base, pages, saved);
    if (rc) {
        kfree(saved);
        kfree(big);
        return rc;
    }
    uint64_t mem = pmm_alloc_frames(P_BIG);
    uint32_t got = 0;
    for (; mem && got < 2 * big_pages; got++)
        if (!(big[got] = pmm_alloc_frame()))
            break;
    if (!mem || got < 2 * big_pages) {
        kprintf("igdgpu: kein Speicher\n");
        rc = -6;
        goto out_mem;
    }
    memset((void *)mem, 0, P_BIG * 4096);
    igd_clflush(mem, P_BIG * 4096);
    for (uint32_t i = 0; i < P_BIG; i++)
        igd_ggtt[base + i] = (mem + i * 4096) | IGD_PTE_VALID;
    for (uint32_t i = 0; i < 2 * big_pages; i++)
        igd_ggtt[base + P_BIG + i] = big[i] | IGD_PTE_VALID;
    igd_ggtt_flush();
    g_batch = (uint32_t *)core_ptr(C_BATCH);
    g_st = (uint32_t *)core_ptr(C_STATE);
    g_kern = (uint32_t *)core_ptr(C_KERN);
    volatile uint32_t *res = (volatile uint32_t *)core_ptr(C_RES);
    uint8_t *sa = (uint8_t *)(mem + P_SA * 4096), *sb = (uint8_t *)(mem + P_SB * 4096);
    g_batch_gtt = core_gtt(C_BATCH);
    g_st_gtt = core_gtt(C_STATE);
    g_res_gtt = core_gtt(C_RES);
    g_kern_gtt = core_gtt(C_KERN);
    GpuSurf SA = {(base + P_SA) << 12, 1024, 64, 1024, 0}, SB = {(base + P_SB) << 12, 1024, 64, 1024, 0};
    GpuSurf BIG = {(base + P_BIG) << 12, big_w, big_h, big_w, 0}, BIG2 = {(base + P_BIG + big_pages) << 12, big_w, big_h, big_w, 0};

    if (!igd_forcewake_get()) {
        kprintf("igdgpu: Forcewake nicht bestaetigt\n");
        rc = -9;
        goto out_fw;
    }
    for (int i = 0; i < 62; i++)
        mocs[i] = igd_rd(GFX_MOCS(i));
    kprintf("igdgpu: GPU-Takt %u MHz\n", ((igd_rd(RPSTAT1) >> 23) & 0x1FF) * 50 / 3);
    int ring_ok = ring_start();
    for (int i = 0; i < 62; i++) /* Cache-Steuerung der Render-Engine: uncached (alte Werte zurueck am Ende) */
        igd_wr(GFX_MOCS(i), 0x09);
    if (!ring_ok) {
        dump("Ring startet NICHT");
        rc = -10;
        goto out_ring;
    }

    /* 1. Fuellen (wie beim letzten Mal): schneller Kernel, jedes Dword pruefen */
    uint64_t us = 0;
    if (gpgpu_fill("1. Fuellen 1024 x 64 Bytes", k32, n32, 32, 8, &SA, 0x11223344u, EU_THREADS, &us) != 0) {
        rc = -20;
        goto out_ring;
    }
    igd_clflush((uint64_t)sa, 16 * 4096);
    int bad = 0;
    for (int i = 0; i < 1024 * 64 / 4; i++)
        bad += ((uint32_t *)sa)[i] != 0x11223344u;
    kprintf("igdgpu: Fuellen: %d von 16384 Dwords falsch\n", bad);
    if (bad) {
        rc = -21;
        goto out_ring;
    }

    /* 2. Kopieren: Muster in A, B leer; Rechteck 512 x 32 Bytes von A (128, 8) nach B (256, 16) */
    for (int i = 0; i < 1024 * 64 / 4; i++)
        ((uint32_t *)sa)[i] = pattern((uint32_t)i, 7);
    memset(sb, 0, 16 * 4096);
    igd_clflush((uint64_t)sa, 16 * 4096);
    igd_clflush((uint64_t)sb, 16 * 4096);
    if (gpgpu_rect("2. Kopieren 512 x 32 Bytes", kcopy, ncopy, &SB, 256, 16, &SA, 128, 8, 512, 32, &us) != 0) {
        rc = -22;
        goto out_ring;
    }
    igd_clflush((uint64_t)sb, 16 * 4096);
    bad = 0;
    int first = -1;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 1024; x++) {
            int in = x >= 256 && x < 256 + 512 && y >= 16 && y < 16 + 32;
            uint8_t want = in ? sa[(y - 16 + 8) * 1024 + (x - 256 + 128)] : 0;
            if (sb[y * 1024 + x] != want) {
                if (first < 0)
                    first = y * 1024 + x;
                bad++;
            }
        }
    kprintf("igdgpu: Kopieren: %d von 65536 Bytes falsch", bad);
    if (first >= 0)
        kprintf(" (erstes bei x %d, y %d: %#x)", first % 1024, first / 1024, sb[first]);
    kprintf("\n");
    if (bad) {
        rc = -23;
        goto out_ring;
    }

    /* 3. Mischen: Quelle A (Muster mit Alpha), Ziel B (anderes Muster); ganze Flaeche, gegen die CPU pruefen */
    static uint8_t orig[1024 * 64];
    for (int i = 0; i < 1024 * 64 / 4; i++)
        ((uint32_t *)sb)[i] = pattern((uint32_t)i, 99);
    memcpy(orig, sb, sizeof(orig));
    igd_clflush((uint64_t)sb, 16 * 4096);
    if (gpgpu_rect("3. Mischen 1024 x 64 Bytes", kblend, nblend, &SB, 0, 0, &SA, 0, 0, 1024, 64, &us) != 0) {
        rc = -24;
        goto out_ring;
    }
    igd_clflush((uint64_t)sb, 16 * 4096);
    bad = 0;
    first = -1;
    for (int i = 0; i < 1024 * 64; i++) {
        uint8_t a = sa[(i & ~3) + 3], want = blend_ref(sa[i], orig[i], a);
        if (sb[i] != want) {
            if (first < 0)
                first = i;
            bad++;
        }
    }
    kprintf("igdgpu: Mischen: %d von 65536 Bytes falsch", bad);
    if (first >= 0)
        kprintf(" (erstes Byte %d: GPU %#x, CPU %#x; Quelle %#x, Ziel %#x, Alpha %#x)", first, sb[first],
                blend_ref(sa[first], orig[first], sa[(first & ~3) + 3]), sa[first], orig[first], sa[(first & ~3) + 3]);
    kprintf("\n");
    if (bad) {
        rc = -25;
        goto out_ring;
    }

    /* 4. Tempo bei Bildschirmgroesse: fuellen, kopieren, mischen */
    uint64_t bytes = (uint64_t)big_w * big_h, tf, tc, tb;
    if (gpgpu_fill("4a. Fuellen (Bildschirm)", k32, n32, 32, 8, &BIG, 0x80402010u, EU_THREADS, &tf) ||
        gpgpu_rect("4b. Kopieren (Bildschirm)", kcopy, ncopy, &BIG2, 0, 0, &BIG, 0, 0, big_w, big_h, &tc) ||
        gpgpu_rect("4c. Mischen (Bildschirm)", kblend, nblend, &BIG2, 0, 0, &BIG, 0, 0, big_w, big_h, &tb)) {
        rc = -26;
        goto out_ring;
    }
    kprintf("igdgpu: %u x %u Bytes (%lu KB): fuellen %lu us (%lu MB/s), kopieren %lu us (%lu MB/s), mischen %lu us (%lu MB/s)\n",
            big_w, big_h, (unsigned long)(bytes / 1024), (unsigned long)tf, (unsigned long)mbps(bytes, tf),
            (unsigned long)tc, (unsigned long)mbps(bytes, tc), (unsigned long)tb, (unsigned long)mbps(bytes, tb));

    /* 5. Sichtbar (nur an der Konsole): halbtransparentes Farbfeld (Verlauf, Alpha 176) ueber die Konsole, 5 s */
    if (!console_gfx_active() && igd_flip_ready && igd_scr_w >= 1600 + 64 && igd_scr_h >= 600 + 64) {
        uint32_t pw = 1600, ph = 600, px = ((igd_scr_w - pw) / 2) & ~7u, py = ((igd_scr_h - ph) / 2) & ~7u;
        for (uint32_t y = 0; y < ph; y++)
            for (uint32_t x = 0; x < pw; x++) {
                uint32_t r = 40 + 180 * x / pw, g = 90 + 100 * y / ph, b = 230 - 120 * x / pw;
                big_wr(big, (uint64_t)y * big_w + x * 4, 0xB0000000u | r << 16 | g << 8 | b);
            }
        for (uint32_t i = 0; i < big_pages; i++)
            igd_clflush(big[i], 4096);
        GpuSurf FB = {igd_surf_a, igd_scr_stride, igd_scr_h, igd_scr_stride, 0};
        if (gpgpu_rect("5. Farbfeld ueber die Konsole mischen", kblend, nblend, &FB, px * 4, py, &BIG, 0, 0, pw * 4, ph, &us) != 0)
            rc = -27;
        else
            kprintf("igdgpu: Farbfeld %u x %u Pixel gemischt in %lu us\n", pw, ph, (unsigned long)us);
        thread_sleep_ms(5000);
        console_repaint();
    } else {
        kprintf("igdgpu: sichtbarer Teil nur an der Konsole (ohne Desktop)\n");
    }
    (void)big_rd;
    (void)res;
    if (rc == 0)
        kprintf("igdgpu: Fuellen, Kopieren und Mischen laufen auf den Recheneinheiten\n");

out_ring:
    ring_stop();
    for (int i = 0; i < 62; i++)
        igd_wr(GFX_MOCS(i), mocs[i]);
out_fw:
    igd_forcewake_put();
out_mem:
    for (uint32_t i = 0; i < pages; i++)
        igd_ggtt[base + i] = saved[i];
    igd_ggtt_flush();
    if (mem)
        pmm_free_frames(mem, P_BIG);
    for (uint32_t i = 0; i < got; i++)
        pmm_free_frame(big[i]);
    kfree(saved);
    kfree(big);
    kprintf("igdtest: %s\n", rc == 0 ? "GPGPU-Kernel laufen" : "GPGPU-Kernel mit Fehlern, bitte Log schicken");
    return rc;
}

int igd_render_test(void)
{
    rcs_acquire(1);
    igd_rcs_comp_wait(0);
    comp_ready = 0;
    int rc = render_test();
    rcs_release();
    return rc;
}

int igd_gpgpu_test(void)
{
    rcs_acquire(1);
    igd_rcs_comp_wait(0);
    comp_ready = 0;
    int rc = gpgpu_test();
    rcs_release();
    return rc;
}

/* ---------- Zusammensetzen fuer den Desktop (igd_comp.c) ----------
 * Kopieren und Mischen mit denselben Kernels wie igdtest gpgpu (in vier Blockgroessen, siehe igd_comp.c). Wie dort
 * sind die Flaechen ganze Flaechen ab ihrem Anfang, die Ecke des Rechtecks steht in den Konstanten.
 * Zustandsbereich: je Auftrag 256 Bytes: Interface Descriptor, Konstanten (Ecke des Rechtecks in Ziel und Quelle),
 * Binding Table (Ziel, Quelle), zwei Surface States.
 * Zwischen zwei Auftraegen wartet ein PIPE_CONTROL, bis alle Threads fertig sind (spaetere Auftraege lesen, was
 * fruehere geschrieben haben). */

#define CS_OPS      0     /* erster Auftrag im Zustandsbereich */
#define HWS_TS0     0x300 /* Zeitstempel vor und nach dem Auftrag (12 MHz) */
#define HWS_TS1     0x304

static uint32_t comp_pending; /* laufende Nummer des noch offenen Auftrags, 0 = keiner */
static int      comp_ts_new;  /* Zeitstempel des letzten Auftrags noch nicht gemeldet */

static uint64_t comp_gpu_us(void)
{
    igd_clflush((uint64_t)&hws[HWS_TS0 / 4], 8);
    return (uint64_t)(uint32_t)(hws[HWS_TS1 / 4] - hws[HWS_TS0 / 4]) / 12;
}

/* Offenen Auftrag abwarten; haengt die Engine: zuruecksetzen. 0 = nichts mehr offen */
static int comp_finish(uint64_t *us)
{
    if (us)
        *us = 0;
    if (comp_pending && !wait_seqno(comp_pending, 500)) {
        kprintf("igdcomp: Render-Engine wird mit einem Auftrag nicht fertig\n");
        dump("Zustand");
        igd_forcewake_get();
        ring_stop();
        engine_reset();
        igd_forcewake_put();
        comp_ready = 0;
        comp_pending = 0;
        comp_ts_new = 0;
        return -2;
    }
    comp_pending = 0;
    if (comp_ts_new && us)
        *us = comp_gpu_us();
    if (us)
        comp_ts_new = 0;
    return 0;
}

int igd_rcs_comp_wait(uint64_t *us)
{
    if (!comp_pending && !(us && comp_ts_new)) {
        if (us)
            *us = 0;
        return 0;
    }
    return comp_finish(us);
}
#define CK_PAGES    4     /* Kernel: je Blockform (IGD_BLK_*) kopieren und mischen */
#define L3_MOCS(r)  (0xB020 + 4u * (uint32_t)(r)) /* L3-Cache-Steuerung (LNCFCMOCS), Eintraege 2r und 2r + 1 */
#define L3_UC       0x10u /* L3 uncached */

static uint32_t ck_off[10]; /* Lage der Kernel in C_CKERN: Blockform * 2 + mischen, 8 senkrecht, 9 waagerecht skalieren */

static int comp_start(void)
{
    if (core_setup() != 0)
        return 0;
    if (!igd_forcewake_get()) {
        kprintf("igdcomp: Forcewake nicht bestaetigt\n");
        return 0;
    }
    int ok = ring_start();
    /* Cache-Steuerung der Render-Engine: alles uncached (wie bei den Tests), nur Eintrag 2 (und 4) write-back im LLC
     * (wie i915: WB, LLC/eLLC, Alter 3) - fuer Flaechen, die der Monitor nicht direkt liest. Der L3-Cache der GPU bleibt
     * fuer diese Eintraege aus: dort koennten sonst alte Fensterinhalte liegen, nachdem die CPU sie geaendert hat. */
    for (int i = 0; i < 62; i++)
        igd_wr(GFX_MOCS(i), i == 2 || i == 4 ? 0x3Bu : 0x09u);
    static int l3_logged;
    for (int r = 0; r < 3; r++) { /* LNCFCMOCS: je Register zwei Eintraege, unten der gerade (0, 2, 4) */
        uint32_t v = igd_rd(L3_MOCS(r));
        if (!l3_logged)
            kprintf("igdcomp: L3-Cache-Steuerung[%d] vorher %#x\n", r, v);
        igd_wr(L3_MOCS(r), (v & 0xFFFF0000u) | L3_UC);
    }
    l3_logged = 1;
    if (!ok)
        dump("Ring startet NICHT");
    igd_forcewake_put();
    if (!ok)
        return 0;
    static const int shape[4][2] = {{32, 8}, {4, 8}, {32, 1}, {4, 1}}; /* IGD_BLK_8X8, 1X8, 8X1, 1X1 */
    uint8_t *k = core_ptr(C_CKERN);
    uint32_t at = 0;
    for (int i = 0; i < 10; i++) {
        static uint32_t tmp[512][4];
        int bw = shape[(i / 2) & 3][0], bh = shape[(i / 2) & 3][1];
        int nk = i == 8 ? asm_vscale(tmp) : i == 9 ? asm_hscale(tmp) : (i & 1) ? asm_blend_blk(tmp, bw, bh)
                                                                              : asm_copy_blk(tmp, bw, bh);
        if (at + (uint32_t)nk * 16 > CK_PAGES * 4096) {
            kprintf("igdcomp: Kernel passen nicht in %d Seiten\n", CK_PAGES);
            return 0;
        }
        memcpy(k + at, tmp, (uint64_t)nk * 16);
        ck_off[i] = at;
        at = (at + (uint32_t)nk * 16 + 63) & ~63u;
    }
    igd_clflush((uint64_t)k, CK_PAGES * 4096);
    comp_ready = 1;
    kprintf("igdcomp: Render-Engine eingerichtet (10 Kernel, %u Bytes)\n", at);
    return 1;
}

int igd_rcs_comp(const IgdCompOp *ops, int n, uint64_t *us, int async)
{
    if (n <= 0)
        return 0;
    if (n > IGD_COMP_MAX_OPS)
        return -2;
    if (!rcs_acquire(0))
        return -1;
    int rc = 0;
    if (comp_finish(0) != 0) { /* Batch und Zustaende werden gleich neu beschrieben */
        rc = -2;
        goto out;
    }
    if (!comp_ready && !comp_start()) {
        rc = -2;
        goto out;
    }
    uint8_t *st = core_ptr(C_CSTATE);
    uint32_t *b = core_ptr(C_BATCH), k = 0;
    memset(st, 0, CS_OPS + (uint32_t)n * 256);
    b[k++] = PIPELINE_SELECT_GPGPU;
    b[k++] = STATE_BASE_ADDRESS; /* Surface/Dynamic = Zustandsbereich, Instruction = Kernel */
    b[k++] = 0 | 1;
    b[k++] = 0;
    b[k++] = 0 | 1;
    b[k++] = core_gtt(C_CSTATE) | 1;
    b[k++] = 0;
    b[k++] = core_gtt(C_CSTATE) | 1;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = core_gtt(C_CKERN) | 1;
    b[k++] = 0;
    b[k++] = 0xFFFFF000u | 1;
    b[k++] = (8u << 12) | 1;     /* Dynamic State: 8 Seiten */
    b[k++] = 0xFFFFF000u | 1;
    b[k++] = ((uint32_t)CK_PAGES << 12) | 1;
    b[k++] = 0 | 1;
    b[k++] = 0;
    b[k++] = 0xFFFFF000u;
    b[k++] = MEDIA_VFE_STATE;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = (EU_THREADS - 1) << 16 | (1u << 8);
    b[k++] = 0;
    b[k++] = (0u << 16) | 1;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    for (int i = 0; i < n; i++) {
        const IgdCompOp *o = &ops[i];
        uint32_t off = CS_OPS + (uint32_t)i * 256;
        uint32_t *idd = (uint32_t *)(st + off), *cb = (uint32_t *)(st + off + 64), *bt = (uint32_t *)(st + off + 96);
        idd[0] = o->op == IGD_OP_VSCALE ? ck_off[8] : o->op == IGD_OP_HSCALE ? ck_off[9]
                                                       : ck_off[(o->shape & 3) * 2 + (o->op == IGD_OP_BLEND)];
        idd[2] = 1u << 18;       /* Single Program Flow */
        idd[4] = off + 96;       /* Binding Table */
        idd[5] = 1u << 16;       /* Konstanten: 1 Register */
        idd[6] = 1;
        cb[0] = o->dx;           /* Ziel x (Bytes), y */
        cb[1] = o->dy;
        cb[2] = o->sx;           /* Quelle x, y */
        cb[3] = o->sy;
        cb[4] = o->galpha;       /* Deckung beim Mischen (256 = unveraendert) */
        cb[5] = o->step;         /* Skalieren: 8.8 */
        bt[0] = off + 128;
        bt[1] = off + 192;
        surf_state((uint32_t *)(st + off + 128), &o->dst);
        surf_state((uint32_t *)(st + off + 192), &o->src);
        b[k++] = MEDIA_CURBE_LOAD;
        b[k++] = 0;
        b[k++] = 32;
        b[k++] = off + 64;
        b[k++] = MEDIA_IDD_LOAD;
        b[k++] = 0;
        b[k++] = 32;
        b[k++] = off;
        b[k++] = GPGPU_WALKER;
        b[k++] = 0;
        b[k++] = 0;
        b[k++] = 0;
        b[k++] = 1u << 30;       /* SIMD16 */
        b[k++] = 0;
        b[k++] = 0;
        b[k++] = o->gx;
        b[k++] = 0;
        b[k++] = 0;
        b[k++] = o->gy;
        b[k++] = 0;
        b[k++] = 1;
        b[k++] = 0xFFFF;
        b[k++] = 0xFFFFFFFFu;
        b[k++] = MEDIA_STATE_FLUSH;
        b[k++] = 0;
        b[k++] = PIPE_CONTROL;   /* alle Threads fertig, Daten-Cache geleert: der naechste Auftrag sieht das Ergebnis */
        b[k++] = i + 1 < n ? PC_CS_STALL | PC_DC_FLUSH
                           : PC_CS_STALL | PC_DC_FLUSH | PC_RT_FLUSH | PC_WRITE_QWORD | PC_GLOBAL_GTT;
        b[k++] = i + 1 < n ? 0 : core_gtt(C_RES);
        b[k++] = 0;
        b[k++] = 0;
        b[k++] = 0;
    }
    b[k++] = MI_BATCH_BUFFER_END;
    b[k++] = MI_NOOP;
    igd_clflush((uint64_t)st, CS_OPS + (uint64_t)n * 256);
    igd_clflush((uint64_t)b, k * 4);
    if (!igd_forcewake_get()) {
        rc = -2;
        goto out;
    }
    begin(4 + 8 + 8);
    emit(MI_STORE_REG_MEM); /* Zeitstempel vorher */
    emit(R_TIMESTAMP);
    emit(hws_gtt + HWS_TS0);
    emit(0);
    emit(MI_BATCH_BUFFER_START);
    emit(core_gtt(C_BATCH));
    emit(0);
    emit(MI_NOOP);
    emit(MI_STORE_REG_MEM); /* und nachher (der Batch endet mit einem PIPE_CONTROL, der auf alle Threads wartet) */
    emit(R_TIMESTAMP);
    emit(hws_gtt + HWS_TS1);
    emit(0);
    comp_pending = submit();
    comp_ts_new = 1;
    igd_forcewake_put();
    if (!async && comp_finish(us) != 0)
        rc = -2;
out:
    rcs_release();
    return rc;
}

/* ---------- Stufe 7: 3D-Pipeline (igdtest 3d) ----------
 * Aufgebaut wie IGTs rendercopy fuer Gen9. Zwei Arten zu zeichnen:
 *  - Rechteck (RECTLIST) ohne Vertex-Shader: die Eckpunkte stehen schon in Bildschirmkoordinaten im Vertex-Buffer
 *    (x, y, u, v), der Pixel-Shader schreibt eine feste Farbe.
 *  - Dreiecke (TRILIST) mit Vertex-Shader (SIMD8): Eckpunkte x, y, r, g, b, a; der Vertex-Shader rechnet die Position
 *    mit einer 2x3-Matrix in Bildschirmkoordinaten um und reicht die Farbe weiter, der Pixel-Shader interpoliert sie
 *    (pln aus den Ebenen-Koeffizienten des Rasterizers).
 * Clipper durchlassen, Rasterizer ohne Culling und ohne Viewport-Umrechnung. Die Statistikzaehler der Pipeline zeigen,
 * wie weit die GPU gekommen ist. Gleitkommawerte stehen als Bitmuster da (der Kernel rechnet ohne FPU). */

#define G3D(sub, op, subop)  ((3u << 29) | ((uint32_t)(sub) << 27) | ((uint32_t)(op) << 24) | ((uint32_t)(subop) << 16))
#define S3D(subop)           G3D(3, 0, subop)     /* 3DSTATE_* */
#define PIPELINE_SELECT_3D   (0x69040000u | (3u << 8) | 0)
#define STATE_SIP            (0x61020000u | 1)
#define VF_STATISTICS_ON     (0x680B0000u | 1)
#define DRAWING_RECTANGLE    (G3D(3, 1, 0x00) | 2)
#define PRIMITIVE_3D         (G3D(3, 3, 0x00) | 5)
#define PRIM_TRILIST         0x04
#define PRIM_RECTLIST        0x0F
#define SURF_B8G8R8A8_UNORM  0x0C0
#define SURF_RGBA32_FLOAT    0x000
#define SURF_RG32_FLOAT      0x085
#define SFID_URB             0x6
#define SFID_RC              0x5               /* Data Port Render Cache */
#define EU_PLN               0x5A
/* Render-Target-Write SIMD16 ohne Kopf: 8 Register (R, G, B, A je 2), letztes Render-Target, Thread-Ende */
#define RT_WRITE_SIMD16      (0x80000000u | (8u << 25) | (0xCu << 14) | (1u << 12))
/* URB-Write SIMD8 (Typ 7) mit Kopf (Handles), 12 Datenregister = 3 vec4 (Kopf, Position, Farbe), Thread-Ende */
#define URB_WRITE_VUE3       (0x80000000u | (13u << 25) | (1u << 19) | 7u)
#define VFC_SRC 1u
#define VFC_0   2u
#define VFC_1F  3u
#define F_1_0   0x3F800000u                    /* 1.0f */
#define F_0_2   0x3E4CCCCDu
#define F_0_4   0x3ECCCCCDu
#define F_0_6   0x3F19999Au
#define F_0_8   0x3F4CCCCDu
#define K3_PSCOL  0x000                        /* Kernel im Kernelbereich (C_KERN) */
#define K3_PSINT  0x400
#define K3_VS     0x800

/* Festkomma Q16.16 (mit Vorzeichen) als float-Bitmuster */
static uint32_t f32q(int32_t q)
{
    if (!q)
        return 0;
    uint32_t s = q < 0 ? 0x80000000u : 0, m = q < 0 ? (uint32_t)-q : (uint32_t)q;
    int e = 31 - __builtin_clz(m);
    uint32_t mant = e >= 23 ? m >> (e - 23) : m << (23 - e);
    return s | (uint32_t)(127 + e - 16) << 23 | (mant & 0x7FFFFF);
}

static uint32_t f32(uint32_t v) { return f32q((int32_t)(v << 16)); } /* Ganzzahl < 32768 */

/* sin(2 pi i / 64) in Q16 */
static const int32_t sin64[64] = {
    0, 6424, 12785, 19024, 25080, 30893, 36410, 41576, 46341, 50660, 54491, 57798, 60547, 62714, 64277, 65220,
    65536, 65220, 64277, 62714, 60547, 57798, 54491, 50660, 46341, 41576, 36410, 30893, 25080, 19024, 12785, 6424,
    0, -6424, -12785, -19024, -25080, -30893, -36410, -41576, -46341, -50660, -54491, -57798, -60547, -62714, -64277,
    -65220, -65536, -65220, -64277, -62714, -60547, -57798, -54491, -50660, -46341, -41576, -36410, -30893, -25080,
    -19024, -12785, -6424,
};

/* Pipeline-Statistik (je 64 Bit; zaehlt mit VF_STATISTICS, VS-, Clipper- und WM-Statistik) */
static const struct {
    uint32_t    reg;
    const char *name;
} stat3d[] = {
    {0x2310, "Eckpunkte gelesen"}, {0x2318, "Primitive gelesen"}, {0x2320, "Vertex-Shader"},
    {0x2338, "Clipper ein"},       {0x2340, "Clipper aus"},       {0x2348, "Pixel-Shader"},
};
#define NSTAT3D ((int)(sizeof(stat3d) / sizeof(stat3d[0])))

static uint64_t rd64(uint32_t r)
{
    return (uint64_t)igd_rd(r) | (uint64_t)igd_rd(r + 4) << 32;
}

/* Pixel-Shader: feste Farbe (float-Bitmuster je Kanal) in r112-r119 (SIMD16: R, G, B, A je 2 Register), dann
 * Render-Target-Write mit Thread-Ende */
static int asm_ps_color(uint32_t (*k)[4], uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    const uint32_t c[4] = {r, g, b, a};
    int n = 0;
    for (int i = 0; i < 4; i++)
        eu_inst(k[n++], EU_MOV, 4, 0, eu_d(EU_F, 112 + 2 * i, 0), eu_imm(EU_F, c[i]), eu_none());
    eu_send(k[n++], 4, SFID_RC, eu_null(), 112, RT_WRITE_SIMD16);
    return n;
}

/* Pixel-Shader: Farbe (Attribut 0) interpolieren. Die Ebenen-Koeffizienten stehen ab r6 (je Komponente 4 floats:
 * r6.0 R, r6.4 G, r7.0 B, r7.4 A), die Baryzentrik in r2-r5; pln wie im Kernel von IGT */
static int asm_ps_interp(uint32_t (*k)[4])
{
    static const int reg[4] = {6, 6, 7, 7}, sub[4] = {0, 16, 0, 16};
    int n = 0;
    for (int c = 0; c < 4; c++)
        eu_inst(k[n++], EU_PLN, 4, 0, eu_d(EU_F, 112 + 2 * c, 0), eu_r(EU_F, reg[c], sub[c], SCALAR),
                eu_r(EU_F, 2, 0, 4, 3, 1));
    eu_send(k[n++], 4, SFID_RC, eu_null(), 112, RT_WRITE_SIMD16);
    return n;
}

/* Vertex-Shader (SIMD8, 8 Eckpunkte je Thread): Eingaben r2-r5 Position x, y, z, w und r6-r9 Farbe (je Register eine
 * Komponente fuer 8 Eckpunkte). x' = m0 x + m1 y + m2, y' = m3 x + m4 y + m5 (Bildschirmkoordinaten), z 0, w 1.
 * Ausgabe per URB-Write an die Handles aus r1: Kopf (4 x 0), Position, Farbe; Thread-Ende */
static int asm_vs_affine(uint32_t (*k)[4], const uint32_t m[6])
{
    int n = 0;
    eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_UD, 112, 0), eu_r(EU_UD, 1, 0, 4, 3, 1), eu_none());
    k[n - 1][0] |= 1u << 9; /* NoMask: alle 8 Handles */
    for (int i = 0; i < 4; i++)
        eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_UD, 113 + i, 0), eu_imm(EU_UD, 0), eu_none());
    EuOp x = eu_r(EU_F, 2, 0, 4, 3, 1), y = eu_r(EU_F, 3, 0, 4, 3, 1), t = eu_r(EU_F, 20, 0, 4, 3, 1);
    for (int row = 0; row < 2; row++) {
        int out = 117 + row;
        EuOp o = eu_r(EU_F, out, 0, 4, 3, 1);
        eu_inst(k[n++], EU_MUL, 3, 0, eu_d(EU_F, out, 0), x, eu_imm(EU_F, m[row * 3 + 0]));
        eu_inst(k[n++], EU_MUL, 3, 0, eu_d(EU_F, 20, 0), y, eu_imm(EU_F, m[row * 3 + 1]));
        eu_inst(k[n++], EU_ADD, 3, 0, eu_d(EU_F, out, 0), o, t);
        eu_inst(k[n++], EU_ADD, 3, 0, eu_d(EU_F, out, 0), o, eu_imm(EU_F, m[row * 3 + 2]));
    }
    eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_F, 119, 0), eu_imm(EU_F, 0), eu_none());
    eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_F, 120, 0), eu_imm(EU_F, F_1_0), eu_none());
    for (int i = 0; i < 4; i++)
        eu_inst(k[n++], EU_MOV, 3, 0, eu_d(EU_UD, 121 + i, 0), eu_r(EU_UD, 6 + i, 0, 4, 3, 1), eu_none());
    eu_send(k[n++], 3, SFID_URB, eu_null(), 112, URB_WRITE_VUE3);
    return n;
}

/* Lage im Zustandsbereich (Surface- und Dynamic-State-Basis = C_STATE) */
#define S3_BLEND   0x000   /* BLEND_STATE: alles 0 = nicht mischen, alle Kanaele schreiben */
#define S3_CC      0x0C0   /* COLOR_CALC_STATE */
#define S3_CCVP    0x100   /* CC_VIEWPORT: Tiefe 0-1 */
#define S3_SFVP    0x140   /* SF_CLIP_VIEWPORT */
#define S3_SCISSOR 0x180
#define S3_BT      0x1C0   /* Binding Table des Pixel-Shaders: 0 = Render-Target */
#define S3_RT      0x200   /* Surface State des Render-Targets */
#define S3_VB      0x300   /* Vertex-Buffer */

typedef struct {
    uint32_t        rt_gtt, w, h, pitch; /* Render-Target (Pixel, Zeilenlaenge in Bytes) */
    int             vs;                  /* 1: Vertex-Shader, Eckpunkte x, y, r, g, b, a; 0: aus, x, y, u, v */
    uint32_t        prim, nvert;
    uint32_t        ps;                  /* Pixel-Shader im Kernelbereich */
    const uint32_t *verts;               /* float-Bitmuster, 6 bzw. 4 je Eckpunkt */
} Draw3d;

/* Zustaende und Batch fuer einen Aufruf */
static uint32_t batch_3d(const Draw3d *d)
{
    uint8_t *st = core_ptr(C_STATE);
    memset(st, 0, 4096);
    uint32_t *ccvp = (uint32_t *)(st + S3_CCVP), *sfvp = (uint32_t *)(st + S3_SFVP);
    ccvp[1] = F_1_0;
    sfvp[9] = F_1_0; /* Guardband (Clipper ist aus) */
    sfvp[11] = F_1_0;
    *(uint32_t *)(st + S3_BT) = S3_RT;
    uint32_t *rt = (uint32_t *)(st + S3_RT);
    rt[0] = (1u << 29) | (SURF_B8G8R8A8_UNORM << 18) | (1u << 16) | (1u << 14); /* 2D, linear */
    rt[2] = (d->w - 1) | ((d->h - 1) << 16);
    rt[3] = d->pitch - 1;
    rt[7] = (4u << 25) | (5u << 22) | (6u << 19) | (7u << 16);
    rt[8] = d->rt_gtt;
    uint32_t vsize = d->vs ? 24 : 16;
    memcpy(st + S3_VB, d->verts, (uint64_t)d->nvert * vsize);

    uint32_t *b = core_ptr(C_BATCH), k = 0, base = core_gtt(C_STATE);
    b[k++] = PIPE_CONTROL; /* vor dem Wechsel der Pipeline: alles fertig, Caches leer */
    b[k++] = PC_CS_STALL | PC_RT_FLUSH | PC_DC_FLUSH;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = PIPELINE_SELECT_3D;
    b[k++] = STATE_SIP;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = VF_STATISTICS_ON;
    for (uint32_t s = 0x12; s <= 0x16; s++) { /* PUSH_CONSTANT_ALLOC_VS, _HS, _DS, _GS, _PS (Opcode 1): keine */
        b[k++] = G3D(3, 1, s);
        b[k++] = 0;
    }
    b[k++] = STATE_BASE_ADDRESS;
    b[k++] = 0 | 1;
    b[k++] = 0;
    b[k++] = 0 | 1;
    b[k++] = base | 1;               /* Surface State */
    b[k++] = 0;
    b[k++] = base | 1;               /* Dynamic State */
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = core_gtt(C_KERN) | 1;   /* Instruction: die Shader */
    b[k++] = 0;
    b[k++] = 0xFFFFF000u | 1;
    b[k++] = (1u << 12) | 1;
    b[k++] = 0xFFFFF000u | 1;
    b[k++] = (2u << 12) | 1;
    b[k++] = 0 | 1;
    b[k++] = 0;
    b[k++] = 0xFFFFF000u;
    b[k++] = S3D(0x23);              /* VIEWPORT_STATE_POINTERS_CC */
    b[k++] = S3_CCVP;
    b[k++] = S3D(0x21);              /* VIEWPORT_STATE_POINTERS_SF_CLIP */
    b[k++] = S3_SFVP;
    b[k++] = S3D(0x30);              /* URB_VS: 64 Eintraege zu 2 x 64 Bytes ab 2 x 8 KiB */
    b[k++] = 64 | (1u << 16) | (2u << 25);
    for (uint32_t s = 0x31; s <= 0x33; s++) { /* URB_HS, _DS, _GS: keine */
        b[k++] = S3D(s);
        b[k++] = 2u << 25;
    }
    b[k++] = S3D(0x24);              /* BLEND_STATE_POINTERS */
    b[k++] = S3_BLEND | 1;
    b[k++] = S3D(0x0E);              /* CC_STATE_POINTERS */
    b[k++] = S3_CC | 1;
    b[k++] = S3D(0x0D);              /* MULTISAMPLE: 1 Abtastpunkt */
    b[k++] = 0;
    b[k++] = S3D(0x18);              /* SAMPLE_MASK */
    b[k++] = 1;
    /* nicht benutzte Stufen aus: HiZ-Operation, Konstanten, Binding Tables, Sampler, HS, TE, DS, GS */
    b[k++] = S3D(0x52) | 3;          /* WM_HZ_OP */
    for (int i = 0; i < 4; i++)
        b[k++] = 0;
    const uint32_t cs[4] = {0x15, 0x19, 0x1A, 0x16}; /* CONSTANT_VS, _HS, _DS, _GS */
    for (int c = 0; c < 4; c++) {
        b[k++] = S3D(cs[c]) | 9;
        for (int i = 0; i < 10; i++)
            b[k++] = 0;
    }
    for (uint32_t s = 0x26; s <= 0x29; s++) { /* BINDING_TABLE_POINTERS_VS, _HS, _DS, _GS */
        b[k++] = S3D(s);
        b[k++] = 0;
    }
    for (uint32_t s = 0x2B; s <= 0x2E; s++) { /* SAMPLER_STATE_POINTERS_VS, _HS, _DS, _GS */
        b[k++] = S3D(s);
        b[k++] = 0;
    }
    b[k++] = S3D(0x10) | 7;          /* VS */
    if (d->vs) { /* Kernel, Eingaben ab r2 (1 x 256 Bit: Position und Farbe), 64 Threads, Statistik, SIMD8, an */
        b[k++] = K3_VS;
        b[k++] = 0;
        b[k++] = 0;
        b[k++] = 0;
        b[k++] = 0;
        b[k++] = (2u << 20) | (1u << 11) | (0u << 4);
        b[k++] = (63u << 23) | (1u << 10) | (1u << 2) | 1u;
        b[k++] = (1u << 21) | (1u << 16); /* Ausgabe fuer SBE: ab 256 Bit (nach Kopf und Position), 1 x 256 Bit */
    } else {
        for (int i = 0; i < 8; i++)
            b[k++] = 0;
    }
    const uint32_t stage[4][2] = {{0x1B, 9}, {0x1C, 4}, {0x1D, 11}, {0x11, 10}}; /* HS, TE, DS, GS */
    for (int s = 0; s < 4; s++) {
        b[k++] = S3D(stage[s][0]) | (stage[s][1] - 2);
        for (uint32_t i = 1; i < stage[s][1]; i++)
            b[k++] = 0;
    }
    b[k++] = S3D(0x1E) | 3;          /* STREAMOUT: aus */
    for (int i = 0; i < 4; i++)
        b[k++] = 0;
    b[k++] = S3D(0x12) | 2;          /* CLIP: Statistik, sonst durchlassen */
    b[k++] = 1u << 10;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = S3D(0x1F) | 4;          /* SBE: 1 Attribut aus dem VUE ab 256 Bit */
    b[k++] = (1u << 22) | (1u << 29) | (1u << 28) | (1u << 11) | (1u << 5);
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 3;                      /* Attribut 0: xyzw aktiv */
    b[k++] = 0;
    b[k++] = S3D(0x51) | 9;          /* SBE_SWIZ */
    for (int i = 0; i < 10; i++)
        b[k++] = 0;
    b[k++] = S3D(0x50) | 3;          /* RASTER: kein Culling, gegen den Uhrzeigersinn vorn */
    b[k++] = (1u << 16) | (1u << 21);
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = S3D(0x13) | 2;          /* SF: keine Viewport-Umrechnung (Bildschirmkoordinaten) */
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = S3D(0x14);              /* WM: Statistik, Baryzentrik perspektivisch je Pixel */
    b[k++] = (1u << 31) | (1u << 11);
    b[k++] = S3D(0x4C);              /* WM_CHROMAKEY */
    b[k++] = 0;
    b[k++] = S3D(0x17) | 9;          /* CONSTANT_PS */
    for (int i = 0; i < 10; i++)
        b[k++] = 0;
    b[k++] = S3D(0x20) | 10;         /* PS: 1 Binding-Table-Eintrag, 64 Threads, SIMD16, Koeffizienten ab r6 */
    b[k++] = d->ps;
    b[k++] = 0;
    b[k++] = 1u << 18;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = (63u << 23) | (1u << 1);
    b[k++] = 6u << 16;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = S3D(0x4F);              /* PS_EXTRA: Pixel-Shader gueltig, mit Attributen */
    b[k++] = (1u << 31) | (1u << 8);
    b[k++] = S3D(0x4D);              /* PS_BLEND: es gibt ein beschreibbares Render-Target */
    b[k++] = 1u << 30;
    b[k++] = S3D(0x2A);              /* BINDING_TABLE_POINTERS_PS */
    b[k++] = S3_BT;
    b[k++] = S3D(0x2F);              /* SAMPLER_STATE_POINTERS_PS */
    b[k++] = 0;
    b[k++] = S3D(0x0F);              /* SCISSOR_STATE_POINTERS */
    b[k++] = S3_SCISSOR;
    b[k++] = S3D(0x4E) | 2;          /* WM_DEPTH_STENCIL: kein Tiefen-/Stenciltest */
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = S3D(0x05) | 6;          /* DEPTH_BUFFER: keiner */
    b[k++] = (7u << 29) | (1u << 18); /* SURFTYPE_NULL, Format D32_FLOAT (wie IGT) */
    for (int i = 0; i < 6; i++)
        b[k++] = 0;
    b[k++] = S3D(0x07) | 3;          /* HIER_DEPTH_BUFFER */
    for (int i = 0; i < 4; i++)
        b[k++] = 0;
    b[k++] = S3D(0x06) | 3;          /* STENCIL_BUFFER */
    for (int i = 0; i < 4; i++)
        b[k++] = 0;
    b[k++] = S3D(0x04) | 1;          /* CLEAR_PARAMS */
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = DRAWING_RECTANGLE;
    b[k++] = 0;
    b[k++] = ((d->h - 1) << 16) | (d->w - 1);
    b[k++] = 0;
    b[k++] = S3D(0x08) | 3;          /* VERTEX_BUFFERS: Puffer 0 */
    b[k++] = (0u << 26) | (1u << 14) | vsize;
    b[k++] = base + S3_VB;
    b[k++] = 0;
    b[k++] = d->nvert * vsize;
    uint32_t nelem;
    if (d->vs) { /* Eingaben des Vertex-Shaders: Position x, y, 0, 1 und Farbe r, g, b, a */
        nelem = 2;
        b[k++] = S3D(0x09) | (2 * nelem - 1);
        b[k++] = (1u << 25) | (SURF_RG32_FLOAT << 16) | 0;
        b[k++] = (VFC_SRC << 28) | (VFC_SRC << 24) | (VFC_0 << 20) | (VFC_1F << 16);
        b[k++] = (1u << 25) | (SURF_RGBA32_FLOAT << 16) | 8;
        b[k++] = (VFC_SRC << 28) | (VFC_SRC << 24) | (VFC_SRC << 20) | (VFC_SRC << 16);
    } else { /* ohne Vertex-Shader ist das schon das VUE: 4 x 0 (Kopf), Position x, y, 0, 1, Attribut u, v, 0, 1 */
        nelem = 3;
        b[k++] = S3D(0x09) | (2 * nelem - 1);
        b[k++] = (1u << 25) | (SURF_RGBA32_FLOAT << 16) | 0;
        b[k++] = (VFC_0 << 28) | (VFC_0 << 24) | (VFC_0 << 20) | (VFC_0 << 16);
        b[k++] = (1u << 25) | (SURF_RG32_FLOAT << 16) | 0;
        b[k++] = (VFC_SRC << 28) | (VFC_SRC << 24) | (VFC_0 << 20) | (VFC_1F << 16);
        b[k++] = (1u << 25) | (SURF_RG32_FLOAT << 16) | 8;
        b[k++] = (VFC_SRC << 28) | (VFC_SRC << 24) | (VFC_0 << 20) | (VFC_1F << 16);
    }
    for (uint32_t e = 0; e < nelem; e++) { /* VF_INSTANCING: nicht instanziert */
        b[k++] = S3D(0x49) | 1;
        b[k++] = e;
        b[k++] = 0;
    }
    b[k++] = S3D(0x4A);              /* VF_SGVS */
    b[k++] = 0;
    b[k++] = S3D(0x0C);              /* VF: kein Cut-Index */
    b[k++] = 0;
    b[k++] = S3D(0x4B);              /* VF_TOPOLOGY */
    b[k++] = d->prim;
    b[k++] = PRIMITIVE_3D;           /* nvert Eckpunkte, 1 Instanz */
    b[k++] = 0;
    b[k++] = d->nvert;
    b[k++] = 0;
    b[k++] = 1;
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = PIPE_CONTROL;           /* warten, Render-Cache leeren, Merker schreiben */
    b[k++] = PC_CS_STALL | PC_RT_FLUSH | PC_DC_FLUSH | PC_WRITE_QWORD | PC_GLOBAL_GTT;
    b[k++] = core_gtt(C_RES);
    b[k++] = 0;
    b[k++] = 0x600D3D00u;
    b[k++] = 0;
    b[k++] = MI_BATCH_BUFFER_END;
    b[k++] = MI_NOOP;
    igd_clflush((uint64_t)st, 4096);
    igd_clflush((uint64_t)b, k * 4);
    return k;
}

/* Batch ausfuehren, Statistik vorher/nachher; 0 = fertig. stat: Werte der Statistik zurueck (oder 0) */
static int draw_3d(const char *what, const Draw3d *d, int log, uint64_t *stat)
{
    batch_3d(d);
    uint64_t before[NSTAT3D];
    for (int i = 0; i < NSTAT3D; i++)
        before[i] = rd64(stat3d[i].reg);
    begin(4 + 8);
    emit(MI_BATCH_BUFFER_START);
    emit(core_gtt(C_BATCH));
    emit(0);
    emit(MI_NOOP);
    uint64_t us = 0;
    int rc = log ? run(what, 1000, &us) : (submit(), wait_seqno(seqno, 1000) ? 0 : -1);
    if (rc != 0) { /* ACTHD - Batch-Anfang = wo die Engine stehen blieb (Dword = Abstand / 4) */
        if (!log) {
            kprintf("igd3d: %s: KEINE Rueckmeldung\n", what);
            dump("Zustand");
            ring_stop();
            engine_reset();
        }
        kprintf("igd3d: Batch ab %#x, Zustaende ab %#x, Shader ab %#x\n", core_gtt(C_BATCH), core_gtt(C_STATE),
                core_gtt(C_KERN));
    }
    for (int i = 0; i < NSTAT3D; i++) {
        uint64_t v = rd64(stat3d[i].reg) - before[i];
        if (stat)
            stat[i] = v;
    }
    if (log && stat) {
        kprintf("igd3d: Pipeline-Statistik:");
        for (int i = 0; i < NSTAT3D; i++)
            kprintf(" %s %lu%s", stat3d[i].name, (unsigned long)stat[i], i + 1 < NSTAT3D ? "," : "\n");
    }
    return rc;
}

/* Kernel in den Kernelbereich schreiben (und aus dem CPU-Cache) */
static void load_kernel(uint32_t off, const uint32_t (*k)[4], int n)
{
    uint8_t *dst = (uint8_t *)core_ptr(C_KERN) + off;
    memset(dst, 0, 0x400);
    memcpy(dst, k, (uint64_t)n * 16);
    igd_clflush((uint64_t)dst, 0x400);
}

/* Drehung um a (0-63 = 0-360 Grad), Massstab s (Pixel je Einheit), Mittelpunkt (cx, cy): 2x3-Matrix */
static void rot_matrix(uint32_t m[6], int a, int32_t s, int32_t cx, int32_t cy)
{
    int32_t c = sin64[(a + 16) & 63], sn = sin64[a & 63];
    m[0] = f32q((int32_t)(((int64_t)c * s) >> 0));      /* s cos (Q16: c ist Q16, s ganzzahlig) */
    m[1] = f32q((int32_t)(((int64_t)-sn * s) >> 0));
    m[2] = f32q(cx << 16);
    m[3] = f32q((int32_t)(((int64_t)sn * s) >> 0));
    m[4] = f32q((int32_t)(((int64_t)c * s) >> 0));
    m[5] = f32q(cy << 16);
}

/* Dreieck mit roter, gruener, blauer Ecke um den Ursprung (Einheitskreis): Spitze oben */
static void tri_verts(uint32_t v[3][6])
{
    static const uint32_t pos[3][2] = {{0, 0xBF800000u}, {0x3F5DB3D7u, 0x3F000000u}, {0xBF5DB3D7u, 0x3F000000u}};
    for (int i = 0; i < 3; i++) {
        v[i][0] = pos[i][0];
        v[i][1] = pos[i][1];
        for (int c = 0; c < 3; c++)
            v[i][2 + c] = c == i ? F_1_0 : 0;
        v[i][5] = F_1_0;
    }
}

static int test_3d(void)
{
    int pre = igd_preflight("Stufe 7 - 3D-Pipeline: Rechteck, Dreieck mit Vertex-Shader");
    if (pre)
        return pre;
    int rc = core_setup();
    if (rc)
        return rc;
    enum { TW = 256, TH = 256, TPAGES = TW * TH * 4 / 4096 };
    uint32_t base = core_base + 0x40;
    static uint64_t saved[TPAGES];
    if ((rc = igd_ggtt_claim(base, TPAGES, saved)) != 0)
        return rc;
    uint64_t mem = pmm_alloc_frames(TPAGES);
    if (!mem) {
        kprintf("igd3d: kein Speicher\n");
        return -6;
    }
    for (uint32_t i = 0; i < TPAGES; i++)
        igd_ggtt[base + i] = (mem + i * 4096) | IGD_PTE_VALID;
    igd_ggtt_flush();
    uint32_t *px = (uint32_t *)mem, mocs[62];
    for (uint32_t i = 0; i < TW * TH; i++)
        px[i] = 0x11111111u;
    igd_clflush(mem, TPAGES * 4096);

    static uint32_t kbuf[64][4];
    if (!igd_forcewake_get()) {
        kprintf("igd3d: Forcewake nicht bestaetigt\n");
        rc = -9;
        goto out_fw;
    }
    for (int i = 0; i < 62; i++)
        mocs[i] = igd_rd(GFX_MOCS(i));
    int ring_ok = ring_start();
    for (int i = 0; i < 62; i++) /* uncached wie bei den anderen Tests */
        igd_wr(GFX_MOCS(i), 0x09);
    if (!ring_ok) {
        dump("Ring startet NICHT");
        rc = -10;
        goto out_ring;
    }
    load_kernel(K3_PSINT, (const uint32_t (*)[4])kbuf, asm_ps_interp(kbuf));
    uint64_t stat[NSTAT3D];

    /* 1. Rechteck [16, 176) x [8, 40) in Rosa-Rot (R 1.0, G 0.2, B 0.6, A 1.0), ohne Vertex-Shader */
    const uint32_t X0 = 16, Y0 = 8, X1 = 176, Y1 = 40, want = 0xFFFF3399u;
    load_kernel(K3_PSCOL, (const uint32_t (*)[4])kbuf, asm_ps_color(kbuf, F_1_0, F_0_2, F_0_6, F_1_0));
    const uint32_t rv[3][4] = {{f32(X1), f32(Y1), F_1_0, F_1_0}, {f32(X0), f32(Y1), 0, F_1_0}, {f32(X0), f32(Y0), 0, 0}};
    Draw3d r1 = {base << 12, TW, TH, TW * 4, 0, PRIM_RECTLIST, 3, K3_PSCOL, &rv[0][0]};
    if (draw_3d("1. Rechteck ueber die 3D-Pipeline", &r1, 1, stat) != 0) {
        rc = -30;
        goto out_ring;
    }
    igd_clflush(mem, TPAGES * 4096);
    int bad = 0;
    for (uint32_t y = 0; y < TH; y++)
        for (uint32_t x = 0; x < TW; x++)
            bad += px[y * TW + x] != (x >= X0 && x < X1 && y >= Y0 && y < Y1 ? want : 0x11111111u);
    kprintf("igd3d: 1. Rechteck %u x %u: %d Pixel falsch\n", X1 - X0, Y1 - Y0, bad);
    if (bad) {
        rc = -31;
        goto out_ring;
    }

    /* 2. Dreieck mit Vertex-Shader: Ecken rot (oben), gruen (rechts unten), blau (links unten), Massstab 100,
     *    Mittelpunkt (128, 128) -> Ecken (128, 28), (214.6, 178), (41.4, 178), Flaeche etwa 12990 Pixel */
    for (uint32_t i = 0; i < TW * TH; i++)
        px[i] = 0x11111111u;
    igd_clflush(mem, TPAGES * 4096);
    uint32_t m[6], tv[3][6];
    rot_matrix(m, 0, 100, 128, 128);
    tri_verts(tv);
    load_kernel(K3_VS, (const uint32_t (*)[4])kbuf, asm_vs_affine(kbuf, m));
    Draw3d t1 = {base << 12, TW, TH, TW * 4, 1, PRIM_TRILIST, 3, K3_PSINT, &tv[0][0]};
    if (draw_3d("2. Dreieck mit Vertex-Shader", &t1, 1, stat) != 0) {
        rc = -33;
        goto out_ring;
    }
    igd_clflush(mem, TPAGES * 4096);
    uint32_t written = 0;
    for (uint32_t i = 0; i < TW * TH; i++)
        written += px[i] != 0x11111111u;
    uint32_t c_mid = px[128 * TW + 128], c_r = px[40 * TW + 128], c_g = px[172 * TW + 200], c_b = px[172 * TW + 56];
    kprintf("igd3d: 2. Dreieck: %u Pixel gezeichnet (erwartet etwa 12990), Mitte %#x, oben %#x, rechts %#x, links %#x\n",
            written, c_mid, c_r, c_g, c_b);
    int mr = (int)(c_mid >> 16 & 0xFF), mg = (int)(c_mid >> 8 & 0xFF), mb = (int)(c_mid & 0xFF);
    int ok = written > 12000 && written < 14000 && mr > 70 && mr < 100 && mg > 70 && mg < 100 && mb > 70 && mb < 100 &&
             (c_r >> 16 & 0xFF) > 200 && (c_g >> 8 & 0xFF) > 180 && (c_b & 0xFF) > 180 && px[0] == 0x11111111u &&
             px[TW * TH - 1] == 0x11111111u;
    kprintf("igd3d: 2. Dreieck %s\n", ok ? "stimmt (Flaeche, Farbverlauf, Ecken)" : "FALSCH");
    if (!ok) {
        rc = -34;
        goto out_ring;
    }

    /* 3. Sichtbar (nur an der Konsole): sich drehendes Dreieck, 128 Bilder */
    if (!console_gfx_active() && igd_flip_ready && igd_scr_w >= 800 && igd_scr_h >= 600 && !(igd_scr_stride & 63)) {
        uint32_t cx = igd_scr_w / 2, cy = igd_scr_h / 2, r = (igd_scr_h * 2 / 5) & ~7u;
        load_kernel(K3_PSCOL, (const uint32_t (*)[4])kbuf, asm_ps_color(kbuf, F_0_2, F_0_2, F_0_2, F_1_0));
        const uint32_t bx0 = cx - r - 8, by0 = cy - r - 8, bx1 = cx + r + 8, by1 = cy + r + 8;
        const uint32_t bg[3][4] = {{f32(bx1), f32(by1), 0, 0}, {f32(bx0), f32(by1), 0, 0}, {f32(bx0), f32(by0), 0, 0}};
        Draw3d clear = {igd_surf_a, igd_scr_w, igd_scr_h, igd_scr_stride, 0, PRIM_RECTLIST, 3, K3_PSCOL, &bg[0][0]};
        Draw3d tri = {igd_surf_a, igd_scr_w, igd_scr_h, igd_scr_stride, 1, PRIM_TRILIST, 3, K3_PSINT, &tv[0][0]};
        uint64_t t0 = time_us();
        for (int f = 0; f < 128 && rc == 0; f++) {
            rot_matrix(m, f, (int32_t)r, (int32_t)cx, (int32_t)cy);
            load_kernel(K3_VS, (const uint32_t (*)[4])kbuf, asm_vs_affine(kbuf, m));
            if (draw_3d("3. Hintergrund", &clear, 0, 0) != 0 || draw_3d("3. Dreieck", &tri, 0, 0) != 0)
                rc = -35;
            thread_sleep_ms(16);
        }
        if (rc == 0)
            kprintf("igd3d: 3. drehendes Dreieck: 128 Bilder in %lu ms\n", (unsigned long)((time_us() - t0) / 1000));
        thread_sleep_ms(1000);
        console_repaint();
    } else {
        kprintf("igd3d: sichtbarer Teil nur an der Konsole (ohne Desktop)\n");
    }
    if (rc == 0)
        kprintf("igd3d: Vertex-Shader, Rasterizer und Pixel-Shader arbeiten\n");

out_ring:
    ring_stop();
    for (int i = 0; i < 62; i++)
        igd_wr(GFX_MOCS(i), mocs[i]);
out_fw:
    igd_forcewake_put();
    for (uint32_t i = 0; i < TPAGES; i++)
        igd_ggtt[base + i] = saved[i];
    igd_ggtt_flush();
    pmm_free_frames(mem, TPAGES);
    kprintf("igdtest: %s\n", rc == 0 ? "3D-Pipeline funktioniert" : "3D-Pipeline mit Fehlern, bitte Log schicken");
    return rc;
}

int igd_3d_test(void)
{
    rcs_acquire(1);
    igd_rcs_comp_wait(0);
    comp_ready = 0;
    int rc = test_3d();
    rcs_release();
    return rc;
}
