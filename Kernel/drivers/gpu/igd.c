/* Intel-Grafik Gen9 (siehe igd.h), Stufe 1: erkennen und den Zustand der Anzeige auslesen. Nur lesende Zugriffe. */

#include "drivers/gpu/igd.h"
#include "arch/x86_64/apic.h"
#include "drivers/pci.h"
#include "lib/kprintf.h"
#include "mm/paging.h"

/* ---------- Register (Offsets in BAR0) ---------- */

#define PIPE_OFF(pipe)        (0x1000u * (uint32_t)(pipe))
#define PIPECONF(p)           (0x70008 + PIPE_OFF(p))   /* Bit 31: an, Bit 30: laeuft */
#define PIPESRC(p)            (0x6001C + PIPE_OFF(p))   /* (Breite-1) << 16 | (Hoehe-1) */
#define HTOTAL(t)             (0x60000 + PIPE_OFF(t))   /* (gesamt-1) << 16 | (sichtbar-1) */
#define VTOTAL(t)             (0x6000C + PIPE_OFF(t))
#define PIPE_FRMCOUNT(p)      (0x70040 + PIPE_OFF(p))   /* zaehlt jedes Bild (VBlank) */
#define PLANE_CTL(p)          (0x70180 + PIPE_OFF(p))   /* primaere Ebene (Plane 1) */
#define PLANE_STRIDE(p)       (0x70188 + PIPE_OFF(p))   /* linear: Einheiten zu 64 Byte */
#define PLANE_POS(p)          (0x7018C + PIPE_OFF(p))
#define PLANE_SIZE(p)         (0x70190 + PIPE_OFF(p))
#define PLANE_SURF(p)         (0x7019C + PIPE_OFF(p))   /* Adresse im Grafik-Adressraum (GTT), 4-KiB-ausgerichtet */
#define PLANE_OFFSET(p)       (0x701A4 + PIPE_OFF(p))
#define CUR_CTL(p)            (0x70080 + PIPE_OFF(p))   /* Hardware-Mauszeiger */
#define CUR_BASE(p)           (0x70084 + PIPE_OFF(p))
#define CUR_POS(p)            (0x70088 + PIPE_OFF(p))
#define TRANS_DDI_FUNC_CTL(t) (0x60400 + PIPE_OFF(t))   /* welcher Port, welcher Modus */
#define DDI_BUF_CTL(port)     (0x64000 + 0x100u * (uint32_t)(port))
#define PWR_WELL_CTL_BIOS     0x45400
#define PWR_WELL_CTL_DRIVER   0x45404
#define PW1_STATE             (1u << 28)
#define PW2_STATE             (1u << 30)

static IgdInfo info = {.scanout_pipe = -1};
static volatile uint8_t *regs;

static uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(regs + off);
}

const IgdInfo *igd_info(void)
{
    return &info;
}

/* ---------- Erkennen ---------- */

typedef struct {
    uint16_t    id;
    const char *name;
} Known;

/* Die haeufigsten Gen9-Modelle; andere Gen9-IDs erkennt gen9_family trotzdem */
static const Known known[] = {
    {0x3E92, "UHD Graphics 630 (Coffee Lake-S GT2)"},
    {0x3E91, "UHD Graphics 630 (Coffee Lake-S GT2)"},
    {0x3E98, "UHD Graphics 630 (Coffee Lake-S GT2, 9. Generation)"},
    {0x3E9B, "UHD Graphics 630 (Coffee Lake-H GT2)"},
    {0x3E90, "UHD Graphics 610 (Coffee Lake-S GT1)"},
    {0x3E93, "UHD Graphics 610 (Coffee Lake-S GT1)"},
    {0x5912, "HD Graphics 630 (Kaby Lake GT2)"},
    {0x5916, "HD Graphics 620 (Kaby Lake GT2)"},
    {0x3EA0, "UHD Graphics 620 (Whiskey Lake GT2)"},
    {0x9BC5, "UHD Graphics 630 (Comet Lake GT2)"},
    {0x9BC8, "UHD Graphics 630 (Comet Lake GT2)"},
    {0x1912, "HD Graphics 530 (Skylake GT2)"},
    {0x1916, "HD Graphics 520 (Skylake GT2)"},
};

