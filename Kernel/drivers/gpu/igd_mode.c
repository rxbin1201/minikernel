/* Intel-Grafik Gen9, Stufe 4: Bildschirmmodi. Teil 1 (igdtest edid): nur lesen. Teil 2 (igdtest scale): kleinere
 * Aufloesung, die der Skalierer der Pipe auf den Bildschirm hochrechnet; Takt und Zeitablauf bleiben.
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

static int edid_checksum(const uint8_t *b)
{
    uint8_t s = 0;
    for (int i = 0; i < 128; i++)
        s = (uint8_t)(s + b[i]);
    return s == 0;
}

/* Detailed Timing Descriptor (18 Byte): Ergebnis Pixeltakt in kHz, 0 = kein Zeitablauf */
static uint32_t dtd_print(const uint8_t *d, const char *what)
{
    uint32_t clk = (uint32_t)(d[0] | d[1] << 8) * 10;
    if (!clk)
        return 0;
    uint32_t ha = d[2] | (uint32_t)(d[4] & 0xF0) << 4, hb = d[3] | (uint32_t)(d[4] & 0x0F) << 8;
    uint32_t va = d[5] | (uint32_t)(d[7] & 0xF0) << 4, vb = d[6] | (uint32_t)(d[7] & 0x0F) << 8;
    uint32_t hso = d[8] | (uint32_t)(d[11] & 0xC0) << 2, hsw = d[9] | (uint32_t)(d[11] & 0x30) << 4;
    uint32_t vso = (d[10] >> 4) | (uint32_t)(d[11] & 0x0C) << 2, vsw = (d[10] & 0xF) | (uint32_t)(d[11] & 0x03) << 4;
    uint32_t ht = ha + hb, vt = va + vb;
    uint64_t mhz100 = (uint64_t)clk * 100000 / ((uint64_t)ht * vt); /* Bildrate * 100 */
    int interlaced = d[17] >> 7, digital_sep = ((d[17] >> 3) & 3) == 3;
    kprintf("igdmode:   %s %ux%u%s @ %lu.%02lu Hz, Pixeltakt %u.%02u MHz, gesamt %ux%u, HSync %u+%u, VSync %u+%u%s%s%s\n",
            what, ha, va, interlaced ? "i" : "", (unsigned long)(mhz100 / 100), (unsigned long)(mhz100 % 100),
            clk / 1000, clk % 1000 / 10, ht, vt, hso, hsw, vso, vsw,
            digital_sep ? ((d[17] & 2) ? ", H+" : ", H-") : "", digital_sep ? ((d[17] & 4) ? " V+" : " V-") : "",
            clk > GEN9_HDMI_MAX_KHZ ? "  [ueber 300 MHz: per HDMI auf Gen9 nicht moeglich]" : "");
    return clk;
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
    int want = ddc_pin(port), pin = 0;
    int order[7] = {want, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 7 && !pin; i++) {
        int p = order[i];
        if (p == 0 || (i > 0 && p == want))
            continue;
        int rc = gmbus_read(p, 0x50, 0, edid, 128);
        int ok = rc == 0 && edid[0] == 0 && edid[1] == 0xFF && edid[7] == 0;
        kprintf("igdmode: DDC Pin-Paar %d%s%s: %s\n", p, p == want ? ", gehoert zu Port " : "",
                p == want ? port_name(port) : "",
                ok ? (edid_checksum(edid) ? "EDID gelesen" : "EDID gelesen (Pruefsumme falsch!)")
                   : rc == -1 ? "keine Antwort" : rc == -2 ? "Zeitueberschreitung" : "keine EDID");
        if (ok)
            pin = p;
    }
    if (!pin) {
        kprintf("igdmode: keine Monitordaten gefunden\n");
        return -2;
    }
    edid_base(edid);
    uint32_t ext = edid[126];
    if (ext) {
        if (gmbus_read(pin, 0x50, 128, edid + 128, 128) != 0) {
            kprintf("igdmode: Erweiterungsblock nicht lesbar\n");
        } else if (!edid_checksum(edid + 128)) {
            kprintf("igdmode: Erweiterungsblock: Pruefsumme falsch\n");
        } else if (edid[128] == 0x02) {
            edid_cta(edid + 128);
        } else {
            kprintf("igdmode: Erweiterungsblock Typ %#x (nicht ausgewertet)\n", edid[128]);
        }
        if (ext > 1)
            kprintf("igdmode: %u weitere Bloecke (nicht gelesen)\n", ext - 1);
    }
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
