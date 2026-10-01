/* Intel-Grafik Gen9 (siehe igd.h), Stufe 1: erkennen und den Zustand der Anzeige auslesen. Nur lesende Zugriffe. */

#include "drivers/gpu/igd.h"
#include "drivers/gpu/igd_internal.h"
#include "arch/x86_64/apic.h"
#include "drivers/pci.h"
#include "lib/kprintf.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "console/console.h"
#include "core/sched.h"
#include "core/cmdline.h"
#include "lib/string.h"

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
#define PLANE_SURFLIVE(p)     (0x701AC + PIPE_OFF(p))   /* Surface, die gerade tatsaechlich angezeigt wird */
#define PLANE_BUF_CFG(p)      (0x7027C + PIPE_OFF(p))   /* Anteil am Display-Puffer (DDB): Start | Ende << 16 */
#define PLANE_WM(p, lvl)      (0x70240 + PIPE_OFF(p) + 4u * (uint32_t)(lvl)) /* Watermarks je Stromsparstufe */
#define CUR_BUF_CFG(p)        (0x7017C + PIPE_OFF(p))
#define CUR_WM(p, lvl)        (0x70140 + PIPE_OFF(p) + 4u * (uint32_t)(lvl))
#define DBUF_CTL              0x45008
#define GFX_FLSH_CNTL         0x101008                  /* schreiben: GPU uebernimmt geaenderte GGTT-Eintraege */
#define CUR_SURFLIVE(p)       (0x700AC + PIPE_OFF(p))   /* Mauszeiger-Bild, das gerade angezeigt wird */
#define DE_PIPE_IMR(p)        (0x44404 + 0x10u * (uint32_t)(p)) /* Interrupt-Maske der Pipe */
#define DE_PIPE_IIR(p)        (0x44408 + 0x10u * (uint32_t)(p)) /* festgehaltene Ereignisse (write 1 to clear) */
#define PIPE_FIFO_UNDERRUN    (1u << 31)
#define DDB_BLOCKS            892                       /* Gen9: 896 Bloecke, 4 davon fuer den Bypass-Pfad */

#define GGTT_OFFSET           (8u << 20)                /* globale GTT in BAR0 ab 8 MiB, 64-Bit-Eintraege */
#define PTE_VALID             1ULL
#define PTE_ADDR              0x7FFFFFF000ULL           /* Bits 38:12 */

IgdInfo igd_state = {.scanout_pipe = -1};
volatile uint8_t  *igd_regs;
volatile uint64_t *igd_ggtt;       /* globale Grafik-Seitentabelle (GGTT) */
uint32_t           igd_ggtt_entries;
uint64_t           igd_stolen_base, igd_stolen_size;

uint32_t igd_rd(uint32_t off)
{
    return *(volatile uint32_t *)(igd_regs + off);
}

void igd_wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(igd_regs + off) = v;
}

const IgdInfo *igd_info(void)
{
    return &igd_state;
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
    uint32_t conf = igd_rd(PIPECONF(p));
    if (!(conf & (1u << 31))) {
        kprintf("igd:   Pipe %c: aus\n", 'A' + p);
        return;
    }
    uint32_t src = igd_rd(PIPESRC(p)), ht = igd_rd(HTOTAL(p)), vt = igd_rd(VTOTAL(p));
    uint32_t w = ((src >> 16) & 0x1FFF) + 1, h = (src & 0x1FFF) + 1;
    uint32_t f0 = igd_rd(PIPE_FRMCOUNT(p));
    uint64_t t0 = time_ms();
    WAIT_UNTIL(time_ms() - t0 >= 250, 300);
    uint32_t frames = igd_rd(PIPE_FRMCOUNT(p)) - f0;
    kprintf("igd:   Pipe %c: an (%s), Quelle %ux%u, Timing %ux%u sichtbar von %ux%u, %u Bilder in 250 ms (~%u Hz)\n",
            'A' + p, (conf & (1u << 30)) ? "laeuft" : "startet", w, h, (ht & 0x1FFF) + 1, (vt & 0x1FFF) + 1,
            ((ht >> 16) & 0x1FFF) + 1, ((vt >> 16) & 0x1FFF) + 1, frames, frames * 4);

    uint32_t ctl = igd_rd(PLANE_CTL(p)), stride = igd_rd(PLANE_STRIDE(p)), size = igd_rd(PLANE_SIZE(p));
    uint32_t surf = igd_rd(PLANE_SURF(p)), off = igd_rd(PLANE_OFFSET(p)), pos = igd_rd(PLANE_POS(p));
    kprintf("igd:     Ebene 1: %s, %s, %s, Zeile %u Byte, Groesse %ux%u, Position %u,%u, Surface %#x, Offset %u,%u\n",
            (ctl & (1u << 31)) ? "an" : "aus", plane_format(ctl), plane_tiling(ctl), (stride & 0x3FF) * 64,
            (size & 0xFFF) + 1, ((size >> 16) & 0xFFF) + 1, pos & 0xFFF, (pos >> 16) & 0xFFF, surf & ~0xFFFu,
            off & 0x1FFF, (off >> 16) & 0xFFF);

    uint32_t cur = igd_rd(CUR_CTL(p));
    kprintf("igd:     Mauszeiger-Ebene: %s (CUR_CTL %#x, CUR_BASE %#x, CUR_POS %#x)\n", (cur & 0x27) ? "an" : "aus",
            cur, igd_rd(CUR_BASE(p)), igd_rd(CUR_POS(p)));

    uint32_t ddi = igd_rd(TRANS_DDI_FUNC_CTL(p));
    if (ddi & (1u << 31))
        kprintf("igd:     Transcoder %c -> Port %c (%s), TRANS_DDI_FUNC_CTL %#x\n", 'A' + p, 'A' + ((ddi >> 28) & 7),
                ddi_mode(ddi), ddi);

    /* Zeigt diese Ebene den GOP-Framebuffer? Die CPU sieht Grafikspeicher ueber das Aperture-Fenster (BAR2): die
     * Surface-Adresse ist dort der Abstand zum Anfang. */
    if ((ctl & (1u << 31)) && igd_state.aperture && bi->fb.base == igd_state.aperture + (surf & ~0xFFFu)) {
        igd_state.scanout_pipe = p;
        igd_state.scanout_surf = surf & ~0xFFFu;
        kprintf("igd:     -> zeigt den GOP-Framebuffer (%#lx = Aperture + Surface)\n", (unsigned long)bi->fb.base);
    }
}