static int gen9_family(uint16_t id)
{
    uint16_t hi = id & 0xFF00;
    return hi == 0x1900 || hi == 0x5900 || hi == 0x3E00 || hi == 0x8700 || hi == 0x9B00;
}

/* ---------- Auslesen ---------- */

static const char *plane_format(uint32_t ctl)
{
    switch ((ctl >> 24) & 0xF) {
    case 0x0: return "YUV 4:2:2";
    case 0x2: return "RGB 10:10:10";
    case 0x4: return "RGB 8:8:8 (32 Bit)";
    case 0x6: return "RGB 16 Bit Float";
    case 0x8: return "YUV 4:2:0";
    case 0xE: return "RGB 5:6:5";
    default:  return "?";
    }
}

static const char *plane_tiling(uint32_t ctl)
{
    switch ((ctl >> 10) & 7) {
    case 0:  return "linear";
    case 1:  return "X-Tiling";
    case 4:  return "Y-Tiling";
    case 5:  return "Yf-Tiling";
    default: return "?";
    }
}

static const char *ddi_mode(uint32_t ctl)
{
    static const char *modes[] = {"HDMI", "DVI", "DisplayPort", "DisplayPort MST", "FDI", "?", "?", "?"};
    return modes[(ctl >> 24) & 7];
}

static void dump_pipe(int p, const BootInfo *bi)
{
    uint32_t conf = rd(PIPECONF(p));
    if (!(conf & (1u << 31))) {
        kprintf("igd:   Pipe %c: aus\n", 'A' + p);
        return;
    }
    uint32_t src = rd(PIPESRC(p)), ht = rd(HTOTAL(p)), vt = rd(VTOTAL(p));
    uint32_t w = ((src >> 16) & 0x1FFF) + 1, h = (src & 0x1FFF) + 1;
    uint32_t f0 = rd(PIPE_FRMCOUNT(p));
    uint64_t t0 = time_ms();
    WAIT_UNTIL(time_ms() - t0 >= 250, 300);
    uint32_t frames = rd(PIPE_FRMCOUNT(p)) - f0;
    kprintf("igd:   Pipe %c: an (%s), Quelle %ux%u, Timing %ux%u sichtbar von %ux%u, %u Bilder in 250 ms (~%u Hz)\n",
            'A' + p, (conf & (1u << 30)) ? "laeuft" : "startet", w, h, (ht & 0x1FFF) + 1, (vt & 0x1FFF) + 1,
            ((ht >> 16) & 0x1FFF) + 1, ((vt >> 16) & 0x1FFF) + 1, frames, frames * 4);

    uint32_t ctl = rd(PLANE_CTL(p)), stride = rd(PLANE_STRIDE(p)), size = rd(PLANE_SIZE(p));
    uint32_t surf = rd(PLANE_SURF(p)), off = rd(PLANE_OFFSET(p)), pos = rd(PLANE_POS(p));
    kprintf("igd:     Ebene 1: %s, %s, %s, Zeile %u Byte, Groesse %ux%u, Position %u,%u, Surface %#x, Offset %u,%u\n",
            (ctl & (1u << 31)) ? "an" : "aus", plane_format(ctl), plane_tiling(ctl), (stride & 0x3FF) * 64,
            (size & 0xFFF) + 1, ((size >> 16) & 0xFFF) + 1, pos & 0xFFF, (pos >> 16) & 0xFFF, surf & ~0xFFFu,
            off & 0x1FFF, (off >> 16) & 0xFFF);

    uint32_t cur = rd(CUR_CTL(p));
    kprintf("igd:     Mauszeiger-Ebene: %s (CUR_CTL %#x, CUR_BASE %#x, CUR_POS %#x)\n", (cur & 0x27) ? "an" : "aus",
            cur, rd(CUR_BASE(p)), rd(CUR_POS(p)));

    uint32_t ddi = rd(TRANS_DDI_FUNC_CTL(p));
    if (ddi & (1u << 31))
        kprintf("igd:     Transcoder %c -> Port %c (%s), TRANS_DDI_FUNC_CTL %#x\n", 'A' + p, 'A' + ((ddi >> 28) & 7),
                ddi_mode(ddi), ddi);

    /* Zeigt diese Ebene den GOP-Framebuffer? Die CPU sieht Grafikspeicher ueber das Aperture-Fenster (BAR2): die
     * Surface-Adresse ist dort der Abstand zum Anfang. */
    if ((ctl & (1u << 31)) && info.aperture && bi->fb.base == info.aperture + (surf & ~0xFFFu)) {
        info.scanout_pipe = p;
        info.scanout_surf = surf & ~0xFFFu;
        kprintf("igd:     -> zeigt den GOP-Framebuffer (%#lx = Aperture + Surface)\n", (unsigned long)bi->fb.base);
    }
}

