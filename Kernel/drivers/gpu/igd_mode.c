/* Intel-Grafik Gen9, Stufe 4: Bildschirmmodi. Fest eingebaut: die Modi des Monitors (EDID) im Betrieb umschalten
 * (igd_mode_set, SYS_SETMODE, resolution). Die Tests: Teil 1 (igdtest edid): nur lesen. Teil 2 (igdtest scale): kleinere
 * Aufloesung, die der Skalierer der Pipe auf den Bildschirm hochrechnet; Takt und Zeitablauf bleiben. Teil 3
 * (igdtest mode): echter Moduswechsel am HDMI-Anschluss (Pipe/Port aus, DPLL neu, Zeitablauf, wieder an).
 *
 *
 * - Monitordaten (EDID) ueber den DDC-Bus des Anschlusses. Die Grafik hat dafuer einen eigenen I2C-Controller im
 *   PCH (GMBUS): Pin-Paar waehlen, Adresse 0x50, ab Offset 0 lesen. Blockweise 4 Byte ueber GMBUS3.
 * - Wie die Firmware die Pipe eingestellt hat: Zeitablauf (HTOTAL/HBLANK/HSYNC, V entsprechend) und der Taktgeber
 *   (DPLL), aus dem sich der Pixeltakt ergibt.
 * Registerangaben nach Intels "Programmer's Reference Manual" (Skylake/Kaby Lake) und dem Linux-i915. */

#include "drivers/gpu/igd_internal.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "console/console.h"
#include "core/cmdline.h"
#include "core/sched.h"
#include "lib/kprintf.h"
#include "lib/string.h"

/* ---------- Register ---------- */

#define PIPE_OFF(p)           (0x1000u * (uint32_t)(p))
#define HTOTAL(t)             (0x60000 + PIPE_OFF(t))   /* (gesamt-1) << 16 | (sichtbar-1) */
#define HBLANK(t)             (0x60004 + PIPE_OFF(t))   /* (Ende-1) << 16 | (Anfang-1) */
#define HSYNC(t)              (0x60008 + PIPE_OFF(t))
#define VTOTAL(t)             (0x6000C + PIPE_OFF(t))
#define VBLANK(t)             (0x60010 + PIPE_OFF(t))
#define VSYNC(t)              (0x60014 + PIPE_OFF(t))
#define PIPESRC(p)            (0x6001C + PIPE_OFF(p))
#define TRANS_DDI_FUNC_CTL(t) (0x60400 + PIPE_OFF(t))   /* Bit 31 an, 30:28 Port, 26:24 Modus, 17/16 Sync-Polaritaet */
#define PS_REG(p, i, off)     (0x68000 + 0x800u * (uint32_t)(p) + 0x100u * (uint32_t)(i) + (off))
#define PS_CTRL(p, i)         PS_REG(p, i, 0x180)       /* Skalierer: Bit 31 an, 28 Modus, 25-27 Ebene (0 = Pipe) */
#define PS_WIN_POS(p, i)      PS_REG(p, i, 0x170)       /* Zielfenster x << 16 | y */
#define PS_WIN_SZ(p, i)       PS_REG(p, i, 0x174)       /* Zielfenster w << 16 | h; Schreiben uebernimmt alles */
#define PS_VPHASE(p, i)       PS_REG(p, i, 0x188)
#define PS_HPHASE(p, i)       PS_REG(p, i, 0x194)
#define PS_SCALER_EN          (1u << 31)
#define PIPE_FRMCOUNT(p)      (0x70040 + PIPE_OFF(p))
#define PLANE_STRIDE(p)       (0x70188 + PIPE_OFF(p))
#define PLANE_SIZE(p)         (0x70190 + PIPE_OFF(p))
#define PLANE_SURF(p)         (0x7019C + PIPE_OFF(p))
#define PLANE_SURFLIVE(p)     (0x701AC + PIPE_OFF(p))
#define PIPECONF(p)           (0x70008 + PIPE_OFF(p))   /* Bit 31 an, Bit 30 laeuft */
#define PIPE_DATA_M1(t)       (0x60030 + PIPE_OFF(t))   /* DP: TU-Groesse << 25 | Daten-M */
#define PIPE_DATA_N1(t)       (0x60034 + PIPE_OFF(t))
#define PIPE_LINK_M1(t)       (0x60040 + PIPE_OFF(t))   /* DP: Pixeltakt : Linktakt */
#define PIPE_LINK_N1(t)       (0x60044 + PIPE_OFF(t))
#define DP_TP_CTL(port)       (0x64040 + 0x100u * (uint32_t)(port)) /* DP-Linkschicht: Bits 10:8 Muster */
#define DP_TP_STATUS(port)    (0x64044 + 0x100u * (uint32_t)(port))
#define DP_TP_TRAIN_MASK      (7u << 8)
#define DP_TP_TRAIN_IDLE      (2u << 8)
#define DP_TP_TRAIN_NORMAL    (3u << 8)
#define DP_TP_IDLE_DONE       (1u << 25)
#define PLANE_WM(p, lvl)      (0x70240 + PIPE_OFF(p) + 4u * (uint32_t)(lvl))
#define CUR_WM(p, lvl)        (0x70140 + PIPE_OFF(p) + 4u * (uint32_t)(lvl))
#define PLANE_BUF_CFG(p)      (0x7027C + PIPE_OFF(p))
#define CUR_BUF_CFG(p)        (0x7017C + PIPE_OFF(p))
#define PLANE_CTL(p)          (0x70180 + PIPE_OFF(p))
#define VSYNCSHIFT(t)         (0x60028 + PIPE_OFF(t))
#define TRANS_CLK_SEL(t)      (0x46140 + 4u * (uint32_t)(t)) /* Takt des Transcoders: (Port + 1) << 29, 0 = aus */
#define DDI_BUF_CTL(port)     (0x64000 + 0x100u * (uint32_t)(port))
#define CUR_CTL(p)            (0x70080 + PIPE_OFF(p))
#define CUR_BASE(p)           (0x70084 + PIPE_OFF(p))

#define DPLL_CTRL1            0x6C058                   /* je DPLL 6 Bit: Override, Linkrate, SSC, HDMI-Modus */
#define DPLL_CTRL2            0x6C05C                   /* welcher DDI an welchem DPLL haengt */
#define DPLL_STATUS           0x6C060
#define DPLL_CFGCR1(id)       (0x6C040 + ((uint32_t)(id) - 1) * 8) /* DPLL1-3: DCO-Frequenz */
#define DPLL_CFGCR2(id)       (0x6C044 + ((uint32_t)(id) - 1) * 8) /* DPLL1-3: Teiler */
#define LCPLL1_CTL            0x46010                   /* DPLL0 */
#define LCPLL2_CTL            0x46014                   /* DPLL1 */
#define WRPLL_CTL(i)          (0x46040 + 0x20u * (uint32_t)(i)) /* DPLL2, DPLL3 */
#define CDCLK_CTL             0x46000

#define GMBUS0                0xC5100                   /* Takt | Pin-Paar */
#define GMBUS1                0xC5104                   /* Befehl */
#define GMBUS2                0xC5108                   /* Status */
#define GMBUS3                0xC510C                   /* Daten, 4 Byte */
#define GMBUS5                0xC5120                   /* 2-Byte-Index (aus) */
#define GMBUS_SW_CLR_INT      (1u << 31)
#define GMBUS_SW_RDY          (1u << 30)
#define GMBUS_CYCLE_WAIT      (1u << 25)
#define GMBUS_CYCLE_INDEX     (2u << 25)
#define GMBUS_CYCLE_STOP      (4u << 25)
#define GMBUS_HW_WAIT_PHASE   (1u << 14)
#define GMBUS_HW_RDY          (1u << 11)
#define GMBUS_SATOER          (1u << 10)                /* keine Antwort (NAK) */
#define GMBUS_ACTIVE          (1u << 9)
#define SOUTH_DSPCLK_GATE_D   0xC2020
#define PCH_GMBUSUNIT_CLOCK_GATE_DISABLE (1u << 31)     /* WaDisableGMBUSClockGating (SPT/KBP-PCH) */

#define GEN9_HDMI_MAX_KHZ     300000                    /* Gen9: HDMI hoechstens 300 MHz Pixeltakt (i915) */

/* ---------- GMBUS (I2C) ---------- */

static int gmbus_wait(uint32_t bits, int us)
{
    uint64_t end = time_us() + (uint64_t)us;
    for (;;) {
        uint32_t st = igd_rd(GMBUS2);
        if (st & GMBUS_SATOER)
            return -1;
        if (st & bits)
            return 0;
        if (time_us() > end)
            return -2;
    }
}

static void gmbus_reset(void)
{
    igd_wr(GMBUS1, GMBUS_SW_CLR_INT);
    igd_wr(GMBUS1, 0);
    igd_wr(GMBUS0, 0);
}

/* Liest len (<= 256) Bytes ab Offset index vom Geraet addr am Pin-Paar pin. 0 = ok, -1 = keine Antwort, -2 = Zeit */
static int gmbus_read(int pin, uint8_t addr, uint8_t index, uint8_t *buf, uint32_t len)
{
    uint32_t gate = igd_rd(SOUTH_DSPCLK_GATE_D);
    igd_wr(SOUTH_DSPCLK_GATE_D, gate | PCH_GMBUSUNIT_CLOCK_GATE_DISABLE);
    gmbus_reset();
    igd_wr(GMBUS5, 0);
    igd_wr(GMBUS0, (uint32_t)pin); /* 100 kHz */
    igd_wr(GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_INDEX | GMBUS_CYCLE_WAIT | (len << 16) | ((uint32_t)index << 8) |
                       ((uint32_t)addr << 1) | 1);
    int rc = 0;
    for (uint32_t got = 0; got < len && rc == 0;) {
        rc = gmbus_wait(GMBUS_HW_RDY, 50000);
        if (rc)
            break;
        uint32_t v = igd_rd(GMBUS3);
        for (int k = 0; k < 4 && got < len; k++, v >>= 8)
            buf[got++] = (uint8_t)v;
    }
    if (rc == 0)
        rc = gmbus_wait(GMBUS_HW_WAIT_PHASE, 50000);
    if (rc == 0) {
        igd_wr(GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_STOP);
        uint64_t end = time_us() + 10000;
        while ((igd_rd(GMBUS2) & GMBUS_ACTIVE) && time_us() < end)
            ;
        igd_wr(GMBUS0, 0);
    } else {
        gmbus_reset();
    }
    igd_wr(SOUTH_DSPCLK_GATE_D, gate);
    return rc;
}

/* ---------- EDID auswerten ---------- */

static const char *port_name(int port)
{
    static const char *n[] = {"A", "B", "C", "D", "E"};
    return port >= 0 && port < 5 ? n[port] : "?";
}

/* Pin-Paar des Anschlusses: B -> 1, C -> 2, D -> 3 (so auf dem Test-PC: i5-8400T mit 400er-PCH; i915 nennt das die
 * CNP-Belegung). Aeltere Gen9-Boards (SPT/KBP-PCH) haben B -> 5, C -> 4, D -> 6; die werden danach probiert. */
static int ddc_pin(int port)
{
    return port >= 1 && port <= 3 ? port : 0;
}

/* Grenze fuer den Hinweis hinter jedem Zeitablauf (HDMI: 300 MHz; DisplayPort: je nach Verbindung) */
static uint32_t    dtd_limit = GEN9_HDMI_MAX_KHZ;
static const char *dtd_note = "  [ueber 300 MHz: per HDMI auf Gen9 nicht moeglich]";

static int edid_checksum(const uint8_t *b)
{
    uint8_t s = 0;
    for (int i = 0; i < 128; i++)
        s = (uint8_t)(s + b[i]);
    return s == 0;
}

typedef struct {
    uint32_t khz;                   /* Pixeltakt */
    uint32_t ha, hso, hsw, ht;      /* sichtbar, Sync-Abstand, Sync-Breite, gesamt */
    uint32_t va, vso, vsw, vt;
    int      hpos, vpos, interlaced; /* Sync-Polaritaet positiv */
} Timing;

/* Detailed Timing Descriptor (18 Byte) zerlegen; 0 = kein Zeitablauf (Beschreibungsblock) */
static int dtd_parse(const uint8_t *d, Timing *t)
{
    t->khz = (uint32_t)(d[0] | d[1] << 8) * 10;
    if (!t->khz)
        return 0;
    t->ha = d[2] | (uint32_t)(d[4] & 0xF0) << 4;
    t->ht = t->ha + (d[3] | (uint32_t)(d[4] & 0x0F) << 8);
    t->va = d[5] | (uint32_t)(d[7] & 0xF0) << 4;
    t->vt = t->va + (d[6] | (uint32_t)(d[7] & 0x0F) << 8);
    t->hso = d[8] | (uint32_t)(d[11] & 0xC0) << 2;
    t->hsw = d[9] | (uint32_t)(d[11] & 0x30) << 4;
    t->vso = (d[10] >> 4) | (uint32_t)(d[11] & 0x0C) << 2;
    t->vsw = (d[10] & 0xF) | (uint32_t)(d[11] & 0x03) << 4;
    t->interlaced = d[17] >> 7;
    int sep = ((d[17] >> 3) & 3) == 3; /* digital getrennt; sonst (selten) wie VESA ueblich: H+ V+ */
    t->hpos = sep ? (d[17] >> 1) & 1 : 1;
    t->vpos = sep ? (d[17] >> 2) & 1 : 1;
    return 1;
}