static void display_init(const BootInfo *bi);

void igd_init(const BootInfo *bi)
{
    PciDevice d;
    unsigned i = 0;
    while (pci_find_class(0x03, 0x00, -1, i, &d) == 0 && d.vendor != 0x8086) /* evtl. steckt noch eine Grafikkarte */
        i++;
    if (d.vendor != 0x8086 || pci_find_class(0x03, 0x00, -1, i, &d) != 0)
        return;
    igd_state.present = 1;
    igd_state.device = d.device;
    igd_state.name = "Intel-Grafik (unbekanntes Modell)";
    for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++)
        if (known[k].id == d.device)
            igd_state.name = known[k].name;
    igd_state.gen9 = gen9_family(d.device);
    igd_state.mmio = pci_bar_mem(&d, 0);
    igd_state.aperture = pci_bar_mem(&d, 2);
    igd_state.mmio_size = 16u << 20; /* Gen9: 16 MiB, Register in den ersten 2 MiB, globale GTT ab 8 MiB */
    kprintf("igd: %02x:%02x.%u 8086:%04x %s, Register @ %#lx, Aperture @ %#lx, PCI-Rev %u\n", d.bus, d.dev, d.fn,
            d.device, igd_state.name, (unsigned long)igd_state.mmio, (unsigned long)igd_state.aperture, d.revision);
    if (!igd_state.gen9) {
        kprintf("igd: keine Gen9-GPU, Register werden nicht gelesen\n");
        return;
    }
    if (!igd_state.mmio || paging_map_mmio(igd_state.mmio, 2u << 20) != 0) {
        kprintf("igd: Register nicht einblendbar\n");
        return;
    }
    igd_regs = (volatile uint8_t *)igd_state.mmio;
    pci_set_driver(&d, "igd");

    /* Stolen Memory: von der Firmware fuer die Grafik reservierter Hauptspeicher (PCI 0x5C: Basis, 0x50: Groesse) */
    uint32_t bdsm = pci_read32(&d, 0x5C) & ~0xFFFFFu, gmch = pci_read32(&d, 0x50);
    uint32_t gms = (gmch >> 8) & 0xFF;
    kprintf("igd: Stolen Memory @ %#x, %u MiB (GMCH_CTRL %#x)\n", bdsm, gms < 0xF0 ? gms * 32 : 4 * (gms - 0xF0 + 1), gmch);
    igd_stolen_base = bdsm;
    igd_stolen_size = (uint64_t)(gms < 0xF0 ? gms * 32 : 4 * (gms - 0xF0 + 1)) << 20;

    /* GGTT: Groesse aus GMCH_CTRL Bits 7:6 (1 = 2 MiB, 2 = 4 MiB, 3 = 8 MiB Eintraege), liegt in BAR0 ab 8 MiB */
    uint32_t ggms = (gmch >> 6) & 3;
    uint32_t ggtt_bytes = ggms ? (1u << ggms) << 20 : 0;
    if (ggtt_bytes && paging_map_mmio(igd_state.mmio + GGTT_OFFSET, ggtt_bytes) == 0) {
        igd_ggtt = (volatile uint64_t *)(igd_state.mmio + GGTT_OFFSET);
        igd_ggtt_entries = ggtt_bytes / 8;
        kprintf("igd: GGTT %u MiB (%u Eintraege, %u MiB Grafik-Adressraum), Eintrag 0 = %#lx\n", ggtt_bytes >> 20,
                igd_ggtt_entries, igd_ggtt_entries / 256, (unsigned long)igd_ggtt[0]);
    }

    uint32_t pw = igd_rd(PWR_WELL_CTL_BIOS) | igd_rd(PWR_WELL_CTL_DRIVER);
    kprintf("igd: Leistungsbereiche: PW1 %s, PW2 %s (BIOS %#x, Treiber %#x)\n", (pw & PW1_STATE) ? "an" : "aus",
            (pw & PW2_STATE) ? "an" : "aus", igd_rd(PWR_WELL_CTL_BIOS), igd_rd(PWR_WELL_CTL_DRIVER));
    for (int port = 0; port < 5; port++) {
        uint32_t buf = igd_rd(DDI_BUF_CTL(port));
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
    if (igd_state.scanout_pipe < 0) {
        kprintf("igd: keine Pipe zeigt den GOP-Framebuffer (%#lx) - bitte dieses Log schicken\n",
                (unsigned long)bi->fb.base);
        return;
    }
    display_init(bi);
    igd_irq_init(&d);
    igd_blt_init();
}

/* ---------- Stufe 2: Page-Flipping (nur auf Befehl: igdtest) ---------- */

void igd_clflush(uint64_t addr, uint64_t len)
{
    for (uint64_t a = addr & ~63ULL; a < addr + len; a += 64)
        __asm__ __volatile__("clflush (%0)" : : "r"(a) : "memory");
    __asm__ __volatile__("mfence" : : : "memory");
}

void igd_ggtt_flush(void)
{
    igd_wr(GFX_FLSH_CNTL, 1);
    (void)igd_rd(GFX_FLSH_CNTL);
}

/* Wartet, bis die Ebene surf tatsaechlich anzeigt; Ergebnis: Bilder bis dahin, -1 = Zeitueberschreitung */
static int wait_live(int p, uint32_t surf)
{
    uint32_t f0 = igd_rd(PIPE_FRMCOUNT(p));
    if (!WAIT_UNTIL((igd_rd(PLANE_SURFLIVE(p)) & ~0xFFFu) == surf, 200))
        return -1;
    return (int)(igd_rd(PIPE_FRMCOUNT(p)) - f0);
}

/* FIFO-Unterlauf der Pipe (der Display-Engine gingen die Daten aus: Flackern/Stoerstreifen) festhalten lassen. Nur
 * die Maske wird geoeffnet, IER bleibt, wie es ist: es entsteht kein Interrupt, das Ereignis steht nur im IIR. */
uint32_t igd_underrun_begin(int p)
{
    uint32_t imr = igd_rd(DE_PIPE_IMR(p));
    igd_wr(DE_PIPE_IIR(p), PIPE_FIFO_UNDERRUN);
    igd_wr(DE_PIPE_IMR(p), imr & ~PIPE_FIFO_UNDERRUN);
    return imr;
}

