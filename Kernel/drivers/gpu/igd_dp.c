/* Intel-Grafik Gen9, DisplayPort. Teil 1 (igdtest dp): nur lesen.
 *
 * Bei DisplayPort spricht die Grafik mit dem Monitor ueber den AUX-Kanal (1 MBit/s, halbduplex, bis 16 Byte je
 * Nachricht). Darueber liest sie das DPCD (die Faehigkeiten des Empfaengers: DP-Version, Lanes, Linkrate) und, als
 * I2C ueber AUX, die EDID. Jeder Anschluss (DDI) hat einen eigenen AUX-Kanal; ohne DP-Geraet laeuft die Nachricht
 * in eine Zeitueberschreitung. Nichts hier veraendert die Anzeige.
 * Registerangaben nach Intels "Programmer's Reference Manual" (Skylake/Kaby Lake) und dem Linux-i915. */

#include "drivers/gpu/igd_internal.h"
#include "arch/x86_64/apic.h"
#include "lib/kprintf.h"
#include "lib/string.h"

#define DP_AUX_CH_CTL(port)     (0x64010 + 0x100u * (uint32_t)(port))
#define DP_AUX_CH_DATA(port, i) (DP_AUX_CH_CTL(port) + 4 + 4u * (uint32_t)(i)) /* 5 Worte, Byte 0 in Bits 31:24 */
#define AUX_SEND_BUSY           (1u << 31)
#define AUX_DONE                (1u << 30)
#define AUX_TIME_OUT_ERROR      (1u << 28)
#define AUX_TIME_OUT_MAX        (3u << 26)                /* 1600 us */
#define AUX_RECEIVE_ERROR       (1u << 25)
#define AUX_SIZE_SHIFT          20
#define AUX_FW_SYNC(c)          (((uint32_t)(c) - 1) << 5)
#define AUX_SYNC(c)             ((uint32_t)(c) - 1)
#define DDI_BUF_CTL(port)       (0x64000 + 0x100u * (uint32_t)(port))
#define SDEISR                  0xC4000                   /* PCH: Hotplug-Zustand (Bit 21/22/23 = Port B/C/D) */
#define SHOTPLUG_CTL            0xC4030
#define PWR_WELL_CTL_DRIVER     0x45404
#define PWR_WELL_CTL_BIOS       0x45400
#define CDCLK_CTL               0x46000

#define AUX_NATIVE_WRITE 0x8
#define AUX_NATIVE_READ  0x9
#define AUX_I2C_WRITE    0x0
#define AUX_I2C_READ     0x1
#define AUX_I2C_MOT      0x4                                /* Middle Of Transaction: kein Stop danach */
#define SKL_MAX_RATE     0x14                               /* Gen9: hoechstens HBR2 (5,4 GBit/s je Lane) */

static void delay_us(uint64_t us)
{
    uint64_t end = time_us() + us;
    while (time_us() < end)
        ;
}

static const char *port_name(int port)
{
    static const char *n[] = {"A", "B", "C", "D", "E"};
    return port >= 0 && port < 5 ? n[port] : "?";
}

/* Eine AUX-Nachricht: tx senden, Antwort nach rx (rx[0] = Antwortcode). Ergebnis: empfangene Bytes,
 * -1 = keine Antwort (Zeitueberschreitung), -2 = Empfangsfehler, -3 = Kanal haengt */
static int aux_once(int port, const uint8_t *tx, int txn, uint8_t *rx, int rxmax)
{
    uint32_t ctl = DP_AUX_CH_CTL(port);
    uint64_t end = time_us() + 10000;
    while (igd_rd(ctl) & AUX_SEND_BUSY)
        if (time_us() > end)
            return -3;
    for (int i = 0; i < txn; i += 4) {
        uint32_t v = 0;
        for (int k = 0; k < 4; k++)
            v |= (uint32_t)(i + k < txn ? tx[i + k] : 0) << (24 - 8 * k);
        igd_wr(DP_AUX_CH_DATA(port, i / 4), v);
    }
    igd_wr(ctl, AUX_SEND_BUSY | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_TIME_OUT_MAX | AUX_RECEIVE_ERROR |
                    (uint32_t)txn << AUX_SIZE_SHIFT | AUX_FW_SYNC(32) | AUX_SYNC(32));
    end = time_us() + 10000;
    uint32_t st;
    while ((st = igd_rd(ctl)) & AUX_SEND_BUSY)
        if (time_us() > end)
            return -3;
    igd_wr(ctl, st | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_RECEIVE_ERROR); /* Statusbits loeschen */
    if (st & AUX_TIME_OUT_ERROR)
        return -1;
    if (st & AUX_RECEIVE_ERROR)
        return -2;
    int n = (int)((st >> AUX_SIZE_SHIFT) & 0x1F);
    if (n == 0 || n > 20)
        return -2;
    for (int i = 0; i < n && i < rxmax; i += 4) {
        uint32_t v = igd_rd(DP_AUX_CH_DATA(port, i / 4));
        for (int k = 0; k < 4 && i + k < n && i + k < rxmax; k++)
            rx[i + k] = (uint8_t)(v >> (24 - 8 * k));
    }
    return n < rxmax ? n : rxmax;
}

