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

/* Lage in der Zustandsseite (alle Basisadressen = Anfang der Seite) */
#define ST_KERNEL   0x000   /* bis zu 16 Befehle */
#define ST_CURBE    0x100
#define ST_IDD      0x140
#define ST_BT       0x180
#define ST_SURF     0x1C0

/* Zustandsseite: Kernel, Fuellwert, Flaeche (GGTT-Adresse, Breite/Hoehe in Bytes/Zeilen, Zeilenlaenge) */
static void gpgpu_state(uint32_t *st, const uint32_t (*kern)[4], int nk, uint32_t surf, uint32_t w, uint32_t h,
                        uint32_t pitch, uint32_t value)
{
    memset(st, 0, 4096);
    memcpy(st + ST_KERNEL / 4, kern, (uint64_t)nk * 16);
    st[ST_CURBE / 4] = value;
    uint32_t *idd = st + ST_IDD / 4;
    idd[0] = ST_KERNEL;      /* Kernel relativ zur Instruction-Basis */
    idd[2] = 1u << 18;       /* Single Program Flow */
    idd[4] = ST_BT;          /* Binding Table relativ zur Surface-State-Basis, 0 Eintraege vorladen */
    idd[5] = 1u << 16;       /* Konstanten: 1 Register (32 Byte) ab Offset 0 */
    idd[6] = 1;              /* 1 Thread je Gruppe */
    st[ST_BT / 4] = ST_SURF; /* Eintrag 0 -> Surface State */
    uint32_t *ss = st + ST_SURF / 4;
    ss[0] = (1u << 29) | (SURF_R8_UNORM << 18) | (1u << 16) | (1u << 14); /* 2D, R8, VALIGN4, HALIGN4, linear */
    ss[2] = (w - 1) | ((h - 1) << 16);
    ss[3] = pitch - 1;
    ss[7] = (4u << 25) | (5u << 22) | (6u << 19) | (7u << 16); /* Kanalzuordnung R, G, B, A */
    ss[8] = surf;
}