int igd_underrun_end(int p, uint32_t imr)
{
    int seen = (igd_rd(DE_PIPE_IIR(p)) & PIPE_FIFO_UNDERRUN) != 0;
    igd_wr(DE_PIPE_IIR(p), PIPE_FIFO_UNDERRUN);
    igd_wr(DE_PIPE_IMR(p), imr);
    return seen;
}

/* Prueft, ob der GGTT-Bereich [base, base + pages) frei ist, und sichert seine Eintraege nach saved. Die Firmware
 * traegt nur ein, was sie braucht (den Framebuffer im Stolen Memory); der Rest der GGTT ist ungeloeschter
 * Speicherinhalt, den niemand liest. Frei heisst deshalb: kein Eintrag zeigt gueltig ins Stolen Memory. */
int igd_ggtt_claim(uint32_t base, uint32_t pages, uint64_t *saved)
{
    if (base + pages > igd_ggtt_entries) {
        kprintf("igdtest: GGTT zu klein\n");
        return -4;
    }
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t e = igd_ggtt[base + i];
        if ((e & PTE_VALID) && (e & PTE_ADDR) >= igd_stolen_base && (e & PTE_ADDR) < igd_stolen_base + igd_stolen_size) {
            kprintf("igdtest: GGTT-Eintrag %#x zeigt ins Stolen Memory (%#lx): wird benutzt, Abbruch\n", base + i,
                    (unsigned long)e);
            return -5;
        }
        saved[i] = e;
    }
    return 0;
}

/* Gemeinsame Vorpruefung: Gen9, Framebuffer erkannt, GGTT-Eintrag der angezeigten Surface im Stolen Memory */
int igd_preflight(const char *what)
{
    int p = igd_state.scanout_pipe;
    if (!igd_state.gen9 || !igd_regs || !igd_ggtt || p < 0) {
        kprintf("igdtest: keine passende Intel-GPU / kein erkannter Framebuffer\n");
        return -1;
    }
    kprintf("igdtest: %s auf Pipe %c\n", what, 'A' + p);
    uint32_t surf = igd_rd(PLANE_SURF(p)) & ~0xFFFu;
    uint64_t pte = igd_ggtt[surf >> 12];
    kprintf("igdtest: GGTT[%#x] = %#lx, Stolen Memory @ %#lx\n", surf >> 12, (unsigned long)pte,
            (unsigned long)igd_stolen_base);
    if (!(pte & PTE_VALID) || (pte & PTE_ADDR) < igd_stolen_base || (pte & PTE_ADDR) >= igd_stolen_base + igd_stolen_size) {
        kprintf("igdtest: GGTT-Eintrag passt nicht zum Stolen Memory, Abbruch (nichts veraendert)\n");
        return -3;
    }
    uint32_t imr = igd_underrun_begin(p); /* laeuft die Anzeige schon vorher sauber? */
    thread_sleep_ms(200);
    kprintf("igdtest: vorher (200 ms): %s\n", igd_underrun_end(p, imr) ? "FIFO-Unterlauf! (schon vor dem Test)" :
                                                                    "kein FIFO-Unterlauf");
    return 0;
}

/* Nur lesen: Aufteilung des Display-Puffers und Watermarks (Voraussetzung fuer den Hardware-Mauszeiger) */
static void dump_ddb(int p)
{
    uint32_t pb = igd_rd(PLANE_BUF_CFG(p)), cb = igd_rd(CUR_BUF_CFG(p));
    kprintf("igd: DDB: DBUF_CTL %#x, Ebene 1 Bloecke %u-%u, Mauszeiger Bloecke %u-%u\n", igd_rd(DBUF_CTL), pb & 0x3FF,
            (pb >> 16) & 0x3FF, cb & 0x3FF, (cb >> 16) & 0x3FF);
    for (int lvl = 0; lvl < 8; lvl++) {
        uint32_t pw = igd_rd(PLANE_WM(p, lvl)), cw = igd_rd(CUR_WM(p, lvl));
        kprintf("igd:   WM%d: Ebene 1 %s %u Zeilen %u Bloecke | Mauszeiger %s %u Zeilen %u Bloecke\n", lvl,
                (pw >> 31) ? "an " : "aus", (pw >> 14) & 0x1F, pw & 0x3FF, (cw >> 31) ? "an " : "aus",
                (cw >> 14) & 0x1F, cw & 0x3FF);
    }
}