static uint32_t hz100(const Timing *t)
{
    return (uint32_t)((uint64_t)t->khz * 100000 / ((uint64_t)t->ht * t->vt));
}

/* Ergebnis: Pixeltakt in kHz, 0 = kein Zeitablauf */
static uint32_t dtd_print(const uint8_t *d, const char *what)
{
    Timing t;
    if (!dtd_parse(d, &t))
        return 0;
    uint32_t hz = hz100(&t);
    kprintf("igdmode:   %s %ux%u%s @ %u.%02u Hz, Pixeltakt %u.%02u MHz, gesamt %ux%u, HSync %u+%u, VSync %u+%u, %s %s%s\n",
            what, t.ha, t.va, t.interlaced ? "i" : "", hz / 100, hz % 100, t.khz / 1000, t.khz % 1000 / 10, t.ht, t.vt,
            t.hso, t.hsw, t.vso, t.vsw, t.hpos ? "H+" : "H-", t.vpos ? "V+" : "V-",
            t.khz > dtd_limit ? dtd_note : "");
    return t.khz;
}

static void edid_base(const uint8_t *e)
{
    char mfg[4] = {(char)('@' + ((e[8] >> 2) & 31)), (char)('@' + (((e[8] & 3) << 3) | (e[9] >> 5))),
                   (char)('@' + (e[9] & 31)), 0};
    kprintf("igdmode: EDID %u.%u, Hersteller %s, Produkt %#x, gebaut %u (Woche %u), %s, %ux%u cm\n", e[18], e[19], mfg,
            (unsigned)(e[10] | e[11] << 8), 1990u + e[17], e[16], (e[20] & 0x80) ? "digital" : "analog", e[21], e[22]);
    if (e[20] & 0x80) {
        static const char *bpc[] = {"?", "6", "8", "10", "12", "14", "16", "?"};
        static const char *ifc[] = {"?", "DVI", "HDMI-a", "HDMI-b", "MDDI", "DisplayPort", "?", "?"};
        if (e[18] == 1 && e[19] >= 4)
            kprintf("igdmode:   Farbtiefe %s Bit, Schnittstelle %s\n", bpc[(e[20] >> 4) & 7], ifc[e[20] & 7]);
    }

    static const char *est[] = {"720x400@70", "720x400@88", "640x480@60", "640x480@67", "640x480@72", "640x480@75",
                                "800x600@56", "800x600@60", "800x600@72", "800x600@75", "832x624@75", "1024x768@87i",
                                "1024x768@60", "1024x768@70", "1024x768@75", "1280x1024@75", "1152x870@75"};
    char line[256];
    int n = 0;
    line[0] = 0;
    for (int i = 0; i < 17; i++) {
        int byte = 35 + i / 8, bit = 7 - i % 8;
        if ((e[byte] >> bit) & 1)
            n += ksnprintf(line + n, sizeof(line) - (size_t)n, " %s", est[i]);
    }
    if (n)
        kprintf("igdmode:   Standardmodi (VESA):%s\n", line);

    n = 0;
    line[0] = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t a = e[38 + 2 * i], b = e[39 + 2 * i];
        if (a == 1 && b == 1)
            continue;
        uint32_t w = ((uint32_t)a + 31) * 8, h;
        switch (b >> 6) {
        case 0: h = (e[18] == 1 && e[19] < 3) ? w : w * 10 / 16; break;
        case 1: h = w * 3 / 4; break;
        case 2: h = w * 4 / 5; break;
        default: h = w * 9 / 16; break;
        }
        n += ksnprintf(line + n, sizeof(line) - (size_t)n, " %ux%u@%u", w, h, (b & 0x3F) + 60u);
    }
    if (n)
        kprintf("igdmode:   weitere Modi:%s\n", line);

    for (int i = 0; i < 4; i++) {
        const uint8_t *d = e + 54 + 18 * i;
        if (d[0] || d[1]) {
            dtd_print(d, i == 0 ? "bevorzugt:" : "Zeitablauf:");
            continue;
        }
        if (d[3] == 0xFC || d[3] == 0xFF || d[3] == 0xFE) {
            char s[14];
            int k = 0;
            for (; k < 13 && d[5 + k] != 0x0A; k++)
                s[k] = (d[5 + k] >= 0x20 && d[5 + k] < 0x7F) ? (char)d[5 + k] : '.';
            s[k] = 0;
            kprintf("igdmode:   %s: %s\n", d[3] == 0xFC ? "Name" : d[3] == 0xFF ? "Seriennummer" : "Text", s);
        } else if (d[3] == 0xFD) {
            uint32_t vmin = d[5] + ((d[4] & 1) ? 255u : 0), vmax = d[6] + ((d[4] & 2) ? 255u : 0);
            uint32_t hmin = d[7] + ((d[4] & 4) ? 255u : 0), hmax = d[8] + ((d[4] & 8) ? 255u : 0);
            kprintf("igdmode:   Grenzen: %u-%u Hz vertikal, %u-%u kHz horizontal, Pixeltakt bis %u MHz\n", vmin, vmax,
                    hmin, hmax, d[9] * 10u);
        }
    }
}

static const char *vic_name(uint32_t v)
{
    switch (v) {
    case 1: return "640x480@60";
    case 2: return "720x480@60(4:3)";
    case 3: return "720x480@60(16:9)";
    case 4: return "1280x720@60";
    case 5: return "1920x1080i@60";
    case 16: return "1920x1080@60";
    case 17: return "720x576@50(4:3)";
    case 18: return "720x576@50(16:9)";
    case 19: return "1280x720@50";
    case 20: return "1920x1080i@50";
    case 31: return "1920x1080@50";
    case 32: return "1920x1080@24";
    case 33: return "1920x1080@25";
    case 34: return "1920x1080@30";
    case 63: return "1920x1080@120";
    case 93: return "3840x2160@24";
    case 94: return "3840x2160@25";
    case 95: return "3840x2160@30";
    case 96: return "3840x2160@50";
    case 97: return "3840x2160@60";
    default: return 0;
    }
}

static void edid_cta(const uint8_t *e)
{
    uint32_t dtd = e[2];
    kprintf("igdmode: CTA-861-Erweiterung Rev. %u%s%s\n", e[1], (e[3] & 0x40) ? ", Audio" : "",
            (e[3] & 0x30) ? ", YCbCr" : "");
    for (uint32_t i = 4; i < dtd && i < 127;) {
        uint32_t tag = e[i] >> 5, len = e[i] & 31;
        const uint8_t *b = e + i + 1;
        if (tag == 2) { /* Video: Liste von VICs */
            char line[256];
            int n = 0;
            for (uint32_t k = 0; k < len; k++) {
                uint32_t v = b[k] & 0x7F;
                if (v == 0)
                    v = b[k]; /* VIC >= 128 ohne "nativ"-Bit */
                const char *nm = vic_name(v);
                n += nm ? ksnprintf(line + n, sizeof(line) - (size_t)n, " %s", nm)
                        : ksnprintf(line + n, sizeof(line) - (size_t)n, " VIC%u", v);
                if (n >= (int)sizeof(line) - 20)
                    break;
            }
            kprintf("igdmode:   Videomodi:%s\n", line);
        } else if (tag == 3 && len >= 3) { /* herstellerspezifisch */
            uint32_t oui = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16;
            if (oui == 0x000C03 && len >= 7 && b[6])
                kprintf("igdmode:   HDMI 1.x: TMDS-Takt bis %u MHz\n", b[6] * 5u);
            else if (oui == 0x000C03)
                kprintf("igdmode:   HDMI 1.x (ohne Taktangabe)\n");
            if (oui == 0xC45DD8 && len >= 5)
                kprintf("igdmode:   HDMI 2.x (Forum): TMDS-Takt bis %u MHz\n", b[4] * 5u);
        }
        i += 1 + len;
    }
    for (uint32_t i = dtd; dtd >= 4 && i + 18 <= 127; i += 18)
        if (!dtd_print(e + i, "Zeitablauf:"))
            break;
}

/* ---------- Aktueller Zustand der Pipe ---------- */

static uint32_t cfg_khz(uint32_t cfg1, uint32_t cfg2) /* HDMI-Pixeltakt aus CFGCR1/2, 0 = ungueltig */
{
    uint64_t dco = 24000ULL * (cfg1 & 0x1FF) + 24000ULL * ((cfg1 >> 9) & 0x7FFF) / 0x8000; /* kHz */
    static const uint32_t pd[] = {1, 2, 3, 0, 7, 0, 0, 0}, kd[] = {5, 2, 3, 1};
    uint32_t p = pd[(cfg2 >> 2) & 7], k = kd[(cfg2 >> 5) & 3], q = (cfg2 & (1u << 7)) ? (cfg2 >> 8) & 0xFF : 1;
    return p && q && k ? (uint32_t)(dco / (p * q * k) / 5) : 0;
}

static uint32_t dpll_khz(int id, int *hdmi)
{
    uint32_t c1 = igd_rd(DPLL_CTRL1);
    *hdmi = (c1 >> (id * 6 + 5)) & 1;
    if (id == 0 || !*hdmi) { /* DP-Linkrate (nicht fuer HDMI benutzt) */
        static const uint32_t rate[] = {2700000, 1350000, 810000, 1620000, 1080000, 2160000};
        uint32_t r = (c1 >> (id * 6 + 1)) & 7;
        return r < 6 ? rate[r] / 10 : 0; /* Link-Symboltakt in kHz */
    }
    uint32_t cfg1 = igd_rd(DPLL_CFGCR1(id)), cfg2 = igd_rd(DPLL_CFGCR2(id));
    uint64_t dco = 24000ULL * (cfg1 & 0x1FF) + 24000ULL * ((cfg1 >> 9) & 0x7FFF) / 0x8000; /* kHz */
    static const uint32_t pd[] = {1, 2, 3, 0, 7, 0, 0, 0}, kd[] = {5, 2, 3, 1};
    uint32_t p = pd[(cfg2 >> 2) & 7], k = kd[(cfg2 >> 5) & 3], q = (cfg2 & (1u << 7)) ? (cfg2 >> 8) & 0xFF : 1;
    kprintf("igdmode:   DPLL%d: CFGCR1 %#x CFGCR2 %#x -> DCO %lu kHz, Teiler P%u Q%u K%u\n", id, cfg1, cfg2,
            (unsigned long)dco, p, q, k);
    if (!p || !q || !k)
        return 0;
    return (uint32_t)(dco / (p * q * k) / 5); /* AFE-Takt / 5 = Pixeltakt bei HDMI */
}

