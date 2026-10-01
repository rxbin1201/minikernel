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

/* ---------- Stufe 5, Schritte 2+3: erstes Programm auf den Recheneinheiten (igdtest gpgpu) ----------
 *
 * GPGPU-Pipeline der Render-Engine: ein Kernel in EU-Maschinensprache fuellt eine Flaeche (8 Bit je Pixel) mit einem
 * Wert aus dem Konstantenspeicher (CURBE). Wie der Fuelltest des Linux-Testwerkzeugs IGT (gpgpu_fill, Gen8/Gen9):
 * jeder Hardware-Thread (SIMD16) bekommt seine Gruppen-Nummer (x, y) in r0, rechnet x * 16 und schreibt per
 * "Media Block Write" (Data Port 1) 16 Bytes an (x * 16, y). Der Batch:
 *   PIPELINE_SELECT (GPGPU), STATE_BASE_ADDRESS, MEDIA_VFE_STATE, MEDIA_CURBE_LOAD, MEDIA_INTERFACE_DESCRIPTOR_LOAD,
 *   GPGPU_WALKER (eine Thread-Gruppe je 16 Bytes x 1 Zeile), MEDIA_STATE_FLUSH, PIPE_CONTROL (Caches leeren)
 * Zustaende, Kernel und Konstanten liegen in einer Seite; alle Basisadressen zeigen auf sie. Die Cache-Steuerung der
 * Render-Engine (MOCS) steht waehrend des Tests auf uncached (wie beim Blitter: sonst sieht die Anzeige nicht alles).
 * Geprueft wird erst im RAM (jedes Byte), dann sichtbar: zwei graue Baender quer ueber den Bildschirm (nur an der
 * Konsole, nicht unter dem Desktop). */

/* Der Kernel (Gen8/Gen9-Befehlsformat, je 128 Bit), aus IGT (lib/gpgpu_fill.c):
 *   mov (4)  r1.0<1>:ub   r1.0<0;1,0>:ub        Farbbyte viermal (ein Dword)
 *   mul (1)  r2.0<1>:ud   r0.1<0;1,0>:ud 16     x = Gruppe x * 16 Bytes
 *   mov (1)  r2.4<1>:ud   r0.6:ud               y = Gruppe y
 *   mov (8)  r4.0<1>:ud   r0.0<8;8,1>:ud        Nachrichtenkopf = r0
 *   mov (2)  r4.0<1>:ud   r2.0<2;2,1>:ud        Kopf: x, y
 *   mov (1)  r4.8<1>:ud   0xf                   Block 16 x 1 Bytes (Breite - 1, Hoehe - 1)
 *   mov (16) r5.0<1>:ud   r1.0<0;1,0>:ud        Daten
 *   send (16) r32 r4      Data Port 1, Media Block Write, Binding-Table-Eintrag 0
 *   mov (8)  r112<1>:ud   r0.0<8;8,1>:ud
 *   send (16) null r112   Thread Spawner: Thread-Ende (EOT) */