int igd_flip_test(void)
{
    int p = igd_state.scanout_pipe;
    int pre = igd_preflight("Stufe 2 - Page-Flipping");
    if (pre)
        return pre;
    dump_ddb(p);

    uint32_t ctl = igd_rd(PLANE_CTL(p)), size = igd_rd(PLANE_SIZE(p));
    uint32_t w = (size & 0xFFF) + 1, h = ((size >> 16) & 0xFFF) + 1, stride = (igd_rd(PLANE_STRIDE(p)) & 0x3FF) * 64;
    uint32_t old_surf = igd_rd(PLANE_SURF(p)) & ~0xFFFu;
    if ((ctl & (7u << 10)) || ((ctl >> 24) & 0xF) != 4 || stride < w * 4) {
        kprintf("igdtest: Ebene ist nicht linear/32 Bit, Abbruch\n");
        return -2;
    }

    /* 2. Freier GGTT-Bereich ab der Haelfte des Adressraums; alte Eintraege sichern */
    uint64_t bytes = (uint64_t)stride * h;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096), base = igd_ggtt_entries / 2;
    uint64_t *frames = kmalloc(sizeof(uint64_t) * pages * 2); /* [0, pages): Seiten, [pages, 2 pages): alte GGTT */
    if (!frames)
        return -6;
    uint64_t *saved = frames + pages;
    int claim = igd_ggtt_claim(base, pages, saved);
    if (claim) {
        kfree(frames);
        return claim;
    }
    kprintf("igdtest: GGTT-Eintraege %#x..%#x frei (alte Werte gesichert)\n", base, base + pages - 1);

    /* 3. Zweiter Bildpuffer im RAM: das aktuelle Bild mit invertierten Farben */
    uint32_t got = 0;
    for (; got < pages; got++)
        if (!(frames[got] = pmm_alloc_frame()))
            break;
    int rc = 0;
    if (got < pages) {
        kprintf("igdtest: kein Speicher fuer %u Seiten\n", pages);
        rc = -6;
        goto free_frames;
    }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < stride / 4; x++) {
            uint64_t off = (uint64_t)y * stride + (uint64_t)x * 4;
            uint32_t px = x < w && x < console_width_px() && y < console_height_px() ? console_read_pixel(x, y) : 0;
            *(uint32_t *)(frames[off >> 12] + (off & 4095)) = px ^ 0x00FFFFFF;
        }
    for (uint32_t i = 0; i < pages; i++) /* die Display-Engine liest am CPU-Cache vorbei */
        igd_clflush(frames[i], 4096);

    /* 4. GGTT-Eintraege setzen, dann umschalten */
    for (uint32_t i = 0; i < pages; i++)
        igd_ggtt[base + i] = frames[i] | PTE_VALID;
    igd_ggtt_flush();
    uint32_t new_surf = base << 12;
    uint32_t imr = igd_underrun_begin(p);
    uint64_t t0 = time_ms();
    igd_wr(PLANE_SURF(p), new_surf);
    int f1 = wait_live(p, new_surf);
    kprintf("igdtest: Flip auf %#x: %s nach %d Bild(ern), %lu ms\n", new_surf, f1 >= 0 ? "angezeigt" : "NICHT angezeigt",
            f1, (unsigned long)(time_ms() - t0));
    thread_sleep_ms(2000); /* invertiertes Bild stehen lassen */

    /* 5. Zehn Wechsel hin und her: jeder sollte nach hoechstens einem Bild sichtbar sein */
    int worst = 0, fails = 0;
    for (int i = 0; i < 10; i++) {
        uint32_t s = (i & 1) ? new_surf : old_surf;
        igd_wr(PLANE_SURF(p), s);
        int f = wait_live(p, s);
        if (f < 0)
            fails++;
        else if (f > worst)
            worst = f;
        thread_sleep_ms(100);
    }
    kprintf("igdtest: 10 Wechsel: %d ohne Anzeige, laengstens %d Bild(er) bis sichtbar\n", fails, worst);

    /* 6. Zurueck zum Original, GGTT wiederherstellen */
    igd_wr(PLANE_SURF(p), old_surf);
    int f2 = wait_live(p, old_surf);
    for (uint32_t i = 0; i < pages; i++)
        igd_ggtt[base + i] = saved[i];
    igd_ggtt_flush();
    int underrun = igd_underrun_end(p, imr);
    kprintf("igdtest: zurueck auf das Original: %s; FIFO-Unterlauf waehrend des Tests: %s\n",
            f2 >= 0 ? "ok" : "NICHT bestaetigt", underrun ? "JA" : "nein");
    rc = f1 >= 0 && fails == 0 && f2 >= 0 && !underrun ? 0 : -7;
    kprintf("igdtest: %s\n", rc == 0 ? "Page-Flipping funktioniert" : "Page-Flipping mit Fehlern, bitte Log schicken");

free_frames:
    for (uint32_t i = 0; i < got; i++)
        pmm_free_frame(frames[i]);
    kfree(frames);
    return rc;
}

/* ---------- Stufe 2: Hardware-Mauszeiger (nur auf Befehl: igdtest cursor) ---------- */

/* Pfeil wie der Software-Zeiger der Konsole ('X' schwarz, '.' weiss), doppelt so gross */
static const char *const arrow[19] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ",
    "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ",
    "X    X..X   ", "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};

static uint32_t cur_pos(int x, int y)
{
    uint32_t v = 0;
    v |= x < 0 ? (1u << 15) | ((uint32_t)-x & 0xFFF) : ((uint32_t)x & 0xFFF);
    v |= y < 0 ? (1u << 31) | (((uint32_t)-y & 0xFFF) << 16) : (((uint32_t)y & 0xFFF) << 16);
    return v;
}