static void dump_current(int pipe, int *port_out)
{
    uint32_t ht = igd_rd(HTOTAL(pipe)), hb = igd_rd(HBLANK(pipe)), hs = igd_rd(HSYNC(pipe));
    uint32_t vt = igd_rd(VTOTAL(pipe)), vb = igd_rd(VBLANK(pipe)), vs = igd_rd(VSYNC(pipe));
    uint32_t src = igd_rd(PIPESRC(pipe)), ddi = igd_rd(TRANS_DDI_FUNC_CTL(pipe));
    int port = (ddi >> 28) & 7;
    *port_out = (ddi & (1u << 31)) ? port : -1;
    uint32_t hact = (ht & 0xFFFF) + 1, htot = (ht >> 16) + 1, vact = (vt & 0xFFFF) + 1, vtot = (vt >> 16) + 1;
    kprintf("igdmode: Pipe %c jetzt: Quelle %ux%u, sichtbar %ux%u, gesamt %ux%u\n", 'A' + pipe, (src >> 16) + 1,
            (src & 0xFFFF) + 1, hact, vact, htot, vtot);
    kprintf("igdmode:   H: Austastung %u-%u, Sync %u-%u; V: Austastung %u-%u, Sync %u-%u; Sync %s/%s\n",
            (hb & 0xFFFF) + 1, (hb >> 16) + 1, (hs & 0xFFFF) + 1, (hs >> 16) + 1, (vb & 0xFFFF) + 1, (vb >> 16) + 1,
            (vs & 0xFFFF) + 1, (vs >> 16) + 1, (ddi & (1u << 16)) ? "H+" : "H-", (ddi & (1u << 17)) ? "V+" : "V-");
    static const char *mode[] = {"HDMI", "DVI", "DP SST", "DP MST", "FDI", "?", "?", "?"};
    static const char *bpc[] = {"8", "10", "6", "12", "?", "?", "?", "?"};
    kprintf("igdmode:   Transcoder -> Port %s, %s, %s Bit je Farbe (TRANS_DDI_FUNC_CTL %#x)\n", port_name(port),
            mode[(ddi >> 24) & 7], bpc[(ddi >> 20) & 7], ddi);
    for (int i = 0; i < 2; i++) {
        uint32_t ps = igd_rd(PS_CTRL(pipe, i));
        if (ps & (1u << 31))
            kprintf("igdmode:   Skalierer %d an (PS_CTRL %#x)\n", i + 1, ps);
    }

    uint32_t c2 = igd_rd(DPLL_CTRL2);
    int id = (c2 & (1u << (port * 3))) ? (int)((c2 >> (port * 3 + 1)) & 3) : -1;
    kprintf("igdmode:   DPLL_CTRL1 %#x DPLL_CTRL2 %#x DPLL_STATUS %#x LCPLL1 %#x LCPLL2 %#x WRPLL %#x/%#x CDCLK_CTL %#x\n",
            igd_rd(DPLL_CTRL1), c2, igd_rd(DPLL_STATUS), igd_rd(LCPLL1_CTL), igd_rd(LCPLL2_CTL), igd_rd(WRPLL_CTL(0)),
            igd_rd(WRPLL_CTL(1)), igd_rd(CDCLK_CTL));
    if (id < 0 || (c2 & (1u << (port + 15)))) {
        kprintf("igdmode:   Port %s: kein DPLL zugeordnet oder Takt aus\n", port_name(port));
        return;
    }
    int hdmi;
    uint32_t khz = dpll_khz(id, &hdmi);
    if (!hdmi || !khz) {
        kprintf("igdmode:   Port %s haengt an DPLL%d (DP-Modus, Link %u kHz)\n", port_name(port), id, khz);
        return;
    }
    uint64_t hz100 = (uint64_t)khz * 100000 / ((uint64_t)htot * vtot);
    kprintf("igdmode:   Port %s haengt an DPLL%d: Pixeltakt %u.%03u MHz -> %lu.%02lu Hz\n", port_name(port), id,
            khz / 1000, khz % 1000, (unsigned long)(hz100 / 100), (unsigned long)(hz100 % 100));
}

/* ---------- igdtest edid ---------- */

/* EDID des Monitors an port nach edid (256 Byte); Ergebnis: gueltige Bloecke (0 = keine Monitordaten) */
static int read_edid(int port, uint8_t *edid, int verbose)
{
    int want = ddc_pin(port), pin = 0;
    int order[7] = {want, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 7 && !pin; i++) {
        int p = order[i];
        if (p == 0 || (i > 0 && p == want))
            continue;
        int rc = gmbus_read(p, 0x50, 0, edid, 128);
        int ok = rc == 0 && edid[0] == 0 && edid[1] == 0xFF && edid[7] == 0 && edid_checksum(edid);
        if (verbose || ok)
            kprintf("igdmode: DDC Pin-Paar %d%s%s: %s\n", p, p == want ? ", gehoert zu Port " : "",
                    p == want ? port_name(port) : "",
                    ok ? "EDID gelesen" : rc == -1 ? "keine Antwort" : rc == -2 ? "Zeitueberschreitung" : "keine gueltige EDID");
        if (ok)
            pin = p;
    }
    if (!pin) {
        kprintf("igdmode: keine Monitordaten gefunden\n");
        return 0;
    }
    uint32_t ext = edid[126];
    if (!ext)
        return 1;
    if (ext > 1)
        kprintf("igdmode: %u weitere Erweiterungsbloecke (nicht gelesen)\n", ext - 1);
    if (gmbus_read(pin, 0x50, 128, edid + 128, 128) != 0 || !edid_checksum(edid + 128)) {
        kprintf("igdmode: Erweiterungsblock nicht lesbar oder Pruefsumme falsch\n");
        return 1;
    }
    return 2;
}

int igd_edid_test(void)
{
    int pipe = igd_state.scanout_pipe;
    if (!igd_state.gen9 || !igd_regs || pipe < 0) {
        kprintf("igdmode: keine passende Intel-GPU / kein erkannter Framebuffer\n");
        return -1;
    }
    int port;
    dump_current(pipe, &port);

    static uint8_t edid[256];
    int blocks = read_edid(port, edid, 1);
    if (!blocks)
        return -2;
    uint32_t ext = edid[126];
    edid_base(edid);
    if (blocks > 1 && edid[128] == 0x02)
        edid_cta(edid + 128);
    else if (blocks > 1)
        kprintf("igdmode: Erweiterungsblock Typ %#x (nicht ausgewertet)\n", edid[128]);
    kprintf("igdmode: Rohdaten:");
    for (uint32_t i = 0; i < 128u * (ext ? 2 : 1); i++)
        kprintf("%s%02x", i % 32 ? "" : "\nigdmode:   ", edid[i]);
    kprintf("\n");
    return 0;
}

/* ---------- igdtest scale: Skalierer der Pipe ---------- */

/* Testbild w x h in den Speicher bei buf (Zeile stride Byte): Rahmen, Gitter mit 1-Pixel-Linien, Kreis, Farbbalken.
 * Bleibt der Kreis rund und sind die Rahmen an allen vier Kanten zu sehen, stimmt die Skalierung. */
static void test_image(uint8_t *buf, uint32_t stride, uint32_t w, uint32_t h, uint32_t tint)
{
    int cx = (int)w / 2, cy = (int)h / 2, r = (int)h * 2 / 5;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(buf + (uint64_t)y * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = tint;
            if (x % 40 == 0 || y % 40 == 0)
                c = 0x606060;
            int dx = (int)x - cx, dy = (int)y - cy, d2 = dx * dx + dy * dy;
            if (d2 >= (r - 2) * (r - 2) && d2 <= r * r)
                c = 0xFFFF00;
            if (y >= h - h / 8 && y < h - h / 16) { /* Farbbalken */
                static const uint32_t bars[] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF};
                c = bars[x * 7 / w];
            }
            if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4)
                c = 0xFF0000;
            if ((x == 8 || x == w - 9) && y >= 8 && y < h - 8) /* einzelne Pixel-Linien am Rand */
                c = 0xFFFFFF;
            row[x] = c;
        }
    }
    igd_clflush((uint64_t)buf, (uint64_t)stride * h);
}

/* Wartet auf den Anfang eines neuen Bildes: danach bleibt fast ein ganzes Bild Zeit, alle Register zu schreiben, die
 * die Hardware dann gemeinsam beim naechsten Bildwechsel uebernimmt */
static void wait_frame(int p)
{
    uint32_t f0 = igd_rd(PIPE_FRMCOUNT(p));
    uint64_t end = time_us() + 50000;
    while (igd_rd(PIPE_FRMCOUNT(p)) == f0 && time_us() < end)
        ;
}

/* Quelle w x h aus surf (Zeile stride Byte); scale: auf das Fenster (wx, wy, ww, wh) hochrechnen, sonst Skalierer aus */
static int set_source(int p, uint32_t surf, uint32_t w, uint32_t h, uint32_t stride, int scale, uint32_t wx,
                      uint32_t wy, uint32_t ww, uint32_t wh)
{
    wait_frame(p);
    uint64_t f = irq_save();
    igd_wr(PIPESRC(p), (w - 1) << 16 | (h - 1));
    if (scale) {
        igd_wr(PS_CTRL(p, 0), PS_SCALER_EN); /* dynamischer Modus, mittlerer Filter, Pipe (keine einzelne Ebene) */
        igd_wr(PS_VPHASE(p, 0), 0);
        igd_wr(PS_HPHASE(p, 0), 0);
        igd_wr(PS_WIN_POS(p, 0), wx << 16 | wy);
        igd_wr(PS_WIN_SZ(p, 0), ww << 16 | wh);
    } else {
        igd_wr(PS_CTRL(p, 0), 0);
        igd_wr(PS_WIN_POS(p, 0), 0);
        igd_wr(PS_WIN_SZ(p, 0), 0);
    }
    igd_wr(PLANE_STRIDE(p), stride / 64);
    igd_wr(PLANE_SIZE(p), (h - 1) << 16 | (w - 1));
    igd_wr(PLANE_SURF(p), surf); /* uebernimmt die Ebene */
    irq_restore(f);
    uint64_t end = time_us() + 200000;
    while ((igd_rd(PLANE_SURFLIVE(p)) & ~0xFFFu) != surf)
        if (time_us() > end)
            return -1;
    return 0;
}

int igd_scale_test(void)
{
    int pre = igd_preflight("Stufe 4, Teil 2 - Skalierer");
    if (pre)
        return pre;
    int p = igd_state.scanout_pipe;
    if (!igd_flip_ready) {
        kprintf("igdmode: keine Doppelpufferung (zweiter Puffer fehlt), Abbruch\n");
        return -4;
    }
    if (igd_rd(PS_CTRL(p, 0)) & PS_SCALER_EN) {
        kprintf("igdmode: Skalierer 1 ist schon belegt (PS_CTRL %#x), Abbruch\n", igd_rd(PS_CTRL(p, 0)));
        return -5;
    }
    uint32_t src0 = igd_rd(PIPESRC(p)), stride0 = igd_rd(PLANE_STRIDE(p)), size0 = igd_rd(PLANE_SIZE(p));
    uint32_t surf0 = igd_rd(PLANE_SURF(p)), cur0 = igd_rd(CUR_CTL(p));
    uint32_t sw = igd_scr_w, sh = igd_scr_h;

    /* Zwei Testbilder im zweiten Puffer (der gerade nicht angezeigt wird). Lineare Ebenen: Adresse 256-KiB-
     * ausgerichtet, Zeile ein Vielfaches von 64 Byte */
    struct {
        uint32_t w, h, off, wx, wy, ww, wh, tint;
        const char *what;
    } t[2] = {
        {sw / 2, sh / 2, 0, 0, 0, sw, sh, 0x102040, "halbe Aufloesung, doppelt so gross auf dem ganzen Bildschirm"},
        {1920, 1080, 0, (sw - sh * 16 / 9) / 2, 0, sh * 16 / 9, sh, 0x204010, "1920x1080 (16:9), in der Mitte, Raender schwarz"},
    };
    if (sh * 16 / 9 > sw) { /* kein Breitbild: 16:9 in voller Breite, oben und unten Rand */
        t[1].wx = 0;
        t[1].ww = sw;
        t[1].wh = sw * 9 / 16;
        t[1].wy = (sh - t[1].wh) / 2;
    }
    uint32_t stride[2];
    uint64_t off = 0;
    for (int i = 0; i < 2; i++) {
        stride[i] = (t[i].w * 4 + 63) & ~63u;
        t[i].off = (uint32_t)off;
        off = (off + (uint64_t)stride[i] * t[i].h + 0x3FFFF) & ~0x3FFFFULL;
    }
    if (off > (uint64_t)igd_scr_stride * igd_scr_h) {
        kprintf("igdmode: zweiter Puffer zu klein fuer die Testbilder\n");
        return -6;
    }
    for (int i = 0; i < 2; i++)
        test_image(igd_buf_b + t[i].off, stride[i], t[i].w, t[i].h, t[i].tint);

    igd_wr(CUR_CTL(p), 0); /* Mauszeiger waehrend des Tests aus (er wuerde mitskaliert) */
    igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
    uint32_t imr = igd_underrun_begin(p);
    int rc = 0;
    for (int i = 0; i < 2 && rc == 0; i++) {
        kprintf("igdmode: %ux%u -> Fenster %ux%u bei %u,%u: %s\n", t[i].w, t[i].h, t[i].ww, t[i].wh, t[i].wx, t[i].wy,
                t[i].what);
        if (set_source(p, igd_surf_b + t[i].off, t[i].w, t[i].h, stride[i], 1, t[i].wx, t[i].wy, t[i].ww, t[i].wh)) {
            kprintf("igdmode:   Hardware uebernimmt die Einstellung nicht (PLANE_SURFLIVE %#x)\n",
                    igd_rd(PLANE_SURFLIVE(p)));
            rc = -7;
            break;
        }
        kprintf("igdmode:   uebernommen: PIPESRC %#x PS_CTRL %#x PS_WIN_POS %#x PS_WIN_SZ %#x PLANE_SIZE %#x\n",
                igd_rd(PIPESRC(p)), igd_rd(PS_CTRL(p, 0)), igd_rd(PS_WIN_POS(p, 0)), igd_rd(PS_WIN_SZ(p, 0)),
                igd_rd(PLANE_SIZE(p)));
        thread_sleep_ms(4000);
    }

    /* zurueck: Skalierer aus, volle Groesse, Framebuffer der Firmware */
    int back = set_source(p, surf0 & ~0xFFFu, (src0 >> 16) + 1, (src0 & 0xFFFF) + 1, (stride0 & 0x3FF) * 64, 0, 0, 0, 0, 0);
    int under = igd_underrun_end(p, imr);
    igd_wr(CUR_CTL(p), cur0);
    igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
    kprintf("igdmode: zurueck%s: PIPESRC %#x (vorher %#x), PLANE_SIZE %#x (vorher %#x), PS_CTRL %#x\n",
            back ? " NICHT bestaetigt" : "", igd_rd(PIPESRC(p)), src0, igd_rd(PLANE_SIZE(p)), size0, igd_rd(PS_CTRL(p, 0)));
    kprintf("igdmode: waehrend des Tests: %s\n", under ? "FIFO-Unterlauf!" : "kein FIFO-Unterlauf");
    console_repaint();
    if (rc == 0 && back)
        rc = -8;
    kprintf("igdmode: %s\n", rc == 0 ? (under ? "Skalierer funktioniert, aber mit Unterlauf" : "Skalierer funktioniert")
                                     : "Skalierer-Test fehlgeschlagen");
    return rc;
}