void igd_init(const BootInfo *bi)
{
    PciDevice d;
    unsigned i = 0;
    while (pci_find_class(0x03, 0x00, -1, i, &d) == 0 && d.vendor != 0x8086) /* evtl. steckt noch eine Grafikkarte */
        i++;
    if (d.vendor != 0x8086 || pci_find_class(0x03, 0x00, -1, i, &d) != 0)
        return;
    info.present = 1;
    info.device = d.device;
    info.name = "Intel-Grafik (unbekanntes Modell)";
    for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++)
        if (known[k].id == d.device)
            info.name = known[k].name;
    info.gen9 = gen9_family(d.device);
    info.mmio = pci_bar_mem(&d, 0);
    info.aperture = pci_bar_mem(&d, 2);
    info.mmio_size = 16u << 20; /* Gen9: 16 MiB, Register in den ersten 2 MiB, globale GTT ab 8 MiB */
    kprintf("igd: %02x:%02x.%u 8086:%04x %s, Register @ %#lx, Aperture @ %#lx, PCI-Rev %u\n", d.bus, d.dev, d.fn,
            d.device, info.name, (unsigned long)info.mmio, (unsigned long)info.aperture, d.revision);
    if (!info.gen9) {
        kprintf("igd: keine Gen9-GPU, Register werden nicht gelesen\n");
        return;
    }
    if (!info.mmio || paging_map_mmio(info.mmio, 2u << 20) != 0) {
        kprintf("igd: Register nicht einblendbar\n");
        return;
    }
    regs = (volatile uint8_t *)info.mmio;
    pci_set_driver(&d, "igd");

    /* Stolen Memory: von der Firmware fuer die Grafik reservierter Hauptspeicher (PCI 0x5C: Basis, 0x50: Groesse) */
    uint32_t bdsm = pci_read32(&d, 0x5C) & ~0xFFFFFu, gmch = pci_read32(&d, 0x50);
    uint32_t gms = (gmch >> 8) & 0xFF;
    kprintf("igd: Stolen Memory @ %#x, %u MiB (GMCH_CTRL %#x)\n", bdsm, gms < 0xF0 ? gms * 32 : 4 * (gms - 0xF0 + 1), gmch);

    uint32_t pw = rd(PWR_WELL_CTL_BIOS) | rd(PWR_WELL_CTL_DRIVER);
    kprintf("igd: Leistungsbereiche: PW1 %s, PW2 %s (BIOS %#x, Treiber %#x)\n", (pw & PW1_STATE) ? "an" : "aus",
            (pw & PW2_STATE) ? "an" : "aus", rd(PWR_WELL_CTL_BIOS), rd(PWR_WELL_CTL_DRIVER));
    for (int port = 0; port < 5; port++) {
        uint32_t buf = rd(DDI_BUF_CTL(port));
        if (buf & (1u << 31))
            kprintf("igd: Port %c aktiv (DDI_BUF_CTL %#x)\n", 'A' + port, buf);
    }
    for (int p = 0; p < 3; p++) {
        if (p > 0 && !(pw & PW2_STATE)) { /* Pipes B und C liegen in PW2: ohne Strom nicht lesen */
            kprintf("igd:   Pipe %c: PW2 aus, nicht gelesen\n", 'A' + p);
            continue;
        }
        dump_pipe(p, bi);
    }
    if (info.scanout_pipe < 0)
        kprintf("igd: keine Pipe zeigt den GOP-Framebuffer (%#lx) - bitte dieses Log schicken\n",
                (unsigned long)bi->fb.base);
}