int igd_cursor_test(void)
{
    int p = igd_state.scanout_pipe;
    int pre = igd_preflight("Stufe 2 - Hardware-Mauszeiger");
    if (pre)
        return pre;
    dump_ddb(p);

    /* Platz im Display-Puffer: direkt hinter der Bildebene 32 Bloecke (so viel gibt Linux dem Zeiger bei einer Pipe) */
    uint32_t plane_end = (igd_rd(PLANE_BUF_CFG(p)) >> 16) & 0x3FF;
    uint32_t start = plane_end + 1, end = start + 31;
    if (end >= DDB_BLOCKS) {
        kprintf("igdtest: kein Platz im Display-Puffer hinter der Bildebene (%u), Abbruch\n", plane_end);
        return -8;
    }

    /* 64x64 ARGB = 16 KiB = 4 Seiten, in der GGTT oberhalb des Flip-Bereichs */
    uint32_t base = igd_ggtt_entries / 2 + 0x20000, pages = 4;
    uint64_t frames[4], saved[4];
    int claim = igd_ggtt_claim(base, pages, saved);
    if (claim)
        return claim;
    uint32_t got = 0;
    for (; got < pages; got++)
        if (!(frames[got] = pmm_alloc_frame()))
            break;
    int rc = 0;
    if (got < pages) {
        rc = -6;
        goto free_frames;
    }
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t px = 0; /* durchsichtig */
            if (y / 2 < 19 && x / 2 < 12) {
                char c = arrow[y / 2][x / 2];
                px = c == 'X' ? 0xFF000000u : c == '.' ? 0xFFFFFFFFu : 0;
            }
            uint64_t off = (uint64_t)y * 256 + (uint64_t)x * 4;
            *(uint32_t *)(frames[off >> 12] + (off & 4095)) = px;
        }
    for (uint32_t i = 0; i < pages; i++) {
        igd_clflush(frames[i], 4096);
        igd_ggtt[base + i] = frames[i] | PTE_VALID;
    }
    igd_ggtt_flush();

    /* Register sichern, Zeiger einschalten. Alle Zeiger-Register sind doppelt gepuffert: erst das Schreiben von
     * CUR_BASE uebernimmt sie beim naechsten Bildwechsel. */
    uint32_t s_ctl = igd_rd(CUR_CTL(p)), s_base = igd_rd(CUR_BASE(p)), s_pos = igd_rd(CUR_POS(p)), s_buf = igd_rd(CUR_BUF_CFG(p));
    uint32_t s_wm0 = igd_rd(CUR_WM(p, 0)), surf = base << 12;
    int cx = (int)console_width_px() / 2, cy = (int)console_height_px() / 2, x = 300, y = 0;
    uint32_t imr = igd_underrun_begin(p);
    igd_wr(CUR_BUF_CFG(p), (end << 16) | start);
    igd_wr(CUR_WM(p, 0), (1u << 31) | (1u << 14) | 8); /* Stufe 0 an: 1 Zeile, 8 Bloecke (wie die Firmware fuer Ebene 1) */
    igd_wr(CUR_CTL(p), 0x27);                          /* 64x64, 32 Bit ARGB */
    igd_wr(CUR_POS(p), cur_pos(cx + x, cy + y));
    igd_wr(CUR_BASE(p), surf);
    uint32_t f0 = igd_rd(PIPE_FRMCOUNT(p));
    int live = WAIT_UNTIL((igd_rd(CUR_SURFLIVE(p)) & ~0xFFFu) == surf, 200);
    kprintf("igdtest: Zeiger an (DDB %u-%u): %s nach %u Bild(ern)\n", start, end, live ? "angezeigt" : "NICHT angezeigt",
            igd_rd(PIPE_FRMCOUNT(p)) - f0);

    /* 3 Sekunden im Kreis (Minskys Kreis-Algorithmus: nur ganze Zahlen) */
    for (int step = 0; step < 150; step++) {
        x -= y / 16;
        y += x / 16;
        igd_wr(CUR_POS(p), cur_pos(cx + x, cy + y));
        igd_wr(CUR_BASE(p), surf); /* uebernehmen */
        thread_sleep_ms(20);
    }

    /* Aus und alles zurueck */
    igd_wr(CUR_CTL(p), s_ctl);
    igd_wr(CUR_POS(p), s_pos);
    igd_wr(CUR_BASE(p), s_base);
    thread_sleep_ms(50);
    igd_wr(CUR_WM(p, 0), s_wm0);
    igd_wr(CUR_BUF_CFG(p), s_buf);
    igd_wr(CUR_BASE(p), s_base);
    thread_sleep_ms(50);
    int underrun = igd_underrun_end(p, imr);
    for (uint32_t i = 0; i < pages; i++)
        igd_ggtt[base + i] = saved[i];
    igd_ggtt_flush();
    kprintf("igdtest: Zeiger wieder aus, Register und GGTT zurueck; FIFO-Unterlauf waehrend des Tests: %s\n",
            underrun ? "JA" : "nein");
    rc = live && !underrun ? 0 : -7;
    kprintf("igdtest: %s\n", rc == 0 ? "Hardware-Mauszeiger funktioniert" : "Mauszeiger mit Fehlern, bitte Log schicken");

free_frames:
    for (uint32_t i = 0; i < got; i++)
        pmm_free_frame(frames[i]);
    return rc;
}

/* ---------- Fest eingebaut: Hardware-Mauszeiger und Doppelpufferung ----------
 *
 * Mauszeiger: eigene Ebene der Pipe (64x64 ARGB), die Konsole und Grafikprogramme nur noch verschieben.
 * Doppelpufferung: Puffer A ist der Framebuffer der Firmware (Stolen Memory, fuer die CPU ueber die Aperture), B ein
 * gleich grosser Puffer im RAM. Ein komplettes Bild eines Grafikprogramms kommt in den gerade nicht angezeigten Puffer,
 * dann wird beim naechsten Bildwechsel umgeschaltet: kein Tearing. Teil-Updates gehen in den angezeigten Puffer; der
 * andere wird vor seiner naechsten Anzeige ohnehin ganz ueberschrieben. Gewartet wird erst, bevor der naechste Puffer
 * beschrieben wird: das Programm laeuft so von selbst im Takt der Bildrate. Kopiert wird nach Moeglichkeit vom Blitter
 * der GPU (igd_blt.c): Teil-Updates im Hintergrund; ganze Bilder kopiert die CPU (sie ist dabei schneller).
 * "noigd" in der Kommandozeile schaltet beides ab (dann wie vorher alles in Software). */

static int       hw_cursor;           /* Zeiger-Ebene eingerichtet */
static uint32_t  cursor_surf;
static int       cursor_on = -1;
int       igd_flip_ready;          /* Puffer B eingerichtet */
uint32_t  igd_scr_w, igd_scr_h, igd_scr_stride;
uint32_t  igd_surf_a, igd_surf_b;
uint8_t  *igd_buf_a, *igd_buf_b;       /* CPU-Adressen: A ueber die Aperture (write-combining), B im RAM */
static int       front_b;             /* 1: B wird angezeigt (oder der Wechsel dorthin steht an) */
static uint32_t  pending;             /* Surface, deren Wechsel noch nicht bestaetigt ist, 0 = keiner */

/* Zaehler fuer igdtest info: was kosten die Bild-Updates der Grafikprogramme? */
static struct {
    uint64_t full_n, full_wait_us, full_copy_us, full_max_us; /* ganze Bilder (Doppelpufferung) */
    uint64_t part_n, part_px, part_us;                         /* Teil-Updates in den angezeigten Puffer */
} bstat;

/* Pfeil wie bei macOS: schwarz, weisser Rand, weicher Schatten, kantengeglaettet (4x4 Abtastpunkte je Pixel).
 * Nur Ganzzahlen (der Kernel rechnet ohne FPU). Eckpunkte in 1/16 Pixel bei Groesse 1; die Spitze liegt bei (2, 2). */
static const int32_t arrow_poly[7][2] = {
    {32, 32}, {32, 288}, {96, 229}, {138, 326}, {176, 310}, {136, 214}, {216, 214},
};
static uint32_t cursor_hot; /* Spitze in Pixeln (fuer die Position) */

static int arrow_inside(int64_t x, int64_t y, int64_t f) /* x, y und Eckpunkte*f in 1/64 Pixel */
{
    int in = 0;
    for (int i = 0, j = 6; i < 7; j = i++) {
        int64_t xi = arrow_poly[i][0] * f, yi = arrow_poly[i][1] * f, xj = arrow_poly[j][0] * f, yj = arrow_poly[j][1] * f;
        if ((yi > y) != (yj > y) && x < xi + (xj - xi) * (y - yi) / (yj - yi))
            in = !in;
    }
    return in;
}