/* ---------- igdtest mode: echter Moduswechsel (HDMI) ---------- */

/* DPLL fuer HDMI (Gen9, nach i915 skl_ddi_hdmi_pll_dividers): AFE-Takt = 5 x Pixeltakt. Der DCO schwingt bei
 * 8400/9000/9600 MHz (hoechstens 1 % darueber, 6 % darunter); gesucht ist der Teiler P0*P1*P2 mit der kleinsten
 * Abweichung, gerade Teiler bevorzugt. Ergebnis: 0 = ok, CFGCR1/CFGCR2 fertig zum Schreiben. */
static int skl_hdmi_dpll(uint32_t khz, uint32_t *cfgcr1, uint32_t *cfgcr2)
{
    static const uint32_t even[] = {4, 6, 8, 10, 12, 14, 16, 18, 20, 24, 28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60,
                                    64, 66, 68, 70, 72, 76, 78, 80, 84, 88, 90, 92, 96, 98};
    static const uint32_t odd[] = {3, 5, 7, 9, 15, 21, 35};
    static const uint64_t central[] = {8400000000ULL, 9000000000ULL, 9600000000ULL};
    uint64_t afe = (uint64_t)khz * 1000 * 5;
    uint32_t best_p = 0;
    uint64_t best_dev = ~0ULL, best_central = 0;
    for (int pass = 0; pass < 2 && !best_p; pass++) {
        const uint32_t *list = pass == 0 ? even : odd;
        int n = pass == 0 ? (int)(sizeof(even) / sizeof(even[0])) : (int)(sizeof(odd) / sizeof(odd[0]));
        for (int i = 0; i < n; i++)
            for (int c = 0; c < 3; c++) {
                uint64_t dco = afe * list[i];
                uint64_t diff = dco > central[c] ? dco - central[c] : central[c] - dco;
                uint64_t dev = diff * 10000 / central[c]; /* in 0,01 % */
                if (dco >= central[c] ? dev >= 100 : dev >= 600)
                    continue;
                if (dev < best_dev) {
                    best_dev = dev;
                    best_p = list[i];
                    best_central = central[c];
                }
            }
    }
    if (!best_p)
        return -1;

    uint32_t p = best_p, p0 = 0, p1 = 0, p2 = 0;
    if (p % 2 == 0) {
        uint32_t half = p / 2;
        if (half == 1 || half == 2 || half == 3 || half == 5)
            p0 = 2, p1 = 1, p2 = half;
        else if (half % 2 == 0)
            p0 = 2, p1 = half / 2, p2 = 2;
        else if (half % 3 == 0)
            p0 = 3, p1 = half / 3, p2 = 2;
        else if (half % 7 == 0)
            p0 = 7, p1 = half / 7, p2 = 2;
    } else if (p == 3 || p == 9) {
        p0 = 3, p1 = 1, p2 = p / 3;
    } else if (p == 5) { /* P0 kennt nur 1, 2, 3, 7: die 5 uebernimmt K */
        p0 = 1, p1 = 1, p2 = 5;
    } else if (p == 7) {
        p0 = 7, p1 = 1, p2 = 1;
    } else if (p == 15) {
        p0 = 3, p1 = 1, p2 = 5;
    } else if (p == 21) {
        p0 = 7, p1 = 1, p2 = 3;
    } else if (p == 35) {
        p0 = 7, p1 = 1, p2 = 5;
    }
    if (!p0)
        return -1;
    uint32_t pdiv = p0 == 1 ? 0 : p0 == 2 ? 1 : p0 == 3 ? 2 : 4;
    uint32_t kdiv = p2 == 5 ? 0 : p2 == 2 ? 1 : p2 == 3 ? 2 : 3;
    uint32_t cf = best_central == 9600000000ULL ? 0 : best_central == 9000000000ULL ? 1 : 3;
    uint64_t dco = afe * p0 * p1 * p2;
    uint64_t integer = dco / 24000000ULL;
    uint64_t fraction = (dco / 24 - integer * 1000000ULL) * 0x8000 / 1000000ULL; /* dco / 24 MHz, Nachkommateil */
    *cfgcr1 = (1u << 31) | (uint32_t)(fraction & 0x7FFF) << 9 | (uint32_t)(integer & 0x1FF);
    *cfgcr2 = (p1 != 1 ? (p1 & 0xFF) << 8 | 1u << 7 : 0) | kdiv << 5 | pdiv << 2 | cf; /* Q nur mit Q-Modus */
    return 0;
}

/* Alles, was ein Modus in den Registern ausmacht (Pipe p, Transcoder p, Port, DPLL) */
typedef struct {
    uint32_t htotal, hblank, hsync, vtotal, vblank, vsync, vsyncshift, pipesrc;
    uint32_t ddi_func, cfgcr1, cfgcr2;
    uint32_t plane_stride, plane_size, plane_surf;
    uint32_t data_m, data_n, link_m, link_n; /* nur DisplayPort */
} HwMode;

typedef struct {
    int      pipe, port, dpll;
    uint32_t pipeconf, plane_ctl, cur_ctl, clk_sel, buf_ctl;
} HwFixed;

static uint32_t pll_ctl_reg(int id)
{
    return id == 1 ? LCPLL2_CTL : WRPLL_CTL(id - 2);
}

static void mode_read(const HwFixed *f, HwMode *m)
{
    int p = f->pipe;
    m->htotal = igd_rd(HTOTAL(p));
    m->hblank = igd_rd(HBLANK(p));
    m->hsync = igd_rd(HSYNC(p));
    m->vtotal = igd_rd(VTOTAL(p));
    m->vblank = igd_rd(VBLANK(p));
    m->vsync = igd_rd(VSYNC(p));
    m->vsyncshift = igd_rd(VSYNCSHIFT(p));
    m->pipesrc = igd_rd(PIPESRC(p));
    m->ddi_func = igd_rd(TRANS_DDI_FUNC_CTL(p));
    m->cfgcr1 = f->dpll >= 1 ? igd_rd(DPLL_CFGCR1(f->dpll)) : 0;
    m->cfgcr2 = f->dpll >= 1 ? igd_rd(DPLL_CFGCR2(f->dpll)) : 0;
    m->data_m = igd_rd(PIPE_DATA_M1(p));
    m->data_n = igd_rd(PIPE_DATA_N1(p));
    m->link_m = igd_rd(PIPE_LINK_M1(p));
    m->link_n = igd_rd(PIPE_LINK_N1(p));
    m->plane_stride = igd_rd(PLANE_STRIDE(p));
    m->plane_size = igd_rd(PLANE_SIZE(p));
    m->plane_surf = igd_rd(PLANE_SURF(p)) & ~0xFFFu;
}

/* Modus aus einem Zeitablauf; Ebene: Bild w x h aus surf, Zeile stride Byte */
static void fill_timing(const Timing *t, const HwMode *cur, uint32_t surf, uint32_t stride, HwMode *m)
{
    m->htotal = (t->ht - 1) << 16 | (t->ha - 1);
    m->hblank = (t->ht - 1) << 16 | (t->ha - 1);
    m->hsync = (t->ha + t->hso + t->hsw - 1) << 16 | (t->ha + t->hso - 1);
    m->vtotal = (t->vt - 1) << 16 | (t->va - 1);
    m->vblank = (t->vt - 1) << 16 | (t->va - 1);
    m->vsync = (t->va + t->vso + t->vsw - 1) << 16 | (t->va + t->vso - 1);
    m->vsyncshift = 0;
    m->pipesrc = (t->ha - 1) << 16 | (t->va - 1);
    m->ddi_func = (cur->ddi_func & ~(3u << 16)) | (t->hpos ? 1u << 16 : 0) | (t->vpos ? 1u << 17 : 0);
    m->plane_stride = stride / 64;
    m->plane_size = (t->va - 1) << 16 | (t->ha - 1);
    m->plane_surf = surf;
    m->data_m = cur->data_m;
    m->data_n = cur->data_n;
    m->link_m = cur->link_m;
    m->link_n = cur->link_n;
}

static int mode_from_timing(const Timing *t, const HwMode *cur, uint32_t surf, uint32_t stride, HwMode *m)
{
    if (skl_hdmi_dpll(t->khz, &m->cfgcr1, &m->cfgcr2))
        return -1;
    fill_timing(t, cur, surf, stride, m);
    return 0;
}

static void delay_us(uint64_t us)
{
    uint64_t end = time_us() + us;
    while (time_us() < end)
        ;
}

static int wait_bits(uint32_t reg, uint32_t mask, uint32_t want, int ms)
{
    uint64_t end = time_us() + (uint64_t)ms * 1000;
    while ((igd_rd(reg) & mask) != want)
        if (time_us() > end)
            return -1;
    return 0;
}

/* Anzeige aus, in der Reihenfolge von i915 (hsw_crtc_disable): Ebenen, Pipe, Transcoder, Takt, Port, DPLL */
static void mode_off(const HwFixed *f)
{
    int p = f->pipe;
    igd_wr(PLANE_CTL(p), f->plane_ctl & ~(1u << 31));
    igd_wr(PLANE_SURF(p), igd_rd(PLANE_SURF(p)));
    igd_wr(CUR_CTL(p), 0);
    igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
    wait_frame(p);
    igd_wr(PIPECONF(p), f->pipeconf & ~(1u << 31));
    if (wait_bits(PIPECONF(p), 1u << 30, 0, 100))
        kprintf("igdmode:   Pipe geht nicht aus (PIPECONF %#x)\n", igd_rd(PIPECONF(p)));
    igd_wr(TRANS_DDI_FUNC_CTL(p), igd_rd(TRANS_DDI_FUNC_CTL(p)) & ~((1u << 31) | (7u << 28)));
    igd_wr(TRANS_CLK_SEL(p), 0);
    igd_wr(DDI_BUF_CTL(f->port), f->buf_ctl & ~(1u << 31));
    delay_us(1000);
    igd_wr(DPLL_CTRL2, igd_rd(DPLL_CTRL2) | 1u << (f->port + 15)); /* Takt zum Port aus */
    igd_wr(pll_ctl_reg(f->dpll), igd_rd(pll_ctl_reg(f->dpll)) & ~(1u << 31));
    (void)igd_rd(pll_ctl_reg(f->dpll));
}

/* Anzeige mit Modus m wieder an (hsw_crtc_enable): DPLL, Takt zum Port und Transcoder, Zeitablauf, Transcoder, Ebene,
 * Pipe, Port. 0 = Pipe laeuft */
static int mode_on(const HwFixed *f, const HwMode *m)
{
    int p = f->pipe, rc = 0;
    igd_wr(DPLL_CFGCR1(f->dpll), m->cfgcr1);
    igd_wr(DPLL_CFGCR2(f->dpll), m->cfgcr2);
    (void)igd_rd(DPLL_CFGCR2(f->dpll));
    igd_wr(pll_ctl_reg(f->dpll), igd_rd(pll_ctl_reg(f->dpll)) | 1u << 31);
    if (wait_bits(DPLL_STATUS, 1u << (f->dpll * 8), 1u << (f->dpll * 8), 5)) {
        kprintf("igdmode:   DPLL%d rastet nicht ein (DPLL_STATUS %#x)\n", f->dpll, igd_rd(DPLL_STATUS));
        rc = -1;
    }
    igd_wr(DPLL_CTRL2, igd_rd(DPLL_CTRL2) & ~(1u << (f->port + 15)));
    igd_wr(TRANS_CLK_SEL(p), f->clk_sel);
    igd_wr(HTOTAL(p), m->htotal);
    igd_wr(HBLANK(p), m->hblank);
    igd_wr(HSYNC(p), m->hsync);
    igd_wr(VTOTAL(p), m->vtotal);
    igd_wr(VBLANK(p), m->vblank);
    igd_wr(VSYNC(p), m->vsync);
    igd_wr(VSYNCSHIFT(p), m->vsyncshift);
    igd_wr(PIPESRC(p), m->pipesrc);
    igd_wr(TRANS_DDI_FUNC_CTL(p), m->ddi_func);
    igd_wr(PLANE_STRIDE(p), m->plane_stride);
    igd_wr(PLANE_SIZE(p), m->plane_size);
    igd_wr(PLANE_CTL(p), f->plane_ctl);
    igd_wr(PLANE_SURF(p), m->plane_surf);
    igd_wr(PIPECONF(p), f->pipeconf | 1u << 31);
    if (wait_bits(PIPECONF(p), 1u << 30, 1u << 30, 100)) {
        kprintf("igdmode:   Pipe laeuft nicht an (PIPECONF %#x)\n", igd_rd(PIPECONF(p)));
        rc = -2;
    }
    igd_wr(DDI_BUF_CTL(f->port), f->buf_ctl | 1u << 31);
    delay_us(1000);
    return rc;
}

