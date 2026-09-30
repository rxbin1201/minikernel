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
    if (info.scanout_pipe < 0)
        kprintf("igd: keine Pipe zeigt den GOP-Framebuffer (%#lx) - bitte dieses Log schicken\n",
                (unsigned long)bi->fb.base);
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
    if (!info.gen9 || !regs || !ggtt || p < 0) {
        kprintf("igdtest: keine passende Intel-GPU / kein erkannter Framebuffer\n");
        return -1;
    }
    kprintf("igdtest: Stufe 2 - Page-Flipping auf Pipe %c\n", 'A' + p);
    dump_ddb(p);

    uint32_t ctl = rd(PLANE_CTL(p)), size = rd(PLANE_SIZE(p));
    uint32_t w = (size & 0xFFF) + 1, h = ((size >> 16) & 0xFFF) + 1, stride = (rd(PLANE_STRIDE(p)) & 0x3FF) * 64;
    uint32_t old_surf = rd(PLANE_SURF(p)) & ~0xFFFu;
    if ((ctl & (7u << 10)) || ((ctl >> 24) & 0xF) != 4 || stride < w * 4) {
        kprintf("igdtest: Ebene ist nicht linear/32 Bit, Abbruch\n");
        return -2;
    }

    /* 1. Verstehen wir die GGTT? Der Eintrag der angezeigten Surface muss ins Stolen Memory zeigen. */
    uint64_t pte = ggtt[old_surf >> 12];
    kprintf("igdtest: GGTT[%#x] = %#lx, Stolen Memory @ %#lx\n", old_surf >> 12, (unsigned long)pte,
            (unsigned long)stolen_base);
    if (!(pte & PTE_VALID) || (pte & PTE_ADDR) < stolen_base || (pte & PTE_ADDR) >= stolen_base + stolen_size) {
        kprintf("igdtest: GGTT-Eintrag passt nicht zum Stolen Memory, Abbruch (nichts veraendert)\n");
        return -3;
    }

    /* 2. Freier GGTT-Bereich ab der Haelfte des Adressraums. Die Firmware traegt nur ein, was sie braucht (den
     * Framebuffer im Stolen Memory); der Rest der GGTT ist ungeloeschter Speicherinhalt, den niemand liest. Frei ist
     * ein Bereich deshalb, wenn keiner seiner Eintraege gueltig ins Stolen Memory zeigt. Die alten Werte werden
     * gesichert und am Ende genau so zurueckgeschrieben. */
    uint64_t bytes = (uint64_t)stride * h;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096), base = ggtt_entries / 2;
    if (base + pages > ggtt_entries) {
        kprintf("igdtest: GGTT zu klein\n");
        return -4;
    }
    uint32_t garbage = 0;
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t e = ggtt[base + i];
        if ((e & PTE_VALID) && (e & PTE_ADDR) >= stolen_base && (e & PTE_ADDR) < stolen_base + stolen_size) {
            kprintf("igdtest: GGTT-Eintrag %#x zeigt ins Stolen Memory (%#lx): wird benutzt, Abbruch\n", base + i,
                    (unsigned long)e);
            return -5;
        }
        if (e)
            garbage++;
    }
    kprintf("igdtest: GGTT-Eintraege %#x..%#x frei (%u davon mit altem Speicherinhalt, werden wiederhergestellt)\n",
            base, base + pages - 1, garbage);

    /* 3. Zweiter Bildpuffer im RAM: das aktuelle Bild mit invertierten Farben */
    uint64_t *frames = kmalloc(sizeof(uint64_t) * pages * 2); /* [0, pages): Seiten, [pages, 2 pages): alte GGTT */
    if (!frames)
        return -6;
    uint64_t *saved = frames + pages;
    for (uint32_t i = 0; i < pages; i++)
        saved[i] = ggtt[base + i];
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
    kprintf("igdtest: zurueck auf das Original: %s\n", f2 >= 0 ? "ok" : "NICHT bestaetigt");
    rc = f1 >= 0 && fails == 0 && f2 >= 0 ? 0 : -7;
    kprintf("igdtest: %s\n", rc == 0 ? "Page-Flipping funktioniert" : "Page-Flipping mit Fehlern, bitte Log schicken");

free_frames:
    for (uint32_t i = 0; i < got; i++)
        pmm_free_frame(frames[i]);
    kfree(frames);
    return rc;
}
