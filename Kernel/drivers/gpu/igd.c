/* Intel-Grafik Gen9 (siehe igd.h), Stufe 1: erkennen und den Zustand der Anzeige auslesen. Nur lesende Zugriffe. */

#include "drivers/gpu/igd.h"
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

static IgdInfo info = {.scanout_pipe = -1};
static volatile uint8_t  *regs;
static volatile uint64_t *ggtt;       /* globale Grafik-Seitentabelle (GGTT) */
static uint32_t           ggtt_entries;
static uint64_t           stolen_base, stolen_size;

static uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(regs + off);
}

static void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(regs + off) = v;
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

static void display_init(const BootInfo *bi);

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
    stolen_base = bdsm;
    stolen_size = (uint64_t)(gms < 0xF0 ? gms * 32 : 4 * (gms - 0xF0 + 1)) << 20;

    /* GGTT: Groesse aus GMCH_CTRL Bits 7:6 (1 = 2 MiB, 2 = 4 MiB, 3 = 8 MiB Eintraege), liegt in BAR0 ab 8 MiB */
    uint32_t ggms = (gmch >> 6) & 3;
    uint32_t ggtt_bytes = ggms ? (1u << ggms) << 20 : 0;
    if (ggtt_bytes && paging_map_mmio(info.mmio + GGTT_OFFSET, ggtt_bytes) == 0) {
        ggtt = (volatile uint64_t *)(info.mmio + GGTT_OFFSET);
        ggtt_entries = ggtt_bytes / 8;
        kprintf("igd: GGTT %u MiB (%u Eintraege, %u MiB Grafik-Adressraum), Eintrag 0 = %#lx\n", ggtt_bytes >> 20,
                ggtt_entries, ggtt_entries / 256, (unsigned long)ggtt[0]);
    }

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
    if (info.scanout_pipe < 0) {
        kprintf("igd: keine Pipe zeigt den GOP-Framebuffer (%#lx) - bitte dieses Log schicken\n",
                (unsigned long)bi->fb.base);
        return;
    }
    display_init(bi);
}

/* ---------- Stufe 2: Page-Flipping (nur auf Befehl: igdtest) ---------- */

static void clflush_range(uint64_t addr, uint64_t len)
{
    for (uint64_t a = addr & ~63ULL; a < addr + len; a += 64)
        __asm__ __volatile__("clflush (%0)" : : "r"(a) : "memory");
    __asm__ __volatile__("mfence" : : : "memory");
}

static void ggtt_flush(void)
{
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);
}

/* Wartet, bis die Ebene surf tatsaechlich anzeigt; Ergebnis: Bilder bis dahin, -1 = Zeitueberschreitung */
static int wait_live(int p, uint32_t surf)
{
    uint32_t f0 = rd(PIPE_FRMCOUNT(p));
    if (!WAIT_UNTIL((rd(PLANE_SURFLIVE(p)) & ~0xFFFu) == surf, 200))
        return -1;
    return (int)(rd(PIPE_FRMCOUNT(p)) - f0);
}

/* FIFO-Unterlauf der Pipe (der Display-Engine gingen die Daten aus: Flackern/Stoerstreifen) festhalten lassen. Nur
 * die Maske wird geoeffnet, IER bleibt, wie es ist: es entsteht kein Interrupt, das Ereignis steht nur im IIR. */
static uint32_t underrun_begin(int p)
{
    uint32_t imr = rd(DE_PIPE_IMR(p));
    wr(DE_PIPE_IIR(p), PIPE_FIFO_UNDERRUN);
    wr(DE_PIPE_IMR(p), imr & ~PIPE_FIFO_UNDERRUN);
    return imr;
}

static int underrun_end(int p, uint32_t imr)
{
    int seen = (rd(DE_PIPE_IIR(p)) & PIPE_FIFO_UNDERRUN) != 0;
    wr(DE_PIPE_IIR(p), PIPE_FIFO_UNDERRUN);
    wr(DE_PIPE_IMR(p), imr);
    return seen;
}

/* Prueft, ob der GGTT-Bereich [base, base + pages) frei ist, und sichert seine Eintraege nach saved. Die Firmware
 * traegt nur ein, was sie braucht (den Framebuffer im Stolen Memory); der Rest der GGTT ist ungeloeschter
 * Speicherinhalt, den niemand liest. Frei heisst deshalb: kein Eintrag zeigt gueltig ins Stolen Memory. */