/* Gemessene Bildrate * 100 (Bilder ueber 2 s) */
static uint32_t measure_hz100(int p)
{
    uint32_t f0 = igd_rd(PIPE_FRMCOUNT(p));
    uint64_t t0 = time_us();
    thread_sleep_ms(2000);
    uint32_t n = igd_rd(PIPE_FRMCOUNT(p)) - f0;
    uint64_t us = time_us() - t0;
    return (uint32_t)((uint64_t)n * 100000000ULL / (us ? us : 1));
}

int igd_mode_test(void)
{
    int pre = igd_preflight("Stufe 4, Teil 3 - Moduswechsel");
    if (pre)
        return pre;
    HwFixed f = {.pipe = igd_state.scanout_pipe};
    int p = f.pipe;
    uint32_t ddi = igd_rd(TRANS_DDI_FUNC_CTL(p)), c2 = igd_rd(DPLL_CTRL2);
    f.port = (int)((ddi >> 28) & 7);
    f.dpll = (c2 & (1u << (f.port * 3))) ? (int)((c2 >> (f.port * 3 + 1)) & 3) : -1;
    if (!(ddi & (1u << 31)) || ((ddi >> 24) & 7) != 0 || f.dpll < 1 ||
        !((igd_rd(DPLL_CTRL1) >> (f.dpll * 6 + 5)) & 1)) {
        kprintf("igdmode: nur fuer HDMI an DPLL1-3 (TRANS_DDI_FUNC_CTL %#x, DPLL_CTRL2 %#x), Abbruch\n", ddi, c2);
        return -4;
    }
    if (!igd_flip_ready) {
        kprintf("igdmode: keine Doppelpufferung (zweiter Puffer fehlt), Abbruch\n");
        return -4;
    }
    if (igd_rd(PS_CTRL(p, 0)) & PS_SCALER_EN) {
        kprintf("igdmode: Skalierer ist an, Abbruch\n");
        return -4;
    }
    f.pipeconf = igd_rd(PIPECONF(p));
    f.plane_ctl = igd_rd(PLANE_CTL(p));
    f.cur_ctl = igd_rd(CUR_CTL(p));
    f.clk_sel = igd_rd(TRANS_CLK_SEL(p));
    f.buf_ctl = igd_rd(DDI_BUF_CTL(f.port));
    HwMode orig;
    mode_read(&f, &orig);
    kprintf("igdmode: vorher: PIPECONF %#x PLANE_CTL %#x TRANS_CLK_SEL %#x DDI_BUF_CTL %#x DPLL%d CFGCR1 %#x CFGCR2 %#x\n",
            f.pipeconf, f.plane_ctl, f.clk_sel, f.buf_ctl, f.dpll, orig.cfgcr1, orig.cfgcr2);

    /* Kandidaten: Zeitablaeufe aus der EDID, die per HDMI gehen, in den zweiten Puffer passen und nicht der jetzige
     * Modus sind (hoechstens drei) */
    static uint8_t edid[256];
    int blocks = read_edid(f.port, edid, 0);
    if (!blocks)
        return -2;
    Timing cand[3];
    int nc = 0;
    uint32_t cur_ht = (orig.htotal >> 16) + 1, cur_ha = (orig.htotal & 0xFFFF) + 1;
    uint32_t cur_vt = (orig.vtotal >> 16) + 1, cur_va = (orig.vtotal & 0xFFFF) + 1;
    for (int b = 0; b < blocks && nc < 3; b++) {
        const uint8_t *e = edid + 128 * b;
        uint32_t first = b == 0 ? 54 : e[2], end = b == 0 ? 126 : 127; /* DTDs: 18 Byte, vor Pruefsumme */
        if (b > 0 && (e[0] != 0x02 || first < 4))
            continue;
        for (uint32_t i = first; i + 18 <= end && nc < 3; i += 18) {
            Timing t;
            if (!dtd_parse(e + i, &t))
                continue;
            int same = t.ha == cur_ha && t.va == cur_va && t.ht == cur_ht && t.vt == cur_vt;
            uint32_t stride = (t.ha * 4 + 63) & ~63u;
            if (t.interlaced || t.khz > GEN9_HDMI_MAX_KHZ || same ||
                (uint64_t)stride * t.va > (uint64_t)igd_scr_stride * igd_scr_h)
                continue;
            cand[nc++] = t;
        }
    }
    if (!nc) {
        kprintf("igdmode: kein anderer Modus des Monitors geht per HDMI (alle ueber 300 MHz oder schon aktiv)\n");
        return -9;
    }

    uint32_t imr = igd_underrun_begin(p);
    int rc = 0, shown = 0;
    static const uint32_t tints[] = {0x102040, 0x204010, 0x401020};
    for (int i = 0; i < nc && rc == 0; i++) {
        Timing *t = &cand[i];
        uint32_t stride = (t->ha * 4 + 63) & ~63u, hz = hz100(t);
        HwMode m;
        if (mode_from_timing(t, &orig, igd_surf_b, stride, &m)) {
            kprintf("igdmode: %ux%u @ %u.%02u Hz: keine DPLL-Einstellung gefunden, uebersprungen\n", t->ha, t->va,
                    hz / 100, hz % 100);
            continue;
        }
        kprintf("igdmode: Modus %ux%u @ %u.%02u Hz, Pixeltakt %u kHz, gesamt %ux%u: DPLL CFGCR1 %#x CFGCR2 %#x\n", t->ha,
                t->va, hz / 100, hz % 100, t->khz, t->ht, t->vt, m.cfgcr1, m.cfgcr2);
        mode_off(&f);
        test_image(igd_buf_b, stride, t->ha, t->va, tints[i % 3]);
        if (mode_on(&f, &m)) {
            rc = -10;
            break;
        }
        shown++;
        uint32_t meas = measure_hz100(p);
        kprintf("igdmode:   laeuft: gemessen %u.%02u Hz, DPLL_STATUS %#x, PIPECONF %#x\n", meas / 100, meas % 100,
                igd_rd(DPLL_STATUS), igd_rd(PIPECONF(p)));
        thread_sleep_ms(6000);
    }

    /* zurueck zum Modus der Firmware (genau die vorher gelesenen Register) */
    mode_off(&f);
    int back = mode_on(&f, &orig);
    uint32_t meas = measure_hz100(p);
    igd_wr(CUR_CTL(p), f.cur_ctl);
    igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
    int under = igd_underrun_end(p, imr);
    kprintf("igdmode: zurueck%s: %ux%u, gemessen %u.%02u Hz, CFGCR1 %#x CFGCR2 %#x, PLANE_SURFLIVE %#x\n",
            back ? " MIT FEHLER" : "", (igd_rd(PIPESRC(p)) >> 16) + 1, (igd_rd(PIPESRC(p)) & 0xFFFF) + 1, meas / 100,
            meas % 100, igd_rd(DPLL_CFGCR1(f.dpll)), igd_rd(DPLL_CFGCR2(f.dpll)), igd_rd(PLANE_SURFLIVE(p)));
    kprintf("igdmode: FIFO-Unterlauf: %s (beim Ein-/Ausschalten der Pipe ist einer moeglich und harmlos)\n",
            under ? "ja" : "nein");
    console_repaint();
    if (rc == 0 && back)
        rc = -11;
    kprintf("igdmode: %s (%d von %d Modi gezeigt)\n", rc == 0 ? "Moduswechsel funktioniert" : "Moduswechsel fehlgeschlagen",
            shown, nc);
    return rc;
}

/* Fuer igd_dp.c: EDID (blocks Bloecke, schon gelesen) auswerten, Zeitablaeufe ueber limit_khz mit note markieren */
void igd_edid_dump(const uint8_t *e, int blocks, uint32_t limit_khz, const char *note)
{
    uint32_t l = dtd_limit;
    const char *n = dtd_note;
    dtd_limit = limit_khz;
    dtd_note = note;
    edid_base(e);
    if (blocks > 1 && edid_checksum(e + 128) && e[128] == 0x02)
        edid_cta(e + 128);
    else if (blocks > 1)
        kprintf("igdmode: Erweiterungsblock Typ %#x (nicht ausgewertet)\n", e[128]);
    dtd_limit = l;
    dtd_note = n;
}

/* ---------- DisplayPort: Moduswechsel auf der bestehenden Verbindung ---------- */

/* Die Firmware hat die Verbindung schon eingemessen (Lanes, Linkrate). Solange die Linkrate fuer den neuen Modus
 * reicht, bleibt sie, wie sie ist: die Linkschicht sendet waehrenddessen Leerlauf-Muster, der Monitor bleibt
 * synchron. Neu gesetzt werden Zeitablauf und die M/N-Werte, aus denen der Transcoder die Fuellsymbole berechnet
 * (Daten: Bits pro Pixel x Pixeltakt : Linktakt x Lanes x 8; Link: Pixeltakt : Linktakt). */

static void compute_mn(uint64_t m, uint64_t n, uint32_t *rm, uint32_t *rn) /* wie i915 compute_m_n */
{
    uint64_t N = 1;
    while (N < n)
        N <<= 1;
    if (N > 0x800000)
        N = 0x800000;
    uint64_t M = m * N / n;
    while (M > 0xFFFFFF) {
        M >>= 1;
        N >>= 1;
    }
    *rm = (uint32_t)M;
    *rn = (uint32_t)N;
}

static void dp_mn(uint32_t px_khz, uint32_t link_khz, uint32_t lanes, HwMode *m)
{
    uint32_t dm, dn;
    compute_mn(24ULL * px_khz, (uint64_t)link_khz * lanes * 8, &dm, &dn);
    m->data_m = (63u << 25) | dm; /* TU-Groesse 64 */
    m->data_n = dn;
    compute_mn(px_khz, link_khz, &m->link_m, &m->link_n);
}

typedef struct {
    HwFixed  f;
    uint32_t link_khz, lanes, tp_ctl;
    uint32_t wm[8], cwm[8];   /* Watermarks der Firmware */
    uint32_t ddb, cddb;       /* Bloecke im Display-Puffer: Ebene, Mauszeiger */
    uint32_t base_khz;        /* Pixeltakt, fuer den die Watermarks der Firmware gelten */
} DpLink;

/* Watermarks fuer einen hoeheren Pixeltakt: die Blockzahl waechst mit dem Datenstrom. Was nicht in den
 * Display-Puffer passt, wird abgeschaltet (die Stufen >0 sind nur fuer Stromsparzustaende). */
static void dp_watermarks(const DpLink *l, uint32_t khz)
{
    int p = l->f.pipe;
    for (int lvl = 0; lvl < 8; lvl++) {
        uint32_t w[2] = {l->wm[lvl], l->cwm[lvl]}, room[2] = {l->ddb, l->cddb};
        for (int k = 0; k < 2; k++) {
            uint32_t v = w[k];
            if (khz > l->base_khz && (v & (1u << 31))) {
                uint32_t blocks = (uint32_t)(((uint64_t)(v & 0x3FF) * khz + l->base_khz - 1) / l->base_khz) + 1;
                uint32_t lines = (uint32_t)(((uint64_t)((v >> 14) & 0x1F) * khz + l->base_khz - 1) / l->base_khz);
                v = (v & ~(0x1Fu << 14)) | (lines > 31 ? 31u : lines) << 14; /* Zeilen werden kuerzer */
                v = blocks >= room[k] ? (lvl == 0 ? (v & ~0x3FFu) | (room[k] - 1) : v & ~(1u << 31))
                                      : (v & ~0x3FFu) | blocks;
            }
            igd_wr(k == 0 ? PLANE_WM(p, lvl) : CUR_WM(p, lvl), v);
        }
    }
}