static const uint32_t gpgpu_kernel[][4] = {
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

/* Lage in der Zustandsseite (alle Basisadressen = Anfang der Seite) */
#define ST_KERNEL   0x000
#define ST_CURBE    0x100
#define ST_IDD      0x140
#define ST_BT       0x180
#define ST_SURF     0x1C0

/* Zustandsseite fuer eine Fuellung vorbereiten: Flaeche (GGTT-Adresse, Breite/Hoehe in Bytes/Zeilen, Zeilenlaenge) */
static void gpgpu_state(uint32_t *st, uint32_t surf, uint32_t w, uint32_t h, uint32_t pitch, uint8_t value)
{
    memset(st, 0, 4096);
    memcpy(st + ST_KERNEL / 4, gpgpu_kernel, sizeof(gpgpu_kernel));
    ((uint8_t *)st)[ST_CURBE] = value;
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

/* Batch fuer eine Fuellung: w Bytes (Vielfaches von 16) x h Zeilen */
static uint32_t gpgpu_batch(uint32_t *b, uint32_t st_gtt, uint32_t res_gtt, uint32_t w, uint32_t h)
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
    b[k++] = (1u << 16) | (1u << 8); /* Threads, URB-Eintraege */
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
    b[k++] = w / 16;             /* Gruppen in x */
    b[k++] = 0;                  /* Gruppe y ab 0 */
    b[k++] = 0;
    b[k++] = h;                  /* Gruppen in y */
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

/* Batch ueber den Ring ausfuehren; 0 = fertig, sonst haengt die Engine (Zustand ausgegeben, zurueckgesetzt) */
static int gpgpu_run(const char *what, uint32_t batch_gtt, uint64_t *us)
{
    begin(4 + 8);
    emit(MI_BATCH_BUFFER_START);
    emit(batch_gtt);
    emit(0);
    emit(MI_NOOP);
    return run(what, 1000, us);
}

int igd_gpgpu_test(void)
{
    int pre = igd_preflight("Stufe 5 - erstes Programm auf den Recheneinheiten");
    if (pre)
        return pre;

    /* GGTT: Ring (4) + Statusseite (1) + Batch (1) + Zustaende (1) + Ergebnis (1) + Testflaeche (16 = 1024 x 64 Byte) */
    enum { P_RING = 0, P_HWS = 4, P_BATCH = 5, P_STATE = 6, P_RES = 7, P_SURF = 8, PAGES = 24 };
    uint32_t base = igd_ggtt_entries / 2 + 0x50000;
    uint64_t saved[PAGES];
    uint32_t mocs[62];
    int rc = igd_ggtt_claim(base, PAGES, saved);
    if (rc)
        return rc;
    uint64_t mem = pmm_alloc_frames(PAGES);
    if (!mem) {
        kprintf("igdgpu: kein Speicher\n");
        return -6;
    }
    memset((void *)mem, 0, PAGES * 4096);
    igd_clflush(mem, PAGES * 4096);
    for (uint32_t i = 0; i < PAGES; i++)
        igd_ggtt[base + i] = (mem + i * 4096) | IGD_PTE_VALID;
    igd_ggtt_flush();
    ring = (uint32_t *)mem;
    hws = (volatile uint32_t *)(mem + P_HWS * 4096);
    uint32_t *batch = (uint32_t *)(mem + P_BATCH * 4096), *st = (uint32_t *)(mem + P_STATE * 4096);
    volatile uint32_t *res = (volatile uint32_t *)(mem + P_RES * 4096);
    uint8_t *surf = (uint8_t *)(mem + P_SURF * 4096);
    ring_gtt = base << 12;
    hws_gtt = (base + P_HWS) << 12;
    uint32_t batch_gtt = (base + P_BATCH) << 12, st_gtt = (base + P_STATE) << 12, res_gtt = (base + P_RES) << 12;
    uint32_t surf_gtt = (base + P_SURF) << 12;
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
    kprintf("igdgpu: GPU-Takt %u MHz, MOCS[0] vorher %#x\n", ((igd_rd(RPSTAT1) >> 23) & 0x1FF) * 50 / 3, mocs[0]);
    if (!ring_start()) {
        dump("Ring startet NICHT");
        rc = -10;
        goto out_ring;
    }

    /* 1. Testflaeche im RAM: 1024 x 64 Bytes mit 0x5A fuellen, jedes Byte pruefen */
    gpgpu_state(st, surf_gtt, 1024, 64, 1024, 0x5A);
    uint32_t k = gpgpu_batch(batch, st_gtt, res_gtt, 1024, 64);
    igd_clflush((uint64_t)st, 4096);
    igd_clflush((uint64_t)batch, k * 4);
    uint64_t us = 0;
    if (gpgpu_run("Kernel fuellt 1024 x 64 Bytes (4096 Threads)", batch_gtt, &us) != 0) {
        rc = -20;
        goto out_ring;
    }
    igd_clflush((uint64_t)surf, 16 * 4096);
    uint32_t mark = rd_scratch(&res[0]);
    int bad = 0, first = -1;
    for (int i = 0; i < 1024 * 64; i++)
        if (surf[i] != 0x5A) {
            if (first < 0)
                first = i;
            bad++;
        }
    kprintf("igdgpu: Merker %#x (%s), Flaeche: %d von 65536 Bytes falsch", mark, mark == 0x600DF111u ? "ok" : "FEHLT", bad);
    if (first >= 0)
        kprintf(" (erstes bei x %d, y %d: %#x)", first % 1024, first / 1024, surf[first]);
    kprintf("\nigdgpu: Bytes 0-15: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            surf[0], surf[1], surf[2], surf[3], surf[4], surf[5], surf[6], surf[7], surf[8], surf[9], surf[10], surf[11],
            surf[12], surf[13], surf[14], surf[15]);
    if (bad) {
        dump("nach dem Kernel");
        rc = -21;
        goto out_ring;
    }
    kprintf("igdgpu: Die Recheneinheiten haben die Flaeche richtig gefuellt (%lu us)\n", (unsigned long)us);

    /* 2. Sichtbar (nur an der Konsole): zwei Baender quer ueber den angezeigten Framebuffer, 3 s, dann zurueck */
    if (!console_gfx_active() && igd_flip_ready && igd_scr_stride <= 16384) {
        static const struct { uint32_t y, h; uint8_t v; } band[2] = {{448, 128, 0x40}, {704, 128, 0xC0}};
        for (int i = 0; i < 2 && rc == 0; i++) { /* y Vielfaches von 64: Anfang auf einer Seitengrenze */
            if (band[i].y + band[i].h > igd_scr_h)
                break;
            gpgpu_state(st, igd_surf_a + band[i].y * igd_scr_stride, igd_scr_stride, band[i].h, igd_scr_stride, band[i].v);
            k = gpgpu_batch(batch, st_gtt, res_gtt, igd_scr_stride, band[i].h);
            igd_clflush((uint64_t)st, 4096);
            igd_clflush((uint64_t)batch, k * 4);
            if (gpgpu_run(i ? "helles Band auf dem Bildschirm" : "dunkles Band auf dem Bildschirm", batch_gtt, &us) != 0)
                rc = -22;
            else
                kprintf("igdgpu: Band %d: %u x %u Bytes in %lu us\n", i + 1, igd_scr_stride, band[i].h, (unsigned long)us);
        }
        thread_sleep_ms(3000);
        console_repaint();
    } else {
        kprintf("igdgpu: sichtbarer Teil nur an der Konsole (ohne Desktop)\n");
    }
    if (rc == 0)
        kprintf("igdgpu: erstes Programm auf den Recheneinheiten laeuft\n");

out_ring:
    ring_stop();
    for (int i = 0; i < 62; i++)
        igd_wr(GFX_MOCS(i), mocs[i]);
out_fw:
    igd_forcewake_put();
    for (uint32_t i = 0; i < PAGES; i++)
        igd_ggtt[base + i] = saved[i];
    igd_ggtt_flush();
    pmm_free_frames(mem, PAGES);
    kprintf("igdtest: %s\n", rc == 0 ? "GPGPU-Kernel laeuft" : "GPGPU-Kernel mit Fehlern, bitte Log schicken");
    return rc;
}