static int ggtt_claim(uint32_t base, uint32_t pages, uint64_t *saved)
{
    if (base + pages > ggtt_entries) {
        kprintf("igdtest: GGTT zu klein\n");
        return -4;
    }
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t e = ggtt[base + i];
        if ((e & PTE_VALID) && (e & PTE_ADDR) >= stolen_base && (e & PTE_ADDR) < stolen_base + stolen_size) {
            kprintf("igdtest: GGTT-Eintrag %#x zeigt ins Stolen Memory (%#lx): wird benutzt, Abbruch\n", base + i,
                    (unsigned long)e);
            return -5;
        }
        saved[i] = e;
    }
    return 0;
}

/* Gemeinsame Vorpruefung: Gen9, Framebuffer erkannt, GGTT-Eintrag der angezeigten Surface im Stolen Memory */
static int preflight(const char *what)
{
    int p = info.scanout_pipe;
    if (!info.gen9 || !regs || !ggtt || p < 0) {
        kprintf("igdtest: keine passende Intel-GPU / kein erkannter Framebuffer\n");
        return -1;
    }
    kprintf("igdtest: %s auf Pipe %c\n", what, 'A' + p);
    uint32_t surf = rd(PLANE_SURF(p)) & ~0xFFFu;
    uint64_t pte = ggtt[surf >> 12];
    kprintf("igdtest: GGTT[%#x] = %#lx, Stolen Memory @ %#lx\n", surf >> 12, (unsigned long)pte,
            (unsigned long)stolen_base);
    if (!(pte & PTE_VALID) || (pte & PTE_ADDR) < stolen_base || (pte & PTE_ADDR) >= stolen_base + stolen_size) {
        kprintf("igdtest: GGTT-Eintrag passt nicht zum Stolen Memory, Abbruch (nichts veraendert)\n");
        return -3;
    }
    uint32_t imr = underrun_begin(p); /* laeuft die Anzeige schon vorher sauber? */
    thread_sleep_ms(200);
    kprintf("igdtest: vorher (200 ms): %s\n", underrun_end(p, imr) ? "FIFO-Unterlauf! (schon vor dem Test)" :
                                                                    "kein FIFO-Unterlauf");
    return 0;
}

/* Nur lesen: Aufteilung des Display-Puffers und Watermarks (Voraussetzung fuer den Hardware-Mauszeiger) */
static void dump_ddb(int p)
{
    uint32_t pb = rd(PLANE_BUF_CFG(p)), cb = rd(CUR_BUF_CFG(p));
    kprintf("igd: DDB: DBUF_CTL %#x, Ebene 1 Bloecke %u-%u, Mauszeiger Bloecke %u-%u\n", rd(DBUF_CTL), pb & 0x3FF,
            (pb >> 16) & 0x3FF, cb & 0x3FF, (cb >> 16) & 0x3FF);
    for (int lvl = 0; lvl < 8; lvl++) {
        uint32_t pw = rd(PLANE_WM(p, lvl)), cw = rd(CUR_WM(p, lvl));
        kprintf("igd:   WM%d: Ebene 1 %s %u Zeilen %u Bloecke | Mauszeiger %s %u Zeilen %u Bloecke\n", lvl,
                (pw >> 31) ? "an " : "aus", (pw >> 14) & 0x1F, pw & 0x3FF, (cw >> 31) ? "an " : "aus",
                (cw >> 14) & 0x1F, cw & 0x3FF);
    }
}