/* Mit Wiederholungen: bei Empfangsfehlern und DEFER (der Empfaenger ist noch nicht so weit). Ergebnis wie aux_once,
 * -4 = NACK. i2c: Antwortcode auch auf I2C-NACK/DEFER pruefen */
static int aux_xfer(int port, const uint8_t *tx, int txn, uint8_t *rx, int rxmax, int i2c)
{
    int timeouts = 0;
    for (int tries = 0; tries < 32; tries++) {
        int n = aux_once(port, tx, txn, rx, rxmax);
        if (n == -1) {
            if (++timeouts >= 3)
                return -1;
            continue;
        }
        if (n == -3)
            return -3;
        if (n < 0) {
            delay_us(400);
            continue;
        }
        uint8_t nat = rx[0] & 0x30, ic = rx[0] & 0xC0;
        if (nat == 0x10 || (i2c && ic == 0x40))
            return -4;
        if (nat == 0x20 || (i2c && ic == 0x80)) {
            delay_us(500);
            continue;
        }
        return n;
    }
    return -2;
}

/* DPCD lesen (len <= 16); Ergebnis: gelesene Bytes oder < 0 */
static int dpcd_read(int port, uint32_t addr, uint8_t *buf, int len)
{
    uint8_t tx[4] = {(uint8_t)(AUX_NATIVE_READ << 4 | ((addr >> 16) & 0xF)), (uint8_t)(addr >> 8), (uint8_t)addr,
                     (uint8_t)(len - 1)};
    uint8_t rx[20];
    int n = aux_xfer(port, tx, 4, rx, 1 + len, 0);
    if (n < 1)
        return n < 0 ? n : -2;
    memcpy(buf, rx + 1, (size_t)(n - 1));
    return n - 1;
}

/* EDID ueber I2C-ueber-AUX (Adresse 0x50): Offset 0 schreiben, dann in Stuecken zu 16 Byte lesen, zum Schluss Stop.
 * Ergebnis: gelesene Bloecke (0 = keine EDID) */
static int edid_read_aux(int port, uint8_t *edid)
{
    uint8_t rx[20];
    uint8_t wr[5] = {(AUX_I2C_WRITE | AUX_I2C_MOT) << 4, 0, 0x50, 0, 0}; /* 1 Byte: Offset 0 */
    if (aux_xfer(port, wr, 5, rx, 20, 1) < 1)
        return 0;
    int want = 128, got = 0;
    while (got < want) {
        int len = want - got > 16 ? 16 : want - got;
        uint8_t rd[4] = {(AUX_I2C_READ | AUX_I2C_MOT) << 4, 0, 0x50, (uint8_t)(len - 1)};
        int n = aux_xfer(port, rd, 4, rx, 1 + len, 1);
        if (n < 2)
            break;
        memcpy(edid + got, rx + 1, (size_t)(n - 1));
        got += n - 1;
        if (got == 128 && edid[126] && edid[0] == 0 && edid[1] == 0xFF)
            want = 256; /* ein Erweiterungsblock: gleich weiterlesen */
    }
    uint8_t stop[3] = {AUX_I2C_READ << 4, 0, 0x50}; /* nur Adresse, ohne MOT: Stop */
    aux_xfer(port, stop, 3, rx, 20, 1);
    if (got < 128 || edid[0] != 0 || edid[1] != 0xFF || edid[7] != 0)
        return 0;
    return got >= 256 ? 2 : 1;
}

static const char *rate_name(uint8_t r)
{
    switch (r) {
    case 0x06: return "1,62 GBit/s (RBR)";
    case 0x0A: return "2,7 GBit/s (HBR)";
    case 0x14: return "5,4 GBit/s (HBR2)";
    case 0x1E: return "8,1 GBit/s (HBR3)";
    default: return "?";
    }
}