/* Anzeige auf den Modus m umstellen, die DP-Verbindung bleibt stehen */
/* Ebenen, Pipe und Transcoder aus; die Linkschicht sendet Leerlauf (die Verbindung bleibt stehen) */
static void dp_pipe_off(const DpLink *l)
{
    int p = l->f.pipe;
    igd_wr(PLANE_CTL(p), l->f.plane_ctl & ~(1u << 31));
    igd_wr(PLANE_SURF(p), igd_rd(PLANE_SURF(p)));
    igd_wr(CUR_CTL(p), 0);
    igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
    wait_frame(p);
    igd_wr(DP_TP_CTL(l->f.port), (l->tp_ctl & ~DP_TP_TRAIN_MASK) | DP_TP_TRAIN_IDLE);
    wait_bits(DP_TP_STATUS(l->f.port), DP_TP_IDLE_DONE, DP_TP_IDLE_DONE, 1); /* SST: Bit bleibt hier 0; 1 ms Leerlauf genuegt */
    igd_wr(PIPECONF(p), l->f.pipeconf & ~(1u << 31));
    if (wait_bits(PIPECONF(p), 1u << 30, 0, 100))
        kprintf("igdmode:   Pipe geht nicht aus (PIPECONF %#x)\n", igd_rd(PIPECONF(p)));
    igd_wr(TRANS_DDI_FUNC_CTL(p), igd_rd(TRANS_DDI_FUNC_CTL(p)) & ~((1u << 31) | (7u << 28)));
}

/* Zeitablauf, M/N, Watermarks und Ebene fuer m, dann Transcoder und Pipe an und normale Daten senden */
static int dp_pipe_on(const DpLink *l, const HwMode *m, uint32_t khz)
{
    int p = l->f.pipe, rc = 0;
    igd_wr(HTOTAL(p), m->htotal);
    igd_wr(HBLANK(p), m->hblank);
    igd_wr(HSYNC(p), m->hsync);
    igd_wr(VTOTAL(p), m->vtotal);
    igd_wr(VBLANK(p), m->vblank);
    igd_wr(VSYNC(p), m->vsync);
    igd_wr(VSYNCSHIFT(p), m->vsyncshift);
    igd_wr(PIPESRC(p), m->pipesrc);
    igd_wr(PIPE_DATA_M1(p), m->data_m);
    igd_wr(PIPE_DATA_N1(p), m->data_n);
    igd_wr(PIPE_LINK_M1(p), m->link_m);
    igd_wr(PIPE_LINK_N1(p), m->link_n);
    igd_wr(TRANS_DDI_FUNC_CTL(p), m->ddi_func);
    dp_watermarks(l, khz);
    igd_wr(PLANE_STRIDE(p), m->plane_stride);
    igd_wr(PLANE_SIZE(p), m->plane_size);
    igd_wr(PLANE_CTL(p), l->f.plane_ctl);
    igd_wr(PLANE_SURF(p), m->plane_surf);
    igd_wr(PIPECONF(p), l->f.pipeconf | 1u << 31);
    if (wait_bits(PIPECONF(p), 1u << 30, 1u << 30, 100)) {
        kprintf("igdmode:   Pipe laeuft nicht an (PIPECONF %#x)\n", igd_rd(PIPECONF(p)));
        rc = -2;
    }
    igd_wr(DP_TP_CTL(l->f.port), (l->tp_ctl & ~DP_TP_TRAIN_MASK) | DP_TP_TRAIN_NORMAL);
    return rc;
}

static int dp_switch(const DpLink *l, const HwMode *m, uint32_t khz)
{
    dp_pipe_off(l);
    return dp_pipe_on(l, m, khz);
}

/* Laufende DP-Verbindung der Pipe einlesen: Port, Lanes, Linktakt, Watermarks; orig = Modus jetzt. 0 = ok */
static int dp_link_init(DpLink *l, HwMode *orig, int verbose)
{
    int p = igd_state.scanout_pipe;
    *l = (DpLink){.f = {.pipe = p}};
    uint32_t ddi = igd_rd(TRANS_DDI_FUNC_CTL(p)), c1 = igd_rd(DPLL_CTRL1), c2 = igd_rd(DPLL_CTRL2);
    l->f.port = (int)((ddi >> 28) & 7);
    l->f.dpll = (c2 & (1u << (l->f.port * 3))) ? (int)((c2 >> (l->f.port * 3 + 1)) & 3) : -1;
    if (!(ddi & (1u << 31)) || ((ddi >> 24) & 7) != 2 || l->f.dpll < 0) {
        if (verbose)
            kprintf("igdmode: die Anzeige laeuft nicht per DisplayPort (SST), TRANS_DDI_FUNC_CTL %#x\n", ddi);
        return -1;
    }
    static const uint32_t rate[] = {540000, 270000, 162000, 324000, 216000, 432000}; /* Linktakt (Symbole) in kHz */
    uint32_t code = (c1 >> (l->f.dpll * 6 + 1)) & 7;
    l->link_khz = code < 6 ? rate[code] : 0;
    l->lanes = ((ddi >> 1) & 7) + 1;
    l->tp_ctl = igd_rd(DP_TP_CTL(l->f.port));
    l->f.pipeconf = igd_rd(PIPECONF(p));
    l->f.plane_ctl = igd_rd(PLANE_CTL(p));
    l->f.cur_ctl = igd_rd(CUR_CTL(p));
    uint32_t pb = igd_rd(PLANE_BUF_CFG(p)), cb = igd_rd(CUR_BUF_CFG(p));
    l->ddb = ((pb >> 16) & 0x3FF) - (pb & 0x3FF) + 1;
    l->cddb = ((cb >> 16) & 0x3FF) - (cb & 0x3FF) + 1;
    for (int lvl = 0; lvl < 8; lvl++) {
        l->wm[lvl] = igd_rd(PLANE_WM(p, lvl));
        l->cwm[lvl] = igd_rd(CUR_WM(p, lvl));
    }
    mode_read(&l->f, orig);
    if (!l->link_khz || !orig->link_n) {
        if (verbose)
            kprintf("igdmode: Linkrate unbekannt (DPLL_CTRL1 %#x)\n", c1);
        return -1;
    }
    l->base_khz = (uint32_t)((uint64_t)l->link_khz * orig->link_m / orig->link_n);
    return 0;
}

/* Hoechster Pixeltakt: Verbindung (24 Bit je Pixel), CDCLK und der Pipe-Takt (= Linktakt, ein Pixel je Takt) */
static uint32_t dp_limit(const DpLink *l)
{
    uint32_t cd = (igd_rd(CDCLK_CTL) & 0x7FF), cdclk = (cd + 2) * 500;
    uint32_t limit = (uint32_t)((uint64_t)l->link_khz * 8 * l->lanes / 24);
    if (cdclk < limit)
        limit = cdclk;
    if (l->link_khz < limit)
        limit = l->link_khz;
    return limit;
}

/* Zustand der Verbindung beim Monitor (DPCD 0x202-0x205): je Lane Takt/Entzerrung/Symbole, Lanes ausgerichtet */
static int dp_link_ok(const DpLink *l, char *out, int size)
{
    uint8_t st[4] = {0};
    if (igd_dpcd_read(l->f.port, 0x202, st, 4) != 4) {
        ksnprintf(out, (size_t)size, "Status nicht lesbar");
        return 0;
    }
    int ok = (st[2] & 1) != 0;
    for (uint32_t lane = 0; lane < l->lanes; lane++)
        ok &= ((st[lane / 2] >> (4 * (lane % 2))) & 7) == 7;
    ksnprintf(out, (size_t)size, "Lanes %02x %02x, ausgerichtet %u, Sink %02x -> %s", st[0], st[1], st[2] & 1, st[3],
              ok ? "Verbindung steht" : "VERBINDUNG GESTOERT");
    return ok;
}

int igd_dpmode_test(void)
{
    int pre = igd_preflight("DisplayPort Teil 2 - Moduswechsel");
    if (pre)
        return pre;
    DpLink l;
    HwMode orig;
    int p = igd_state.scanout_pipe;
    if (dp_link_init(&l, &orig, 1))
        return -4;
    if (!igd_flip_ready || (igd_rd(PS_CTRL(p, 0)) & PS_SCALER_EN)) {
        kprintf("igdmode: kein zweiter Puffer oder Skalierer an, Abbruch\n");
        return -4;
    }
    char st[96];
    dp_link_ok(&l, st, sizeof(st));
    kprintf("igdmode: DP an Port %s: %u Lanes, Linktakt %u MHz, DPLL%d; Pixeltakt jetzt %u kHz; %s\n",
            port_name(l.f.port), l.lanes, l.link_khz / 1000, l.f.dpll, l.base_khz, st);
    HwMode chk = orig;
    dp_mn(l.base_khz, l.link_khz, l.lanes, &chk);
    kprintf("igdmode:   M/N der Firmware: Daten %#x/%#x Link %#x/%#x; nachgerechnet: Daten %#x/%#x Link %#x/%#x\n",
            orig.data_m, orig.data_n, orig.link_m, orig.link_n, chk.data_m, chk.data_n, chk.link_m, chk.link_n);
    kprintf("igdmode:   DP_TP_CTL %#x, DDB Ebene %u / Zeiger %u Bloecke, WM0 %#x\n", l.tp_ctl, l.ddb, l.cddb, l.wm[0]);

    uint32_t limit = dp_limit(&l);
    static uint8_t edid[256];
    int blocks = igd_dp_edid(l.f.port, edid);
    if (!blocks) {
        kprintf("igdmode: EDID ueber AUX nicht lesbar\n");
        return -2;
    }
    Timing cand[4];
    int nc = 0;
    uint32_t cur_ha = (orig.htotal & 0xFFFF) + 1, cur_va = (orig.vtotal & 0xFFFF) + 1;
    uint32_t cur_ht = (orig.htotal >> 16) + 1, cur_vt = (orig.vtotal >> 16) + 1;
    for (int b = 0; b < blocks && nc < 4; b++) {
        const uint8_t *e = edid + 128 * b;
        uint32_t first = b == 0 ? 54 : e[2], end = b == 0 ? 126 : 127;
        if (b > 0 && (e[0] != 0x02 || first < 4))
            continue;
        for (uint32_t i = first; i + 18 <= end && nc < 4; i += 18) {
            Timing t;
            if (!dtd_parse(e + i, &t) || t.interlaced || t.khz > limit)
                continue;
            uint32_t dk = t.khz > l.base_khz ? t.khz - l.base_khz : l.base_khz - t.khz;
            int dup = t.ha == cur_ha && t.va == cur_va && t.ht == cur_ht && t.vt == cur_vt && dk < 1000;
            for (int k = 0; k < nc; k++)
                dup |= cand[k].ha == t.ha && cand[k].va == t.va && cand[k].khz == t.khz;
            uint32_t stride = (t.ha * 4 + 63) & ~63u;
            if (!dup && (uint64_t)stride * t.va <= (uint64_t)igd_scr_stride * igd_scr_h)
                cand[nc++] = t;
        }
    }
    for (int i = 1; i < nc; i++) /* hoechste Bildrate zuerst: das ist der interessante Fall */
        for (int k = i; k > 0 && hz100(&cand[k]) > hz100(&cand[k - 1]); k--) {
            Timing x = cand[k];
            cand[k] = cand[k - 1];
            cand[k - 1] = x;
        }
    if (!nc) {
        kprintf("igdmode: kein anderer Modus passt (Grenze %u kHz)\n", limit);
        return -9;
    }

    int rc = 0, shown = 0, bad = 0;
    static const uint32_t tints[] = {0x102040, 0x204010, 0x401020, 0x303010};
    for (int i = 0; i < nc && rc == 0; i++) {
        Timing *t = &cand[i];
        uint32_t stride = (t->ha * 4 + 63) & ~63u, hz = hz100(t);
        HwMode m;
        fill_timing(t, &orig, igd_surf_b, stride, &m);
        dp_mn(t->khz, l.link_khz, l.lanes, &m);
        kprintf("igdmode: Modus %ux%u @ %u.%02u Hz, Pixeltakt %u kHz, gesamt %ux%u: M/N Daten %#x/%#x Link %#x/%#x\n",
                t->ha, t->va, hz / 100, hz % 100, t->khz, t->ht, t->vt, m.data_m, m.data_n, m.link_m, m.link_n);
        test_image(igd_buf_b, stride, t->ha, t->va, tints[i % 4]);
        uint32_t imr = igd_underrun_begin(p);
        if (dp_switch(&l, &m, t->khz)) {
            igd_underrun_end(p, imr);
            rc = -10;
            break;
        }
        shown++;
        thread_sleep_ms(300);
        int ok = dp_link_ok(&l, st, sizeof(st));
        uint32_t meas = measure_hz100(p);
        int under = igd_underrun_end(p, imr);
        bad += !ok || under;
        kprintf("igdmode:   laeuft: gemessen %u.%02u Hz, %s, %s, WM0 %#x\n", meas / 100, meas % 100, st,
                under ? "FIFO-UNTERLAUF" : "kein Unterlauf", igd_rd(PLANE_WM(p, 0)));
        thread_sleep_ms(6000);
    }

    /* zurueck: Zeitablauf, M/N und Watermarks der Firmware */
    int back = dp_switch(&l, &orig, l.base_khz);
    igd_wr(CUR_CTL(p), l.f.cur_ctl);
    igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
    thread_sleep_ms(300);
    int ok = dp_link_ok(&l, st, sizeof(st));
    uint32_t meas = measure_hz100(p);
    kprintf("igdmode: zurueck%s: %ux%u, gemessen %u.%02u Hz, %s, PLANE_SURFLIVE %#x\n", back ? " MIT FEHLER" : "",
            (igd_rd(PIPESRC(p)) >> 16) + 1, (igd_rd(PIPESRC(p)) & 0xFFFF) + 1, meas / 100, meas % 100, st,
            igd_rd(PLANE_SURFLIVE(p)));
    console_repaint();
    if (rc == 0 && (back || !ok))
        rc = -11;
    kprintf("igdmode: %s (%d von %d Modi gezeigt%s)\n", rc == 0 ? "DP-Moduswechsel funktioniert" : "DP-Moduswechsel fehlgeschlagen",
            shown, nc, bad ? ", mit Stoerungen" : "");
    return rc;
}