static int64_t arrow_dist2(int64_t x, int64_t y, int64_t f) /* Abstand zum Rand, zum Quadrat */
{
    int64_t best = INT64_MAX;
    for (int i = 0, j = 6; i < 7; j = i++) {
        int64_t ax = arrow_poly[j][0] * f, ay = arrow_poly[j][1] * f, bx = arrow_poly[i][0] * f, by = arrow_poly[i][1] * f;
        int64_t dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy, t = (x - ax) * dx + (y - ay) * dy;
        int64_t px = ax, py = ay;
        if (t >= len) { px = bx; py = by; }
        else if (t > 0) { px = ax + dx * t / len; py = ay + dy * t / len; }
        int64_t d = (x - px) * (x - px) + (y - py) * (y - py);
        if (d < best)
            best = d;
    }
    return best;
}

static void draw_cursor_image(uint64_t frames[4])
{
    uint32_t sc = console_scale();
    if (sc < 1)
        sc = 1;
    if (sc > 3)
        sc = 3;
    int64_t f = sc == 1 ? 4 : 1 + (int64_t)sc * 2; /* 1x, 1,25x, 1,75x in Vierteln: 16tel * f = 1/64 Pixel */
    int64_t unit = 64;                  /* 1 Pixel (bei Groesse 1) in 1/64, skaliert unten */
    int64_t border = unit * 3 / 2 * f / 4, sh_dy = unit * 3 / 2 * f / 4, sh_r = unit * 3 * f / 4;
    cursor_hot = (uint32_t)(2 * f / 4);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t a = 0, wsum = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int64_t X = (int64_t)x * 64 + sx * 16 + 8, Y = (int64_t)y * 64 + sy * 16 + 8;
                    if (arrow_inside(X, Y, f)) {
                        a += 255;
                        continue;
                    }
                    int64_t d = arrow_dist2(X, Y, f);
                    if (d <= border * border) {
                        a += 255;
                        wsum += 255;
                        continue;
                    }
                    /* Schatten: Pfeil etwas nach unten verschoben, nach aussen auslaufend */
                    int64_t ds = arrow_inside(X, Y - sh_dy, f) ? 0 : arrow_dist2(X, Y - sh_dy, f);
                    if (ds < sh_r * sh_r) {
                        int64_t k = 0; /* Wurzel ganzzahlig */
                        while ((k + 1) * (k + 1) <= ds)
                            k++;
                        a += (uint32_t)(90 * (sh_r - k) / sh_r);
                    }
                }
            a /= 16;
            wsum /= 16;
            uint32_t px = (a << 24) | (wsum << 16) | (wsum << 8) | wsum; /* vormultipliziert */
            uint64_t off = (uint64_t)y * 256 + (uint64_t)x * 4;
            *(uint32_t *)(frames[off >> 12] + (off & 4095)) = px;
        }
    for (int i = 0; i < 4; i++)
        igd_clflush(frames[i], 4096);
}

static int setup_cursor(int p)
{
    uint32_t plane_end = (igd_rd(PLANE_BUF_CFG(p)) >> 16) & 0x3FF, start = plane_end + 1, end = start + 31;
    uint32_t base = igd_ggtt_entries / 2 + 0x30000; /* nicht dort, wo igdtest seine Probe-Bereiche hat */
    uint64_t saved[4], frames[4];
    if (end >= DDB_BLOCKS || igd_ggtt_claim(base, 4, saved) != 0)
        return -1;
    for (int i = 0; i < 4; i++)
        if (!(frames[i] = pmm_alloc_frame())) {
            while (i--)
                pmm_free_frame(frames[i]);
            return -1;
        }
    draw_cursor_image(frames);
    for (int i = 0; i < 4; i++)
        igd_ggtt[base + i] = frames[i] | PTE_VALID;
    igd_ggtt_flush();
    cursor_surf = base << 12;
    igd_wr(CUR_BUF_CFG(p), (end << 16) | start);
    igd_wr(CUR_WM(p, 0), (1u << 31) | (1u << 14) | 8);
    igd_wr(CUR_CTL(p), 0);            /* erst sichtbar, wenn eine Maus da ist */
    igd_wr(CUR_BASE(p), cursor_surf);
    hw_cursor = 1;
    cursor_on = 0;
    return 0;
}

static int setup_flip(int p)
{
    uint32_t pages = (uint32_t)(((uint64_t)igd_scr_stride * igd_scr_h + 4095) / 4096);
    uint32_t base = igd_ggtt_entries / 4; /* 1 GiB: getrennt von igdtest (ab 2 GiB) */
    uint64_t *saved = kmalloc(sizeof(uint64_t) * pages);
    if (!saved)
        return -1;
    int claim = igd_ggtt_claim(base, pages, saved);
    kfree(saved); /* die Eintraege gehoeren ab jetzt dauerhaft uns */
    if (claim)
        return -1;
    uint64_t b = pmm_alloc_frames(pages); /* am Stueck: dann ist jede Zeile zusammenhaengend */
    if (!b)
        return -1;
    for (uint32_t i = 0; i < pages; i++)
        igd_ggtt[base + i] = (b + (uint64_t)i * 4096) | PTE_VALID;
    igd_ggtt_flush();
    igd_buf_b = (uint8_t *)b;
    igd_surf_b = base << 12;
    (void)p;
    igd_flip_ready = 1;
    return 0;
}

static void display_init(const BootInfo *bi)
{
    if (cmdline_has("noigd")) {
        kprintf("igd: 'noigd': Mauszeiger und Doppelpufferung bleiben in Software\n");
        return;
    }
    int p = igd_state.scanout_pipe;
    uint32_t ctl = igd_rd(PLANE_CTL(p)), size = igd_rd(PLANE_SIZE(p));
    igd_scr_w = (size & 0xFFF) + 1;
    igd_scr_h = ((size >> 16) & 0xFFF) + 1;
    igd_scr_stride = (igd_rd(PLANE_STRIDE(p)) & 0x3FF) * 64;
    if ((ctl & (7u << 10)) || ((ctl >> 24) & 0xF) != 4 || igd_scr_stride < igd_scr_w * 4) {
        kprintf("igd: Ebene nicht linear/32 Bit: Mauszeiger und Doppelpufferung bleiben in Software\n");
        return;
    }
    igd_surf_a = igd_state.scanout_surf;
    igd_buf_a = (uint8_t *)bi->fb.base;
    int c = setup_cursor(p), f = setup_flip(p);
    kprintf("igd: Hardware-Mauszeiger %s, Doppelpufferung %s (zweiter Puffer %u KiB im RAM)\n", c == 0 ? "an" : "AUS",
            f == 0 ? "an" : "AUS", (uint32_t)((uint64_t)igd_scr_stride * igd_scr_h / 1024));
    igd_modes_boot();
}