int igd_flip_test(void)
{
    int p = info.scanout_pipe;
    int pre = preflight("Stufe 2 - Page-Flipping");
    if (pre)
        return pre;
    dump_ddb(p);

    uint32_t ctl = rd(PLANE_CTL(p)), size = rd(PLANE_SIZE(p));
    uint32_t w = (size & 0xFFF) + 1, h = ((size >> 16) & 0xFFF) + 1, stride = (rd(PLANE_STRIDE(p)) & 0x3FF) * 64;
    uint32_t old_surf = rd(PLANE_SURF(p)) & ~0xFFFu;
    if ((ctl & (7u << 10)) || ((ctl >> 24) & 0xF) != 4 || stride < w * 4) {
        kprintf("igdtest: Ebene ist nicht linear/32 Bit, Abbruch\n");
        return -2;
    }

    /* 2. Freier GGTT-Bereich ab der Haelfte des Adressraums; alte Eintraege sichern */
    uint64_t bytes = (uint64_t)stride * h;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096), base = ggtt_entries / 2;
    uint64_t *frames = kmalloc(sizeof(uint64_t) * pages * 2); /* [0, pages): Seiten, [pages, 2 pages): alte GGTT */
    if (!frames)
        return -6;
    uint64_t *saved = frames + pages;
    int claim = ggtt_claim(base, pages, saved);
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
        clflush_range(frames[i], 4096);

    /* 4. GGTT-Eintraege setzen, dann umschalten */
    for (uint32_t i = 0; i < pages; i++)
        ggtt[base + i] = frames[i] | PTE_VALID;
    ggtt_flush();
    uint32_t new_surf = base << 12;
    uint32_t imr = underrun_begin(p);
    uint64_t t0 = time_ms();
    wr(PLANE_SURF(p), new_surf);
    int f1 = wait_live(p, new_surf);
    kprintf("igdtest: Flip auf %#x: %s nach %d Bild(ern), %lu ms\n", new_surf, f1 >= 0 ? "angezeigt" : "NICHT angezeigt",
            f1, (unsigned long)(time_ms() - t0));
    thread_sleep_ms(2000); /* invertiertes Bild stehen lassen */

    /* 5. Zehn Wechsel hin und her: jeder sollte nach hoechstens einem Bild sichtbar sein */
    int worst = 0, fails = 0;
    for (int i = 0; i < 10; i++) {
        uint32_t s = (i & 1) ? new_surf : old_surf;
        wr(PLANE_SURF(p), s);
        int f = wait_live(p, s);
        if (f < 0)
            fails++;
        else if (f > worst)
            worst = f;
        thread_sleep_ms(100);
    }
    kprintf("igdtest: 10 Wechsel: %d ohne Anzeige, laengstens %d Bild(er) bis sichtbar\n", fails, worst);

    /* 6. Zurueck zum Original, GGTT wiederherstellen */
    wr(PLANE_SURF(p), old_surf);
    int f2 = wait_live(p, old_surf);
    for (uint32_t i = 0; i < pages; i++)
        ggtt[base + i] = saved[i];
    ggtt_flush();
    int underrun = underrun_end(p, imr);
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
    int p = info.scanout_pipe;
    int pre = preflight("Stufe 2 - Hardware-Mauszeiger");
    if (pre)
        return pre;
    dump_ddb(p);

    /* Platz im Display-Puffer: direkt hinter der Bildebene 32 Bloecke (so viel gibt Linux dem Zeiger bei einer Pipe) */
    uint32_t plane_end = (rd(PLANE_BUF_CFG(p)) >> 16) & 0x3FF;
    uint32_t start = plane_end + 1, end = start + 31;
    if (end >= DDB_BLOCKS) {
        kprintf("igdtest: kein Platz im Display-Puffer hinter der Bildebene (%u), Abbruch\n", plane_end);
        return -8;
    }

    /* 64x64 ARGB = 16 KiB = 4 Seiten, in der GGTT oberhalb des Flip-Bereichs */
    uint32_t base = ggtt_entries / 2 + 0x20000, pages = 4;
    uint64_t frames[4], saved[4];
    int claim = ggtt_claim(base, pages, saved);
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
        clflush_range(frames[i], 4096);
        ggtt[base + i] = frames[i] | PTE_VALID;
    }
    ggtt_flush();

    /* Register sichern, Zeiger einschalten. Alle Zeiger-Register sind doppelt gepuffert: erst das Schreiben von
     * CUR_BASE uebernimmt sie beim naechsten Bildwechsel. */
    uint32_t s_ctl = rd(CUR_CTL(p)), s_base = rd(CUR_BASE(p)), s_pos = rd(CUR_POS(p)), s_buf = rd(CUR_BUF_CFG(p));
    uint32_t s_wm0 = rd(CUR_WM(p, 0)), surf = base << 12;
    int cx = (int)console_width_px() / 2, cy = (int)console_height_px() / 2, x = 300, y = 0;
    uint32_t imr = underrun_begin(p);
    wr(CUR_BUF_CFG(p), (end << 16) | start);
    wr(CUR_WM(p, 0), (1u << 31) | (1u << 14) | 8); /* Stufe 0 an: 1 Zeile, 8 Bloecke (wie die Firmware fuer Ebene 1) */
    wr(CUR_CTL(p), 0x27);                          /* 64x64, 32 Bit ARGB */
    wr(CUR_POS(p), cur_pos(cx + x, cy + y));
    wr(CUR_BASE(p), surf);
    uint32_t f0 = rd(PIPE_FRMCOUNT(p));
    int live = WAIT_UNTIL((rd(CUR_SURFLIVE(p)) & ~0xFFFu) == surf, 200);
    kprintf("igdtest: Zeiger an (DDB %u-%u): %s nach %u Bild(ern)\n", start, end, live ? "angezeigt" : "NICHT angezeigt",
            rd(PIPE_FRMCOUNT(p)) - f0);

    /* 3 Sekunden im Kreis (Minskys Kreis-Algorithmus: nur ganze Zahlen) */
    for (int step = 0; step < 150; step++) {
        x -= y / 16;
        y += x / 16;
        wr(CUR_POS(p), cur_pos(cx + x, cy + y));
        wr(CUR_BASE(p), surf); /* uebernehmen */
        thread_sleep_ms(20);
    }

    /* Aus und alles zurueck */
    wr(CUR_CTL(p), s_ctl);
    wr(CUR_POS(p), s_pos);
    wr(CUR_BASE(p), s_base);
    thread_sleep_ms(50);
    wr(CUR_WM(p, 0), s_wm0);
    wr(CUR_BUF_CFG(p), s_buf);
    wr(CUR_BASE(p), s_base);
    thread_sleep_ms(50);
    int underrun = underrun_end(p, imr);
    for (uint32_t i = 0; i < pages; i++)
        ggtt[base + i] = saved[i];
    ggtt_flush();
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
 * beschrieben wird: das Programm laeuft so von selbst im Takt der Bildrate.
 * "noigd" in der Kommandozeile schaltet beides ab (dann wie vorher alles in Software). */