/* Ein Anschluss: antwortet dort ein DP-Empfaenger? Dann DPCD und EDID ausgeben. 1 = DP-Geraet gefunden */
static int probe_port(int port)
{
    uint8_t cap[16];
    int n = dpcd_read(port, 0x000, cap, 16);
    if (n < 0) {
        kprintf("igddp: Port %s: %s\n", port_name(port),
                n == -1 ? "kein DisplayPort-Geraet (AUX: keine Antwort)" : n == -3 ? "AUX-Kanal haengt" :
                n == -4 ? "AUX: NACK" : "AUX: Empfangsfehler");
        return 0;
    }
    if (n < 16) {
        kprintf("igddp: Port %s: DPCD nur %d Byte gelesen\n", port_name(port), n);
        return 0;
    }
    uint8_t rate = cap[1], lanes = cap[2] & 0x1F;
    if (cap[0xE] & 0x80) { /* DPCD 1.4: die echten Faehigkeiten stehen ab 0x2200 */
        uint8_t ext[16];
        if (dpcd_read(port, 0x2200, ext, 16) == 16) {
            rate = ext[1];
            lanes = ext[2] & 0x1F;
            cap[0] = ext[0];
        }
    }
    uint8_t sink = 0, power = 0;
    dpcd_read(port, 0x200, &sink, 1);
    dpcd_read(port, 0x600, &power, 1);
    kprintf("igddp: Port %s: DisplayPort %u.%u, bis %u Lane(s) mit %s%s%s, Downspread %s\n", port_name(port),
            cap[0] >> 4, cap[0] & 0xF, lanes, rate_name(rate), (cap[2] & 0x80) ? ", Enhanced Framing" : "",
            (cap[2] & 0x40) ? ", TPS3" : "", (cap[3] & 1) ? "ja" : "nein");
    kprintf("igddp:   Empfaenger: %u Sink(s), Zustand %s, Training-Abstand %s, DPCD 0x000: %02x %02x %02x %02x %02x %02x %02x %02x"
            " %02x %02x %02x %02x %02x %02x %02x %02x\n", sink & 0x3F, (power & 7) == 1 ? "an" : (power & 7) == 2 ? "Standby" : "?",
            (cap[0xE] & 0x7F) ? "lang" : "100/400 us", cap[0], cap[1], cap[2], cap[3], cap[4], cap[5], cap[6], cap[7],
            cap[8], cap[9], cap[10], cap[11], cap[12], cap[13], cap[14], cap[15]);

    /* Wie viel Pixeltakt traegt die Verbindung (24 Bit je Pixel, 8b/10b: je Lane 8 Nutzbits pro Symboltakt)? */
    uint8_t use = rate > SKL_MAX_RATE ? SKL_MAX_RATE : rate;
    uint32_t link_khz = (uint32_t)use * 27000;                       /* Symboltakt je Lane */
    uint32_t max_px = (uint32_t)((uint64_t)link_khz * 8 * lanes / 24); /* Pixeltakt */
    uint32_t cd = igd_rd(CDCLK_CTL) & 0x7FF, cdclk = (cd + 2) * 500;  /* kHz; der Pixeltakt darf nicht hoeher sein */
    uint32_t limit = max_px < cdclk ? max_px : cdclk;
    kprintf("igddp:   mit dieser Grafik: %u Lane(s) x %s -> Pixeltakt bis %u MHz (Verbindung %u MHz, CDCLK %u MHz)\n",
            lanes, rate_name(use), limit / 1000, max_px / 1000, cdclk / 1000);

    static uint8_t edid[256];
    int blocks = edid_read_aux(port, edid);
    if (!blocks) {
        kprintf("igddp:   EDID ueber AUX nicht lesbar\n");
        return 1;
    }
    kprintf("igddp:   EDID ueber AUX gelesen (%d Block/Bloecke%s)\n", blocks,
            edid[126] > 1 ? ", weitere nicht gelesen" : "");
    igd_edid_dump(edid, blocks, limit, "  [zu schnell fuer diese DP-Verbindung]");
    return 1;
}

int igd_dpcd_read(int port, uint32_t addr, uint8_t *buf, int len)
{
    return dpcd_read(port, addr, buf, len);
}

int igd_dp_edid(int port, uint8_t *edid)
{
    return edid_read_aux(port, edid);
}

int igd_dp_test(void)
{
    if (!igd_state.gen9 || !igd_regs) {
        kprintf("igddp: keine passende Intel-GPU\n");
        return -1;
    }
    kprintf("igddp: Hotplug: SDEISR %#x (B %s, C %s, D %s), SHOTPLUG_CTL %#x\n", igd_rd(SDEISR),
            (igd_rd(SDEISR) >> 21) & 1 ? "belegt" : "frei", (igd_rd(SDEISR) >> 22) & 1 ? "belegt" : "frei",
            (igd_rd(SDEISR) >> 23) & 1 ? "belegt" : "frei", igd_rd(SHOTPLUG_CTL));
    kprintf("igddp: Leistungsbereiche BIOS %#x Treiber %#x; DDI_BUF_CTL B %#x C %#x D %#x\n", igd_rd(PWR_WELL_CTL_BIOS),
            igd_rd(PWR_WELL_CTL_DRIVER), igd_rd(DDI_BUF_CTL(1)), igd_rd(DDI_BUF_CTL(2)), igd_rd(DDI_BUF_CTL(3)));
    int found = 0;
    for (int port = 1; port <= 3; port++)
        found += probe_port(port);
    kprintf("igddp: %d DisplayPort-Geraet(e) gefunden\n", found);
    return found ? 0 : -2;
}