/* Nach dem Umschalten des Anschlusses (igd_mode.c): Zeiger-Ebene wieder so setzen, wie der Treiber sie fuehrt */
void igd_cursor_reapply(void)
{
    if (!hw_cursor)
        return;
    int p = igd_state.scanout_pipe;
    igd_wr(CUR_CTL(p), cursor_on == 1 ? 0x27 : 0);
    igd_wr(CUR_BASE(p), cursor_surf);
}

int igd_cursor_available(void)
{
    return hw_cursor;
}

void igd_cursor_move(int x, int y, int visible)
{
    if (!hw_cursor)
        return;
    int p = igd_state.scanout_pipe;
    if (visible != cursor_on) {
        igd_wr(CUR_CTL(p), visible ? 0x27 : 0);
        cursor_on = visible;
    }
    igd_wr(CUR_POS(p), cur_pos(x - (int)cursor_hot, y - (int)cursor_hot));
    igd_wr(CUR_BASE(p), cursor_surf); /* uebernimmt Position/Sichtbarkeit beim naechsten Bildwechsel */
}

/* Wartet, bis der zuletzt angestossene Wechsel angezeigt wird (danach wird der andere Puffer nicht mehr gelesen) */
static void wait_flip(void)
{
    if (!pending)
        return;
    int p = igd_state.scanout_pipe;
    for (int i = 0; i < 20 && (igd_rd(PLANE_SURFLIVE(p)) & ~0xFFFu) != pending; i++)
        if (!igd_wait_vblank(30)) /* der Wechsel passiert beim naechsten Bildwechsel: darauf warten */
            thread_sleep_ms(2);   /* ohne Interrupt: nachsehen, hoechstens ein Bild lang */
    pending = 0;
}

/* Kopieren mit Non-Temporal-Stores (n Vielfaches von 4): die Daten gehen am Cache vorbei direkt in den RAM. Die
 * Display-Engine liest am Cache vorbei, so ist kein clflush noetig (das war achtmal langsamer als das Kopieren
 * selbst: igdtest info). Danach ist ein sfence noetig. */
static void nt_copy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (((uint64_t)d & 4) && n >= 4) { /* auf 8 Byte ausrichten */
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint32_t *)d) : "r"(*(const uint32_t *)s));
        d += 4, s += 4, n -= 4;
    }
    for (; n >= 64; d += 64, s += 64, n -= 64) /* eine Cache-Zeile am Stueck */
        for (int k = 0; k < 64; k += 8) {
            uint64_t v;
            __builtin_memcpy(&v, s + k, 8);
            __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint64_t *)(d + k)) : "r"(v));
        }
    for (; n >= 8; d += 8, s += 8, n -= 8) {
        uint64_t v;
        __builtin_memcpy(&v, s, 8);
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint64_t *)d) : "r"(v));
    }
    if (n >= 4)
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint32_t *)d) : "r"(*(const uint32_t *)s));
}

/* nt = 1: RAM-Puffer B (Non-Temporal-Stores), sonst Framebuffer A (write-combining, normales memcpy) */
static void copy_rect(uint8_t *dst, const uint32_t *src, uint32_t pitch, int x, int y, int w, int h, int nt)
{
    for (int yy = 0; yy < h; yy++) {
        uint8_t *d = dst + (uint64_t)(y + yy) * igd_scr_stride + (uint64_t)x * 4;
        if (nt)
            nt_copy(d, src + (uint64_t)yy * pitch, (uint64_t)w * 4);
        else
            memcpy(d, src + (uint64_t)yy * pitch, (uint64_t)w * 4);
    }
    __asm__ __volatile__("sfence" : : : "memory"); /* Non-Temporal- bzw. write-combining-Puffer leeren */
}

/* Der angezeigte Puffer (dorthin gehen Teil-Updates) - fuer das Anzeigen durch die Render-Engine (igd_comp.c) */
int igd_front_surface(uint32_t *gtt, uint32_t *pitch, uint32_t *w, uint32_t *h)
{
    if (!igd_flip_ready)
        return -1;
    *gtt = front_b ? igd_surf_b : igd_surf_a;
    *pitch = igd_scr_stride;
    *w = igd_scr_w;
    *h = igd_scr_h;
    return 0;
}

int igd_gfx_blit(const uint32_t *src, uint32_t pitch, int x, int y, int w, int h)
{
    if (!igd_flip_ready || x < 0 || y < 0 || (uint32_t)(x + w) > igd_scr_w || (uint32_t)(y + h) > igd_scr_h)
        return 0;
    igd_rcs_comp_wait(0); /* die Render-Engine schreibt vielleicht noch ein Bild in den Puffer (igd_comp.c) */
    uint64_t t0 = time_us();
    if (x == 0 && y == 0 && (uint32_t)w == igd_scr_w && (uint32_t)h == igd_scr_h) { /* ganzes Bild: in den Hintergrund, umschalten */
        wait_flip();
        uint64_t t1 = time_us();
        int to_b = !front_b;
        /* die CPU ist hier schneller als der Blitter (3,1 statt 5,3 ms bei 3440x1440); vorher die Auftraege des
         * Blitters abwarten, die vielleicht noch in diesen (eben noch angezeigten) Puffer schreiben */
        igd_blt_sync();
        copy_rect(to_b ? igd_buf_b : igd_buf_a, src, pitch, 0, 0, w, h, to_b);
        pending = to_b ? igd_surf_b : igd_surf_a;
        igd_wr(PLANE_SURF(igd_state.scanout_pipe), pending);
        front_b = to_b;
        uint64_t t2 = time_us();
        bstat.full_n++;
        bstat.full_wait_us += t1 - t0;
        bstat.full_copy_us += t2 - t1;
        if (t2 - t1 > bstat.full_max_us)
            bstat.full_max_us = t2 - t1;
        return 1;
    }
    /* Teil-Update: in den angezeigten Puffer. Der Blitter kopiert im Hintergrund; die CPU (kleine Ausschnitte) muss nicht
     * auf ihn warten - er liest das Programmbild erst beim Kopieren, also nie aelteres als die CPU jetzt */
    if (!igd_blt_copy_user(front_b ? igd_surf_b : igd_surf_a, src, pitch, x, y, w, h, 0))
        copy_rect(front_b ? igd_buf_b : igd_buf_a, src, pitch, x, y, w, h, front_b);
    bstat.part_n++;
    bstat.part_px += (uint64_t)w * (uint64_t)h;
    bstat.part_us += time_us() - t0;
    return 1;
}