static int       hw_cursor;           /* Zeiger-Ebene eingerichtet */
static uint32_t  cursor_surf;
static int       cursor_on = -1;
static int       flip_ready;          /* Puffer B eingerichtet */
static uint32_t  scr_w, scr_h, scr_stride;
static uint32_t  surf_a, surf_b;
static uint8_t  *buf_a, *buf_b;       /* CPU-Adressen: A ueber die Aperture (write-combining), B im RAM */
static int       front_b;             /* 1: B wird angezeigt (oder der Wechsel dorthin steht an) */
static uint32_t  pending;             /* Surface, deren Wechsel noch nicht bestaetigt ist, 0 = keiner */

static void draw_cursor_image(uint64_t frames[4])
{
    uint32_t sc = console_scale();
    if (sc < 1)
        sc = 1;
    if (sc > 3) /* 12x19 Punkte, 64x64 Pixel Platz */
        sc = 3;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t px = 0;
            if (y / sc < 19 && x / sc < 12) {
                char c = arrow[y / sc][x / sc];
                px = c == 'X' ? 0xFF000000u : c == '.' ? 0xFFFFFFFFu : 0;
            }
            uint64_t off = (uint64_t)y * 256 + (uint64_t)x * 4;
            *(uint32_t *)(frames[off >> 12] + (off & 4095)) = px;
        }
    for (int i = 0; i < 4; i++)
        clflush_range(frames[i], 4096);
}

static int setup_cursor(int p)
{
    uint32_t plane_end = (rd(PLANE_BUF_CFG(p)) >> 16) & 0x3FF, start = plane_end + 1, end = start + 31;
    uint32_t base = ggtt_entries / 2 + 0x30000; /* nicht dort, wo igdtest seine Probe-Bereiche hat */
    uint64_t saved[4], frames[4];
    if (end >= DDB_BLOCKS || ggtt_claim(base, 4, saved) != 0)
        return -1;
    for (int i = 0; i < 4; i++)
        if (!(frames[i] = pmm_alloc_frame())) {
            while (i--)
                pmm_free_frame(frames[i]);
            return -1;
        }
    draw_cursor_image(frames);
    for (int i = 0; i < 4; i++)
        ggtt[base + i] = frames[i] | PTE_VALID;
    ggtt_flush();
    cursor_surf = base << 12;
    wr(CUR_BUF_CFG(p), (end << 16) | start);
    wr(CUR_WM(p, 0), (1u << 31) | (1u << 14) | 8);
    wr(CUR_CTL(p), 0);            /* erst sichtbar, wenn eine Maus da ist */
    wr(CUR_BASE(p), cursor_surf);
    hw_cursor = 1;
    cursor_on = 0;
    return 0;
}

static int setup_flip(int p)
{
    uint32_t pages = (uint32_t)(((uint64_t)scr_stride * scr_h + 4095) / 4096);
    uint32_t base = ggtt_entries / 4; /* 1 GiB: getrennt von igdtest (ab 2 GiB) */
    uint64_t *saved = kmalloc(sizeof(uint64_t) * pages);
    if (!saved)
        return -1;
    int claim = ggtt_claim(base, pages, saved);
    kfree(saved); /* die Eintraege gehoeren ab jetzt dauerhaft uns */
    if (claim)
        return -1;
    uint64_t b = pmm_alloc_frames(pages); /* am Stueck: dann ist jede Zeile zusammenhaengend */
    if (!b)
        return -1;
    for (uint32_t i = 0; i < pages; i++)
        ggtt[base + i] = (b + (uint64_t)i * 4096) | PTE_VALID;
    ggtt_flush();
    buf_b = (uint8_t *)b;
    surf_b = base << 12;
    (void)p;
    flip_ready = 1;
    return 0;
}