/* ---------- DisplayPort: Link-Training ---------- */

/* Die Verbindung einmessen (DP 1.2, wie i915 intel_dp_link_training.c):
 * 1. Taktrueckgewinnung: Muster TPS1, der Monitor rastet je Lane auf den Takt ein (CR_DONE). Er darf dabei einen
 *    hoeheren Spannungshub / mehr Vorverzerrung verlangen (DPCD 0x206/0x207); die Grafik stellt sie am Port ein
 *    (DDI_BUF_CTL waehlt einen Eintrag der Pegel-Tabelle DDI_BUF_TRANS) und meldet sie dem Monitor (0x103-0x106).
 * 2. Entzerrung: Muster TPS2 bzw. TPS3, bis jede Lane entzerrt und symbolsynchron ist und die Lanes zueinander
 *    ausgerichtet sind.
 * Danach sendet die Linkschicht Leerlauf, bis die Pipe laeuft. */

#define DDI_BUF_TRANS_SEL(n)  ((uint32_t)(n) << 24)
#define DDI_BUF_IS_IDLE       (1u << 7)
#define DDI_BUF_TRANS_LO(port, i) (0x64E00 + 0x60u * (uint32_t)(port) + 8u * (uint32_t)(i))
#define DDI_BUF_TRANS_HI(port, i) (DDI_BUF_TRANS_LO(port, i) + 4)
#define DP_TP_CTL_ENABLE      (1u << 31)
#define DP_TP_CTL_ENH_FRAME   (1u << 18)
#define DP_TP_TRAIN_PAT1      (0u << 8)
#define DP_TP_TRAIN_PAT2      (1u << 8)
#define DP_TP_TRAIN_PAT3      (4u << 8)

/* Gen9 (9 Eintraege): Spannungshub vs 0-2, Vorverzerrung pe; Index in DDI_BUF_TRANS (i915 index_to_dp_signal_levels) */
static int dp_level_index(int vs, int pe)
{
    static const int8_t idx[3][4] = {{0, 1, 2, 3}, {4, 5, 6, -1}, {7, 8, -1, -1}};
    return vs >= 0 && vs < 3 && pe >= 0 && pe < 4 ? idx[vs][pe] : -1;
}

static int dp_max_pe(int vs)
{
    return vs == 0 ? 3 : vs == 1 ? 2 : 1;
}

typedef struct {
    int      port, lanes, tps3;
    uint32_t buf_ctl;       /* DDI_BUF_CTL ohne Pegel-Auswahl */
    uint32_t cr_us, eq_us;  /* Wartezeit vor dem Lesen des Status */
    int      vs, pe;        /* aktuelle Pegel (alle Lanes gleich) */
} DpTrain;

static int dp_set_levels(DpTrain *t, uint8_t pattern)
{
    int idx = dp_level_index(t->vs, t->pe);
    if (idx < 0)
        return -1;
    igd_wr(DDI_BUF_CTL(t->port), t->buf_ctl | DDI_BUF_TRANS_SEL(idx) | (1u << 31));
    uint8_t d[5] = {pattern};
    for (int i = 0; i < t->lanes; i++)
        d[1 + i] = (uint8_t)(t->vs | (t->vs == 2 ? 4 : 0) | t->pe << 3 | (t->pe == dp_max_pe(t->vs) ? 0x20 : 0));
    return igd_dpcd_write(t->port, 0x102, d, 1 + t->lanes); /* Muster und Pegel in einem Rutsch */
}

/* Hoechste vom Monitor verlangte Pegel ueber alle Lanes */
static void dp_requested(const DpTrain *t, const uint8_t *st, int *vs, int *pe)
{
    *vs = *pe = 0;
    for (int lane = 0; lane < t->lanes; lane++) {
        uint8_t a = (uint8_t)(st[4 + lane / 2] >> (4 * (lane % 2)));
        if ((a & 3) > *vs)
            *vs = a & 3;
        if (((a >> 2) & 3) > *pe)
            *pe = (a >> 2) & 3;
    }
    if (*vs > 2)
        *vs = 2;
    if (*pe > dp_max_pe(*vs))
        *pe = dp_max_pe(*vs);
}

static int lanes_have(const DpTrain *t, const uint8_t *st, uint8_t bits)
{
    for (int lane = 0; lane < t->lanes; lane++)
        if (((st[lane / 2] >> (4 * (lane % 2))) & bits) != bits)
            return 0;
    return 1;
}

/* Linkschicht und Port einschalten und einmessen (rate_code: DPCD-Linkrate, z.B. 0x14). 0 = Verbindung steht */
static int dp_train(DpTrain *t, uint8_t rate_code, uint32_t tp_ctl_base)
{
    uint8_t st[6];
    uint8_t d0 = 1;
    igd_dpcd_write(t->port, 0x600, &d0, 1); /* Monitor-Empfaenger an (D0) */
    delay_us(1000);
    uint8_t bw[2] = {rate_code, (uint8_t)(t->lanes | 0x80)}; /* Linkrate, Lanes + Enhanced Framing */
    if (igd_dpcd_write(t->port, 0x100, bw, 2)) {
        kprintf("igdmode:   DPCD 0x100 nicht schreibbar\n");
        return -1;
    }
    igd_wr(DP_TP_CTL(t->port), tp_ctl_base | DP_TP_CTL_ENABLE | DP_TP_CTL_ENH_FRAME | DP_TP_TRAIN_PAT1);
    t->vs = t->pe = 0;
    igd_wr(DDI_BUF_CTL(t->port), t->buf_ctl | DDI_BUF_TRANS_SEL(0) | (1u << 31));
    delay_us(600);

    /* 1. Taktrueckgewinnung */
    int cr = 0, same = 0, last_vs = -1, loops = 0;
    if (dp_set_levels(t, 0x21) != 0) /* TPS1, Verwuerfelung aus */
        return -1;
    for (loops = 0; loops < 20 && !cr; loops++) {
        delay_us(t->cr_us);
        if (igd_dpcd_read(t->port, 0x202, st, 6) != 6)
            return -1;
        if (lanes_have(t, st, 1)) {
            cr = 1;
            break;
        }
        int vs, pe;
        dp_requested(t, st, &vs, &pe);
        same = vs == last_vs ? same + 1 : 0;
        last_vs = vs;
        if (same >= 5)
            break;
        t->vs = vs;
        t->pe = pe;
        dp_set_levels(t, 0x21);
    }
    kprintf("igdmode:   Taktrueckgewinnung %s nach %d Runde(n), Pegel vs%d pe%d, Status %02x %02x\n",
            cr ? "ok" : "FEHLGESCHLAGEN", loops + 1, t->vs, t->pe, st[0], st[1]);
    if (!cr)
        return -2;

    /* 2. Entzerrung */
    uint8_t pat = t->tps3 ? 3 : 2;
    igd_wr(DP_TP_CTL(t->port), tp_ctl_base | DP_TP_CTL_ENABLE | DP_TP_CTL_ENH_FRAME |
                                   (t->tps3 ? DP_TP_TRAIN_PAT3 : DP_TP_TRAIN_PAT2));
    dp_set_levels(t, (uint8_t)(pat | 0x20));
    int eq = 0;
    for (loops = 0; loops < 6 && !eq; loops++) {
        delay_us(t->eq_us);
        if (igd_dpcd_read(t->port, 0x202, st, 6) != 6)
            return -1;
        if (!lanes_have(t, st, 1))
            break; /* Takt verloren */
        if (lanes_have(t, st, 7) && (st[2] & 1)) {
            eq = 1;
            break;
        }
        dp_requested(t, st, &t->vs, &t->pe);
        dp_set_levels(t, (uint8_t)(pat | 0x20));
    }
    kprintf("igdmode:   Entzerrung (TPS%u) %s nach %d Runde(n), Pegel vs%d pe%d, Status %02x %02x %02x\n", pat,
            eq ? "ok" : "FEHLGESCHLAGEN", loops + 1, t->vs, t->pe, st[0], st[1], st[2]);

    uint8_t off = 0;
    igd_dpcd_write(t->port, 0x102, &off, 1); /* Training beim Monitor beenden */
    igd_wr(DP_TP_CTL(t->port), tp_ctl_base | DP_TP_CTL_ENABLE | DP_TP_CTL_ENH_FRAME | DP_TP_TRAIN_IDLE);
    return eq ? 0 : -3;
}

/* Port und Linkschicht aus (i915 intel_disable_ddi_buf) */
static void dp_link_off(int port, uint32_t tp_ctl_base)
{
    igd_wr(DDI_BUF_CTL(port), igd_rd(DDI_BUF_CTL(port)) & ~(1u << 31));
    igd_wr(DP_TP_CTL(port), tp_ctl_base | DP_TP_TRAIN_PAT1);
    wait_bits(DDI_BUF_CTL(port), DDI_BUF_IS_IDLE, DDI_BUF_IS_IDLE, 2);
}

int igd_dptrain_test(void)
{
    int pre = igd_preflight("DisplayPort Teil 3 - Link-Training");
    if (pre)
        return pre;
    DpLink l;
    HwMode cur;
    if (dp_link_init(&l, &cur, 1))
        return -4;
    int p = l.f.pipe, port = l.f.port;
    uint8_t cap[16];
    if (igd_dpcd_read(port, 0x000, cap, 16) != 16) {
        kprintf("igdmode: DPCD nicht lesbar\n");
        return -4;
    }
    uint8_t tps3 = cap[2] & 0x40, interval = cap[0xE] & 0x7F;
    uint32_t buf0 = igd_rd(DDI_BUF_CTL(port));
    kprintf("igdmode: Port %s: DDI_BUF_CTL %#x (Pegel-Eintrag %u), DP_TP_CTL %#x, Training-Abstand %u, TPS3 %s\n",
            port_name(port), buf0, (buf0 >> 24) & 0xF, l.tp_ctl, interval, tps3 ? "ja" : "nein");
    for (int i = 0; i < 10; i++)
        kprintf("igdmode:   DDI_BUF_TRANS[%d] = %#x %#x\n", i, igd_rd(DDI_BUF_TRANS_LO(port, i)),
                igd_rd(DDI_BUF_TRANS_HI(port, i)));

    uint32_t khz_now = l.base_khz; /* Pixeltakt jetzt; die gelesenen Watermarks gelten dafuer */
    DpTrain t = {.port = port, .lanes = (int)l.lanes, .tps3 = tps3 != 0,
                 .buf_ctl = buf0 & ~(DDI_BUF_TRANS_SEL(0xF) | (1u << 31)),
                 .cr_us = 100, .eq_us = interval ? interval * 4000u : 400};
    uint32_t tp_base = l.tp_ctl & ~(DP_TP_CTL_ENABLE | DP_TP_CTL_ENH_FRAME | DP_TP_TRAIN_MASK);
    uint8_t rate = (uint8_t)(l.link_khz / 27000); /* DPCD-Linkrate in 0,27 GBit/s: 540 MHz -> 0x14 */

    kprintf("igdmode: Verbindung aus (der Monitor zeigt eventuell kurz 'kein Signal') ...\n");
    uint32_t imr = igd_underrun_begin(p);
    dp_pipe_off(&l);
    dp_link_off(port, tp_base);
    thread_sleep_ms(1000);
    char st[96];
    dp_link_ok(&l, st, sizeof(st));
    kprintf("igdmode: Verbindung ist aus: %s\n", st);

    uint64_t t0 = time_us();
    int rc = dp_train(&t, rate, tp_base);
    uint64_t us = time_us() - t0;
    if (rc) {
        kprintf("igdmode: Training fehlgeschlagen (%d), zweiter Versuch ...\n", rc);
        dp_link_off(port, tp_base);
        thread_sleep_ms(100);
        rc = dp_train(&t, rate, tp_base);
    }
    kprintf("igdmode: Link-Training %s in %lu us\n", rc == 0 ? "erfolgreich" : "FEHLGESCHLAGEN", (unsigned long)us);
    int on = dp_pipe_on(&l, &cur, khz_now);
    igd_wr(CUR_CTL(p), l.f.cur_ctl);
    igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
    thread_sleep_ms(300);
    int ok = dp_link_ok(&l, st, sizeof(st));
    uint32_t meas = measure_hz100(p);
    int under = igd_underrun_end(p, imr);
    kprintf("igdmode: Bild wieder an%s: gemessen %u.%02u Hz, %s, DDI_BUF_CTL %#x, %s\n", on ? " MIT FEHLER" : "",
            meas / 100, meas % 100, st, igd_rd(DDI_BUF_CTL(port)), under ? "FIFO-Unterlauf (beim Einschalten moeglich)" : "kein Unterlauf");
    console_repaint();
    kprintf("igdmode: %s\n", rc == 0 && ok && !on ? "Link-Training funktioniert" : "Link-Training fehlgeschlagen");
    return rc == 0 && ok && !on ? 0 : -12;
}