void igd_gfx_sync(void)
{
    igd_rcs_comp_wait(0);
    igd_blt_sync();
}

void igd_gfx_end(void)
{
    if (!igd_flip_ready)
        return;
    igd_rcs_comp_wait(0);
    igd_blt_sync();
    wait_flip();
    if (front_b) { /* die Konsole zeichnet in A */
        pending = igd_surf_a;
        igd_wr(PLANE_SURF(igd_state.scanout_pipe), igd_surf_a);
        wait_flip();
        front_b = 0;
    }
}

/* ---------- igdtest info: Zaehler und Vergleich der Kopierwege fuer ganze Bilder ---------- */

static int has_clflushopt(void)
{
    uint32_t a = 7, b, c = 0, d;
    __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (b >> 23) & 1;
}

static void flush_opt(uint64_t addr, uint64_t len)
{
    for (uint64_t a = addr & ~63ULL; a < addr + len; a += 64)
        __asm__ __volatile__("clflushopt (%0)" : : "r"(a) : "memory");
    __asm__ __volatile__("sfence" : : : "memory");
}

static uint64_t us_avg(uint64_t us, uint64_t n)
{
    return n ? us / n : 0;
}

int igd_info_report(void)
{
    if (!igd_state.present || !igd_state.gen9) {
        kprintf("igdinfo: keine Intel-Grafik (Gen9) gefunden\n");
        return -1;
    }
    kprintf("igdinfo: Mauszeiger %s, Doppelpufferung %s, %ux%u, Zeile %u Byte\n", hw_cursor ? "Hardware" : "Software",
            igd_flip_ready ? "an" : "aus", igd_scr_w, igd_scr_h, igd_scr_stride);
    kprintf("igdinfo: ganze Bilder: %lu, je %lu us kopieren (max %lu us), %lu us auf den Bildwechsel gewartet\n",
            (unsigned long)bstat.full_n, (unsigned long)us_avg(bstat.full_copy_us, bstat.full_n),
            (unsigned long)bstat.full_max_us, (unsigned long)us_avg(bstat.full_wait_us, bstat.full_n));
    kprintf("igdinfo: Teil-Updates: %lu, je %lu Pixel, %lu us\n", (unsigned long)bstat.part_n,
            (unsigned long)us_avg(bstat.part_px, bstat.part_n), (unsigned long)us_avg(bstat.part_us, bstat.part_n));
    igd_blt_report();
    igd_comp_report();
    if (!igd_flip_ready)
        return 0;
    igd_blt_sync();
    if (front_b || pending) {
        kprintf("igdinfo: Puffer B wird gerade angezeigt, Vergleich uebersprungen\n");
        return 0;
    }

    /* Ein ganzes Bild aus einem RAM-Puffer (wie das Bild eines Programms) in den verdeckten Puffer B; je Weg das beste
     * von 3 Laeufen. B wird nicht angezeigt, der Inhalt ist egal (das naechste ganze Bild ueberschreibt ihn). */
    uint64_t size = (uint64_t)igd_scr_stride * igd_scr_h, pages = (size + 4095) / 4096;
    uint64_t src = pmm_alloc_frames(pages);
    if (!src) {
        kprintf("igdinfo: kein Speicher fuer den Vergleich\n");
        return 0;
    }
    uint32_t *sp = (uint32_t *)src;
    for (uint64_t i = 0; i < size / 4; i++)
        sp[i] = (uint32_t)i * 2654435761u;
    int opt = has_clflushopt();
    const char *names[] = {"memcpy ohne Zurueckschreiben (nicht anzeigbar)", "frueher: memcpy + clflush je Zeile",
                           "memcpy, danach clflushopt", "jetzt: Non-Temporal-Stores (movnti)"};
    for (int way = 0; way < 4; way++) {
        if (way == 2 && !opt) {
            kprintf("igdinfo:   %s: CPU kann kein clflushopt\n", names[way]);
            continue;
        }
        uint64_t best = ~0ULL;
        for (int run = 0; run < 3; run++) {
            igd_clflush((uint64_t)igd_buf_b, size); /* gleiche Ausgangslage: B nicht im Cache */
            uint64_t t0 = time_us();
            if (way == 0) {
                memcpy(igd_buf_b, sp, size);
            } else if (way == 1) {
                for (uint32_t yy = 0; yy < igd_scr_h; yy++) {
                    uint8_t *d = igd_buf_b + (uint64_t)yy * igd_scr_stride;
                    memcpy(d, sp + (uint64_t)yy * igd_scr_stride / 4, (uint64_t)igd_scr_w * 4);
                    igd_clflush((uint64_t)d, (uint64_t)igd_scr_w * 4);
                }
            } else if (way == 2) {
                memcpy(igd_buf_b, sp, size);
                flush_opt((uint64_t)igd_buf_b, size);
            } else {
                copy_rect(igd_buf_b, sp, igd_scr_stride / 4, 0, 0, (int)igd_scr_w, (int)igd_scr_h, 1);
            }
            uint64_t t = time_us() - t0;
            if (t < best)
                best = t;
        }
        kprintf("igdinfo:   %s: %lu us (%lu MB/s)\n", names[way], (unsigned long)best,
                (unsigned long)(size / (best ? best : 1)));
    }
    pmm_free_frames(src, pages);
    kprintf("igdinfo: zum Vergleich Blitter (igdtest blit, hoechster Takt): ganzes Bild kopieren ca. 4200 us\n");
    return 0;
}