/* Batch: gx x gy Thread-Gruppen, threads = so viele duerfen gleichzeitig laufen */
static uint32_t gpgpu_batch(uint32_t *b, uint32_t st_gtt, uint32_t res_gtt, uint32_t gx, uint32_t gy, uint32_t threads)
{
    uint32_t k = 0;
    b[k++] = PIPELINE_SELECT_GPGPU;
    b[k++] = STATE_BASE_ADDRESS; /* wie IGT: allgemein 0, Surface/Dynamic/Instruction = Zustandsseite */
    b[k++] = 0 | 1;
    b[k++] = 0;
    b[k++] = 0 | 1;              /* Stateless Data Port: MOCS */
    b[k++] = st_gtt | 1;         /* Surface State */
    b[k++] = 0;
    b[k++] = st_gtt | 1;         /* Dynamic State */
    b[k++] = 0;
    b[k++] = 0;                  /* Indirect Object */
    b[k++] = 0;
    b[k++] = st_gtt | 1;         /* Instruction */
    b[k++] = 0;
    b[k++] = 0xFFFFF000u | 1;    /* Groessen */
    b[k++] = (1u << 12) | 1;
    b[k++] = 0xFFFFF000u | 1;
    b[k++] = (1u << 12) | 1;
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

/* Eine Fuellung vorbereiten und ueber den Ring ausfuehren; 0 = fertig (Zeit in *us) */
static uint32_t *g_st, *g_batch;
static uint32_t g_st_gtt, g_batch_gtt, g_res_gtt;

static int gpgpu_fill(const char *what, const uint32_t (*kern)[4], int nk, int block_w, int block_h, uint32_t surf,
                      uint32_t w, uint32_t h, uint32_t pitch, uint32_t value, uint32_t threads, uint64_t *us)
{
    gpgpu_state(g_st, kern, nk, surf, w, h, pitch, value);
    uint32_t k = gpgpu_batch(g_batch, g_st_gtt, g_res_gtt, w / (uint32_t)block_w, h / (uint32_t)block_h, threads);
    igd_clflush((uint64_t)g_st, 4096);
    igd_clflush((uint64_t)g_batch, k * 4);
    begin(4 + 8);
    emit(MI_BATCH_BUFFER_START);
    emit(g_batch_gtt);
    emit(0);
    emit(MI_NOOP);
    return run(what, 2000, us);
}

static uint64_t mbps(uint64_t bytes, uint64_t us)
{
    return bytes / (us ? us : 1);
}

int igd_gpgpu_test(void)
{
    int pre = igd_preflight("Stufe 5 - Programme auf den Recheneinheiten");
    if (pre)
        return pre;

    /* 0. Assembler: baut er den IGT-Kernel bitgenau nach? */
    uint32_t k16[16][4], k32[16][4];
    int n16 = asm_fill16(k16), n32 = asm_fill32x8(k32), diff = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 4; j++)
            if (k16[i][j] != igt_fill_kernel[i][j]) {
                if (!diff)
                    kprintf("igdgpu: Assembler weicht ab bei Befehl %d, Dword %d: %#x statt %#x\n", i, j, k16[i][j],
                            igt_fill_kernel[i][j]);
                diff++;
            }
    if (n16 != 10 || diff) {
        kprintf("igdgpu: Assembler stimmt NICHT mit dem IGT-Kernel ueberein (%d Abweichungen) - Abbruch\n", diff);
        return -30;
    }
    kprintf("igdgpu: Assembler erzeugt den IGT-Kernel bitgenau (10 Befehle); schneller Kernel: %d Befehle\n", n32);

    /* GGTT: Ring (4) + Statusseite (1) + Batch (1) + Zustaende (1) + Ergebnis (1) + Testflaeche (16 = 1024 x 64 Byte)
     * + grosse Flaeche (so gross wie der Bildschirm, Seite fuer Seite) */
    enum { P_RING = 0, P_HWS = 4, P_BATCH = 5, P_STATE = 6, P_RES = 7, P_SURF = 8, P_BIG = 24 };
    uint32_t big_w = igd_flip_ready ? igd_scr_stride : 13760, big_h = igd_flip_ready ? igd_scr_h : 1440;
    big_h &= ~7u;
    uint32_t big_pages = (uint32_t)(((uint64_t)big_w * big_h + 4095) / 4096), pages = P_BIG + big_pages;
    uint32_t base = igd_ggtt_entries / 2 + 0x50000;
    uint64_t *saved = kmalloc(sizeof(uint64_t) * pages), *big = kmalloc(sizeof(uint64_t) * big_pages);
    uint32_t mocs[62];
    int rc = !saved || !big ? -6 : igd_ggtt_claim(base, pages, saved);
    if (rc) {
        kfree(saved);
        kfree(big);
        return rc;
    }
    uint64_t mem = pmm_alloc_frames(P_BIG);
    uint32_t got = 0;
    for (; mem && got < big_pages; got++)
        if (!(big[got] = pmm_alloc_frame()))
            break;
    if (!mem || got < big_pages) {
        kprintf("igdgpu: kein Speicher\n");
        rc = -6;
        goto out_mem;
    }
    memset((void *)mem, 0, P_BIG * 4096);
    igd_clflush(mem, P_BIG * 4096);
    for (uint32_t i = 0; i < P_BIG; i++)
        igd_ggtt[base + i] = (mem + i * 4096) | IGD_PTE_VALID;
    for (uint32_t i = 0; i < big_pages; i++)
        igd_ggtt[base + P_BIG + i] = big[i] | IGD_PTE_VALID;
    igd_ggtt_flush();
    ring = (uint32_t *)mem;
    hws = (volatile uint32_t *)(mem + P_HWS * 4096);
    g_batch = (uint32_t *)(mem + P_BATCH * 4096);
    g_st = (uint32_t *)(mem + P_STATE * 4096);
    volatile uint32_t *res = (volatile uint32_t *)(mem + P_RES * 4096);
    uint8_t *surf = (uint8_t *)(mem + P_SURF * 4096);
    ring_gtt = base << 12;
    hws_gtt = (base + P_HWS) << 12;
    g_batch_gtt = (base + P_BATCH) << 12;
    g_st_gtt = (base + P_STATE) << 12;
    g_res_gtt = (base + P_RES) << 12;
    uint32_t surf_gtt = (base + P_SURF) << 12, big_gtt = (base + P_BIG) << 12;
    seqno = 0;

    if (!igd_forcewake_get()) {
        kprintf("igdgpu: Forcewake nicht bestaetigt\n");
        rc = -9;
        goto out_fw;
    }
    for (int i = 0; i < 62; i++) { /* Cache-Steuerung der Render-Engine: uncached (alte Werte zurueck am Ende) */
        mocs[i] = igd_rd(GFX_MOCS(i));
        igd_wr(GFX_MOCS(i), 0x09);
    }
    kprintf("igdgpu: GPU-Takt %u MHz\n", ((igd_rd(RPSTAT1) >> 23) & 0x1FF) * 50 / 3);
    if (!ring_start()) {
        dump("Ring startet NICHT");
        rc = -10;
        goto out_ring;
    }

    /* 1. Testflaeche mit dem assemblierten IGT-Kernel (wie beim letzten Mal), jedes Byte pruefen */
    uint64_t us = 0;
    if (gpgpu_fill("1. IGT-Kernel aus dem Assembler, 1024 x 64 Bytes", k16, n16, 16, 1, surf_gtt, 1024, 64, 1024, 0x5A, 2,
                   &us) != 0) {
        rc = -20;
        goto out_ring;
    }
    igd_clflush((uint64_t)surf, 16 * 4096);
    int bad = 0;
    for (int i = 0; i < 1024 * 64; i++)
        bad += surf[i] != 0x5A;
    kprintf("igdgpu: Merker %#x, Flaeche: %d von 65536 Bytes falsch\n", rd_scratch(&res[0]), bad);
    if (bad) {
        rc = -21;
        goto out_ring;
    }

    /* 2. Schneller Kernel (32 x 8 Bytes, ganzes Dword), viele Threads: Testflaeche mit einer Farbe, jedes Dword pruefen */
    if (gpgpu_fill("2. schneller Kernel, 1024 x 64 Bytes", k32, n32, 32, 8, surf_gtt, 1024, 64, 1024, 0x11223344u,
                   EU_THREADS, &us) != 0) {
        rc = -22;
        goto out_ring;
    }
    igd_clflush((uint64_t)surf, 16 * 4096);
    const uint32_t *sw = (const uint32_t *)surf;
    bad = 0;
    int first = -1;
    for (int i = 0; i < 1024 * 64 / 4; i++)
        if (sw[i] != 0x11223344u) {
            if (first < 0)
                first = i;
            bad++;
        }
    kprintf("igdgpu: schneller Kernel: %d von 16384 Dwords falsch", bad);
    if (first >= 0)
        kprintf(" (erstes bei Byte %d, Zeile %d: %#x)", first * 4 % 1024, first * 4 / 1024, sw[first]);
    kprintf("\n");
    if (bad) {
        rc = -23;
        goto out_ring;
    }

    /* 3. Tempo: bildschirmgrosse Flaeche (im RAM), drei Stufen */
    uint64_t bytes = (uint64_t)big_w * big_h, t1, t2, t3;
    if (gpgpu_fill("3a. IGT-Kernel, 2 Threads gleichzeitig", k16, n16, 16, 1, big_gtt, big_w, big_h, big_w, 0x33, 2, &t1) ||
        gpgpu_fill("3b. IGT-Kernel, 168 Threads gleichzeitig", k16, n16, 16, 1, big_gtt, big_w, big_h, big_w, 0x44,
                   EU_THREADS, &t2) ||
        gpgpu_fill("3c. schneller Kernel, 168 Threads", k32, n32, 32, 8, big_gtt, big_w, big_h, big_w, 0x55667788u,
                   EU_THREADS, &t3)) {
        rc = -24;
        goto out_ring;
    }
    kprintf("igdgpu: %u x %u Bytes (%lu KB): IGT-Kernel %lu us (%lu MB/s), mit 168 Threads %lu us (%lu MB/s), "
            "schneller Kernel %lu us (%lu MB/s)\n",
            big_w, big_h, (unsigned long)(bytes / 1024), (unsigned long)t1, (unsigned long)mbps(bytes, t1), (unsigned long)t2,
            (unsigned long)mbps(bytes, t2), (unsigned long)t3, (unsigned long)mbps(bytes, t3));
    bad = 0; /* Stichprobe: je Seite ein Dword */
    for (uint32_t i = 0; i < big_pages; i++) {
        uint32_t off = (i * 4096 + 1028) % 4096;
        if ((uint64_t)i * 4096 + off + 4 > bytes)
            break;
        igd_clflush(big[i] + off, 4);
        bad += *(volatile uint32_t *)(big[i] + off) != 0x55667788u;
    }
    kprintf("igdgpu: Stichprobe (je Seite ein Dword): %d falsch\n", bad);
    if (bad) {
        rc = -25;
        goto out_ring;
    }

    /* 4. Sichtbar (nur an der Konsole): zwei farbige Baender quer ueber den Bildschirm, 4 s, dann zurueck */
    if (!console_gfx_active() && igd_flip_ready && igd_scr_stride <= 16384 && igd_scr_stride % 32 == 0) {
        static const struct { uint32_t y, h, c; } band[2] = {{448, 128, 0x2060C0}, {704, 128, 0xE08020}};
        for (int i = 0; i < 2 && rc == 0; i++) { /* y Vielfaches von 64: Anfang auf einer Seitengrenze */
            if (band[i].y + band[i].h > igd_scr_h)
                break;
            if (gpgpu_fill(i ? "oranges Band" : "blaues Band", k32, n32, 32, 8, igd_surf_a + band[i].y * igd_scr_stride,
                           igd_scr_stride, band[i].h, igd_scr_stride, band[i].c, EU_THREADS, &us) != 0)
                rc = -26;
            else
                kprintf("igdgpu: Band %d: %u x %u Bytes in %lu us\n", i + 1, igd_scr_stride, band[i].h, (unsigned long)us);
        }
        thread_sleep_ms(4000);
        console_repaint();
    } else {
        kprintf("igdgpu: sichtbarer Teil nur an der Konsole (ohne Desktop)\n");
    }
    if (rc == 0)
        kprintf("igdgpu: Assembler und schneller Kernel laufen\n");

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