/* ---------- Fest eingebaut: Modi des Monitors im Betrieb umschalten ---------- */

#define MAX_MODES 16
static Timing   modes[MAX_MODES];     /* nach Flaeche, dann Bildrate absteigend */
static int      nmodes, cur_idx = -1, boot_idx = -1;
static HwFixed  live;                 /* Pipe, Port, DPLL und die festen Registerwerte */
static DpLink   dpl;                  /* bei DisplayPort: die Verbindung der Firmware */
static int      is_dp;
static HwMode   boot_hw, cur_hw;      /* Modus der Firmware / gerade gesetzter */

static int same_timing(const Timing *a, const Timing *b)
{
    uint32_t d = a->khz > b->khz ? a->khz - b->khz : b->khz - a->khz;
    return a->ha == b->ha && a->va == b->va && a->ht == b->ht && a->vt == b->vt && d < 1000;
}

static void add_mode(const Timing *t)
{
    for (int i = 0; i < nmodes; i++)
        if (same_timing(&modes[i], t))
            return;
    if (nmodes >= MAX_MODES)
        return;
    int i = nmodes++;
    uint64_t area = (uint64_t)t->ha * t->va;
    while (i > 0) {
        uint64_t a = (uint64_t)modes[i - 1].ha * modes[i - 1].va;
        if (a > area || (a == area && hz100(&modes[i - 1]) >= hz100(t)))
            break;
        modes[i] = modes[i - 1];
        i--;
    }
    modes[i] = *t;
}

static int find_mode(const Timing *t)
{
    for (int i = 0; i < nmodes; i++)
        if (same_timing(&modes[i], t))
            return i;
    return -1;
}

/* Liest die Modi: den der Firmware und die Detailed Timings aus der EDID, die per HDMI gehen und in den Framebuffer
 * der Firmware passen (er wird weiterbenutzt, mit gleicher Zeilenlaenge) */
static void modes_init(void)
{
    int p = igd_state.scanout_pipe;
    uint32_t ddi = igd_rd(TRANS_DDI_FUNC_CTL(p)), c2 = igd_rd(DPLL_CTRL2);
    live.pipe = p;
    live.port = (int)((ddi >> 28) & 7);
    live.dpll = (c2 & (1u << (live.port * 3))) ? (int)((c2 >> (live.port * 3 + 1)) & 3) : -1;
    uint32_t limit = GEN9_HDMI_MAX_KHZ, khz;
    if (igd_rd(PS_CTRL(p, 0)) & PS_SCALER_EN) {
        kprintf("igd: Skalierer ist an: Modus bleibt der der Firmware\n");
        return;
    }
    if ((ddi & (1u << 31)) && ((ddi >> 24) & 7) == 2) { /* DisplayPort: auf der Verbindung der Firmware */
        if (dp_link_init(&dpl, &boot_hw, 0)) {
            kprintf("igd: DisplayPort-Verbindung nicht lesbar: Modus bleibt der der Firmware\n");
            return;
        }
        is_dp = 1;
        limit = dp_limit(&dpl);
        khz = dpl.base_khz;
    } else if ((ddi & (1u << 31)) && ((ddi >> 24) & 7) == 0 && live.dpll >= 1 &&
               ((igd_rd(DPLL_CTRL1) >> (live.dpll * 6 + 5)) & 1)) { /* HDMI */
        live.pipeconf = igd_rd(PIPECONF(p));
        live.plane_ctl = igd_rd(PLANE_CTL(p));
        live.clk_sel = igd_rd(TRANS_CLK_SEL(p));
        live.buf_ctl = igd_rd(DDI_BUF_CTL(live.port));
        mode_read(&live, &boot_hw);
        khz = cfg_khz(boot_hw.cfgcr1, boot_hw.cfgcr2);
    } else {
        kprintf("igd: Moduswechsel nur per HDMI (DPLL1-3) oder DisplayPort: Modus bleibt der der Firmware\n");
        return;
    }
    cur_hw = boot_hw;

    Timing b = {.khz = khz};
    b.ha = (boot_hw.htotal & 0xFFFF) + 1;
    b.ht = (boot_hw.htotal >> 16) + 1;
    b.hso = (boot_hw.hsync & 0xFFFF) + 1 - b.ha;
    b.hsw = (boot_hw.hsync >> 16) - (boot_hw.hsync & 0xFFFF);
    b.va = (boot_hw.vtotal & 0xFFFF) + 1;
    b.vt = (boot_hw.vtotal >> 16) + 1;
    b.vso = (boot_hw.vsync & 0xFFFF) + 1 - b.va;
    b.vsw = (boot_hw.vsync >> 16) - (boot_hw.vsync & 0xFFFF);
    b.hpos = (boot_hw.ddi_func >> 16) & 1;
    b.vpos = (boot_hw.ddi_func >> 17) & 1;
    if (!b.khz || b.ha != igd_scr_w || b.va != igd_scr_h) {
        kprintf("igd: Modus der Firmware nicht lesbar: Modus bleibt, wie er ist\n");
        return;
    }
    add_mode(&b);

    static uint8_t edid[256];
    int blocks = is_dp ? igd_dp_edid(live.port, edid) : read_edid(live.port, edid, 0);
    for (int blk = 0; blk < blocks; blk++) {
        const uint8_t *e = edid + 128 * blk;
        uint32_t first = blk == 0 ? 54 : e[2], end = blk == 0 ? 126 : 127;
        if (blk > 0 && (e[0] != 0x02 || first < 4))
            continue;
        for (uint32_t i = first; i + 18 <= end; i += 18) {
            Timing t;
            if (dtd_parse(e + i, &t) && !t.interlaced && t.khz <= limit && t.ha <= igd_scr_w &&
                t.va <= igd_scr_h && t.ha >= 640 && t.va >= 400)
                add_mode(&t);
        }
    }
    boot_idx = cur_idx = find_mode(&b);
    char line[256];
    int n = 0;
    for (int i = 0; i < nmodes; i++) {
        uint32_t hz = hz100(&modes[i]);
        n += ksnprintf(line + n, sizeof(line) - (size_t)n, "%s%ux%u@%u%s", i ? ", " : "", modes[i].ha, modes[i].va,
                       (hz + 50) / 100, i == boot_idx ? " (jetzt)" : "");
        if (n >= (int)sizeof(line) - 24)
            break;
    }
    kprintf("igd: Modi zum Umschalten (%s): %s\n", is_dp ? "DisplayPort" : "HDMI", line);
}

int igd_mode_count(void)
{
    return nmodes;
}

int igd_mode_info(int i, uint32_t *w, uint32_t *h, uint32_t *hz, int *current)
{
    if (i < 0 || i >= nmodes)
        return -1;
    *w = modes[i].ha;
    *h = modes[i].va;
    *hz = hz100(&modes[i]);
    *current = i == cur_idx;
    return 0;
}

int igd_mode_set(uint32_t w, uint32_t h, uint32_t hz)
{
    if (!nmodes)
        return IGD_MODE_NODRIVER;
    int idx = -1;
    if (!w) {
        idx = boot_idx;
    } else {
        for (int i = 0; i < nmodes; i++) {
            if (modes[i].ha != w || modes[i].va != h)
                continue;
            uint32_t mh = hz100(&modes[i]), d = mh > hz ? mh - hz : hz - mh;
            if (hz && d > 100) /* auf 1 Hz genau */
                continue;
            if (idx < 0 || (!hz && mh > hz100(&modes[idx])))
                idx = i;
        }
    }
    if (idx < 0)
        return IGD_MODE_NOMODE;
    if (idx == cur_idx)
        return 0;
    if (console_gfx_active())
        return IGD_MODE_BUSY;
    igd_gfx_end(); /* die Konsole zeigt den Framebuffer der Firmware (A) */

    HwMode m;
    if (idx == boot_idx) {
        m = boot_hw;
    } else if (is_dp) {
        fill_timing(&modes[idx], &boot_hw, igd_surf_a, igd_scr_stride, &m);
        dp_mn(modes[idx].khz, dpl.link_khz, dpl.lanes, &m);
    } else if (mode_from_timing(&modes[idx], &boot_hw, igd_surf_a, igd_scr_stride, &m)) {
        return IGD_MODE_FAILED;
    }
    int p = live.pipe;
    uint32_t cur = igd_rd(CUR_CTL(p));
    if (is_dp) { /* die Verbindung bleibt, nur Zeitablauf, M/N und Watermarks wechseln */
        dpl.f.plane_ctl = igd_rd(PLANE_CTL(p));
        if (dp_switch(&dpl, &m, idx == boot_idx ? dpl.base_khz : modes[idx].khz)) {
            kprintf("igd: Modus %ux%u laesst sich nicht setzen, zurueck zum vorigen\n", modes[idx].ha, modes[idx].va);
            dp_switch(&dpl, &cur_hw, cur_idx == boot_idx ? dpl.base_khz : modes[cur_idx].khz);
            igd_wr(CUR_CTL(p), cur);
            igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
            return IGD_MODE_FAILED;
        }
        char st[96];
        if (!dp_link_ok(&dpl, st, sizeof(st)))
            kprintf("igd: DisplayPort nach dem Wechsel: %s\n", st);
    } else { /* HDMI: Pipe und Port aus, DPLL neu, wieder an */
        live.plane_ctl = igd_rd(PLANE_CTL(p));
        mode_off(&live);
        if (mode_on(&live, &m)) {
            kprintf("igd: Modus %ux%u laesst sich nicht setzen, zurueck zum vorigen\n", modes[idx].ha, modes[idx].va);
            mode_off(&live);
            if (mode_on(&live, &cur_hw)) {
                mode_off(&live);
                mode_on(&live, &boot_hw);
                cur_hw = boot_hw;
                cur_idx = boot_idx;
                igd_scr_w = modes[boot_idx].ha;
                igd_scr_h = modes[boot_idx].va;
                console_resize(igd_scr_w, igd_scr_h);
            }
            igd_wr(CUR_CTL(p), cur);
            igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
            return IGD_MODE_FAILED;
        }
    }
    igd_wr(CUR_CTL(p), cur);
    igd_wr(CUR_BASE(p), igd_rd(CUR_BASE(p)));
    cur_hw = m;
    cur_idx = idx;
    igd_scr_w = modes[idx].ha;
    igd_scr_h = modes[idx].va;
    console_resize(igd_scr_w, igd_scr_h);
    uint32_t hzv = hz100(&modes[idx]);
    kprintf("igd: Modus %ux%u @ %u.%02u Hz gesetzt (Pixeltakt %u kHz)\n", modes[idx].ha, modes[idx].va, hzv / 100,
            hzv % 100, modes[idx].khz);
    return 0;
}

/* Beim Start (aus display_init): Modi einsammeln, dann ggf. "igdmode=BxH[@Hz]" aus der Kommandozeile setzen */
void igd_modes_boot(void)
{
    modes_init();
    const char *want = cmdline_get("igdmode");
    if (!want || !nmodes)
        return;
    uint32_t w = 0, h = 0, hz = 0;
    const char *s = want;
    while (*s >= '0' && *s <= '9')
        w = w * 10 + (uint32_t)(*s++ - '0');
    if (*s == 'x')
        s++;
    while (*s >= '0' && *s <= '9')
        h = h * 10 + (uint32_t)(*s++ - '0');
    if (*s == '@') {
        s++;
        while (*s >= '0' && *s <= '9')
            hz = hz * 10 + (uint32_t)(*s++ - '0');
        hz *= 100;
    }
    int rc = w && h ? igd_mode_set(w, h, hz) : IGD_MODE_NOMODE;
    if (rc)
        kprintf("igd: igdmode=%s: %s (%d Modi bekannt)\n", want,
                rc == IGD_MODE_NOMODE ? "diesen Modus bietet der Monitor nicht an (resolution zeigt die Liste)"
                                      : "Umschalten fehlgeschlagen", nmodes);
}