static void display_init(const BootInfo *bi)
{
    if (cmdline_has("noigd")) {
        kprintf("igd: 'noigd': Mauszeiger und Doppelpufferung bleiben in Software\n");
        return;
    }
    int p = info.scanout_pipe;
    uint32_t ctl = rd(PLANE_CTL(p)), size = rd(PLANE_SIZE(p));
    scr_w = (size & 0xFFF) + 1;
    scr_h = ((size >> 16) & 0xFFF) + 1;
    scr_stride = (rd(PLANE_STRIDE(p)) & 0x3FF) * 64;
    if ((ctl & (7u << 10)) || ((ctl >> 24) & 0xF) != 4 || scr_stride < scr_w * 4) {
        kprintf("igd: Ebene nicht linear/32 Bit: Mauszeiger und Doppelpufferung bleiben in Software\n");
        return;
    }
    surf_a = info.scanout_surf;
    buf_a = (uint8_t *)bi->fb.base;
    int c = setup_cursor(p), f = setup_flip(p);
    kprintf("igd: Hardware-Mauszeiger %s, Doppelpufferung %s (zweiter Puffer %u KiB im RAM)\n", c == 0 ? "an" : "AUS",
            f == 0 ? "an" : "AUS", (uint32_t)((uint64_t)scr_stride * scr_h / 1024));
}

int igd_cursor_available(void)
{
    return hw_cursor;
}

void igd_cursor_move(int x, int y, int visible)
{
    if (!hw_cursor)
        return;
    int p = info.scanout_pipe;
    if (visible != cursor_on) {
        wr(CUR_CTL(p), visible ? 0x27 : 0);
        cursor_on = visible;
    }
    wr(CUR_POS(p), cur_pos(x, y));
    wr(CUR_BASE(p), cursor_surf); /* uebernimmt Position/Sichtbarkeit beim naechsten Bildwechsel */
}

/* Wartet, bis der zuletzt angestossene Wechsel angezeigt wird (danach wird der andere Puffer nicht mehr gelesen) */
static void wait_flip(void)
{
    if (!pending)
        return;
    int p = info.scanout_pipe;
    for (int i = 0; i < 20 && (rd(PLANE_SURFLIVE(p)) & ~0xFFFu) != pending; i++)
        thread_sleep_ms(2); /* hoechstens ein Bild (20 ms bei 50 Hz); schlafend, damit andere CPUs weiterkommen */
    pending = 0;
}

static void copy_rect(uint8_t *dst, const uint32_t *src, uint32_t pitch, int x, int y, int w, int h, int flush)
{
    for (int yy = 0; yy < h; yy++) {
        uint8_t *d = dst + (uint64_t)(y + yy) * scr_stride + (uint64_t)x * 4;
        memcpy(d, src + (uint64_t)yy * pitch, (uint64_t)w * 4);
        if (flush) /* RAM-Puffer B: die Display-Engine liest am CPU-Cache vorbei */
            clflush_range((uint64_t)d, (uint64_t)w * 4);
    }
    __asm__ __volatile__("sfence" : : : "memory"); /* write-combining-Puffer leeren (A) */
}

int igd_gfx_blit(const uint32_t *src, uint32_t pitch, int x, int y, int w, int h)
{
    if (!flip_ready || x < 0 || y < 0 || (uint32_t)(x + w) > scr_w || (uint32_t)(y + h) > scr_h)
        return 0;
    if (x == 0 && y == 0 && (uint32_t)w == scr_w && (uint32_t)h == scr_h) { /* ganzes Bild: in den Hintergrund, umschalten */
        wait_flip();
        int to_b = !front_b;
        copy_rect(to_b ? buf_b : buf_a, src, pitch, 0, 0, w, h, to_b);
        pending = to_b ? surf_b : surf_a;
        wr(PLANE_SURF(info.scanout_pipe), pending);
        front_b = to_b;
        return 1;
    }
    copy_rect(front_b ? buf_b : buf_a, src, pitch, x, y, w, h, front_b); /* Teil-Update: in den angezeigten Puffer */
    return 1;
}

void igd_gfx_end(void)
{
    if (!flip_ready)
        return;
    wait_flip();
    if (front_b) { /* die Konsole zeichnet in A */
        pending = surf_a;
        wr(PLANE_SURF(info.scanout_pipe), surf_a);
        wait_flip();
        front_b = 0;
    }
}
