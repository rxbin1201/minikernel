/* Intel High Definition Audio (siehe hda.h).
 *
 * Aufbau: der Controller (PCI) spricht ueber eine serielle Verbindung mit bis zu 15 Codecs (z.B. ein Realtek-Chip
 * fuer Klinke/Lautsprecher, dazu der HDMI-Codec der Grafik). Befehle ("Verbs") gehen ueber einen Ringpuffer im RAM
 * (CORB) an die Codecs, Antworten kommen in einen zweiten (RIRB). Ein Codec besteht aus Knoten ("Widgets"): Wandler
 * (DAC), Mischer, Auswahlschalter und Anschluesse ("Pins"). Fuer die Wiedergabe sucht der Treiber von jedem analogen
 * Ausgangs-Pin (Kopfhoerer, Line-Out, Lautsprecher) einen Weg zu einem DAC, schaltet ihn durch und stellt die
 * Verstaerker ein. Die Tondaten liest der Controller per DMA aus einem Ringpuffer (Liste von Pufferstuecken, BDL)
 * und schickt sie mit einer Stream-Nummer an die DACs, die auf diese Nummer eingestellt sind.
 * Grundlage: Intel "High Definition Audio Specification" Rev. 1.0a und der Linux-Treiber (snd-hda-intel). */

#include "drivers/sound/hda.h"
#include "drivers/bt/bt.h"
#include "drivers/pci.h"
#include "arch/x86_64/apic.h"
#include "core/sched.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/paging.h"
#include "mm/pmm.h"

/* ---------- Controller-Register ---------- */

#define GCAP      0x00 /* 16: Anzahl Streams: OSS 15:12, ISS 11:8 */
#define GCTL      0x08 /* 32: Bit 0 CRST (0 = Reset) */
#define STATESTS  0x0E /* 16: welche Codecs sich gemeldet haben */
#define INTCTL    0x20
#define SSYNC     0x38
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48 /* 16 */
#define CORBRP    0x4A /* 16, Bit 15 = Reset */
#define CORBCTL   0x4C /* 8, Bit 1 = laeuft */
#define CORBSIZE  0x4E /* 8 */
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58 /* 16, Bit 15 = Reset */
#define RINTCNT   0x5A /* 16 */
#define RIRBCTL   0x5C /* 8, Bit 1 = DMA an */
#define RIRBSTS   0x5D
#define RIRBSIZE  0x5E

/* Stream-Deskriptor (je 0x20 Byte ab 0x80; erst die Eingabe-, dann die Ausgabe-Streams) */
#define SD_CTL   0x00 /* 24 Bit: 0 SRST, 1 RUN, 2 IOCE; Bits 23:20 Stream-Nummer */
#define SD_STS   0x03
#define SD_LPIB  0x04 /* Position im Ringpuffer (Bytes) */
#define SD_CBL   0x08 /* Laenge des Ringpuffers */
#define SD_LVI   0x0C /* 16: letzter Eintrag der BDL */
#define SD_FMT   0x12 /* 16: Format */
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

/* ---------- Verbs ---------- */

#define V_GET_PARAM     0xF00
#define V_GET_CONN      0xF02
#define V_SET_CONN_SEL  0x701
#define V_SET_POWER     0x705
#define V_SET_STREAM    0x706 /* Stream-Nummer << 4 | erster Kanal */
#define V_SET_PIN_CTL   0x707
#define V_GET_PIN_SENSE 0xF09
#define V_SET_EAPD      0x70C
#define V_GET_CFG       0xF1C
#define V_SET_FORMAT    0x2   /* 4-Bit-Verb, 16 Bit Nutzdaten */
#define V_SET_AMP       0x3   /* 4-Bit-Verb: 15 Ausgang, 14 Eingang, 13 links, 12 rechts, 11:8 Index, 7 stumm, 6:0 Pegel */

#define P_VENDOR     0x00
#define P_NODES      0x04 /* erster Knoten << 16 | Anzahl */
#define P_FG_TYPE    0x05 /* 1 = Audio-Funktionsgruppe */
#define P_WCAPS      0x09 /* Knotentyp 23:20; 0 Stereo, 1 Eingangsverst., 2 Ausgangsverst., 3 eigene Verst.-Werte,
                             8 Verbindungsliste, 9 digital, 10 Stromsteuerung */
#define P_PCM        0x0A /* unterstuetzte Raten (Bits 0-11) und Breiten */
#define P_PINCAPS    0x0C /* 4 Ausgang, 3 Kopfhoerertreiber, 16 EAPD */
#define P_IN_AMP     0x0D
#define P_CONN_LEN   0x0E
#define P_OUT_AMP    0x12 /* Schritte 14:8, 0-dB-Stufe 6:0 */

#define W_OUT   0
#define W_IN    1
#define W_MIXER 2
#define W_SEL   3
#define W_PIN   4

/* ---------- Zustand ---------- */

#define MAX_NODES   64
#define MAX_PATHS   6
#define RING_BYTES  (128u * 1024)      /* Tonpuffer: 4 Stuecke zu 32 KiB (bei 48 kHz Stereo ca. 0,7 s) */
#define BDL_ENTRIES 4
#define STREAM_TAG  1

typedef struct {
    uint8_t  type, nconn;
    uint32_t caps, pincaps, cfg;
    uint8_t  conn[16];
} Node;

typedef struct {
    int     cad;
    uint8_t node[6], sel[6]; /* Weg vom Pin (node[0]) zum DAC (node[n-1]); sel = Eingang, der zum Naechsten fuehrt */
    int     n;
    uint8_t dev;             /* Art des Pins: 0 Line-Out, 1 Lautsprecher, 2 Kopfhoerer */
    uint8_t jack;            /* Buchse nach aussen, die meldet, ob etwas eingesteckt ist */
    uint8_t pinctl;          /* Pin-Steuerung, wenn der Ausgang an ist */
    uint8_t muted;           /* Pin abgeschaltet (Auto-Mute oder anderer Ausgang gewaehlt) */
    uint8_t plugged;         /* an dieser Buchse steckt etwas (zuletzt abgefragt) */
} Path;

static volatile uint8_t *regs;
static PciDevice         pci;
static int               present;
static uint32_t         *corb;
static volatile uint64_t *rirb;
static uint16_t          corb_wp, rirb_rp;
static uint32_t          sd;          /* Offset des ersten Ausgabe-Streams */
static Path              paths[MAX_PATHS];
static int               npaths;
static uint32_t          rates_ok;    /* P_PCM des ersten DACs */
static uint8_t          *ring;
static uint64_t          ring_phys;
static int               running;
static uint64_t          written, played;
static uint32_t          last_lpib;
static uint64_t          last_poll_us;
static uint32_t          bytes_per_sec;
static int               volume = 80;
static Node              nodes[16][MAX_NODES]; /* je Codec, Index = Knotennummer - erster Knoten */
static uint8_t           node_first[16], node_count[16];
static uint32_t          afg_out_amp[16];     /* Verstaerker-Werte der Funktionsgruppe (gelten ohne "Override") */

static inline uint8_t  rd8(uint32_t o)  { return *(volatile uint8_t *)(regs + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(regs + o); }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static inline void wr8(uint32_t o, uint8_t v)   { *(volatile uint8_t *)(regs + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(regs + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }

static void delay_us(uint64_t us)
{
    uint64_t end = time_us() + us;
    while (time_us() < end)
        ;
}

static int wait32(uint32_t o, uint32_t mask, uint32_t want, int ms)
{
    uint64_t end = time_us() + (uint64_t)ms * 1000;
    while ((rd32(o) & mask) != want)
        if (time_us() > end)
            return -1;
    return 0;
}

/* ---------- Befehle an die Codecs (CORB/RIRB) ---------- */

/* verb: 12-Bit-Verb << 8 | 8 Bit Daten, oder 4-Bit-Verb << 16 | 16 Bit Daten. -1 = keine Antwort */
static int64_t cmd(int cad, int nid, uint32_t verb)
{
    corb_wp = (uint16_t)((corb_wp + 1) & 0xFF);
    corb[corb_wp] = (uint32_t)cad << 28 | (uint32_t)nid << 20 | (verb & 0xFFFFF);
    __asm__ __volatile__("mfence" : : : "memory");
    wr16(CORBWP, corb_wp);
    uint64_t end = time_us() + 20000;
    while (time_us() < end) {
        while ((rd16(RIRBWP) & 0xFF) != rirb_rp) {
            rirb_rp = (uint16_t)((rirb_rp + 1) & 0xFF);
            uint64_t e = rirb[rirb_rp];
            wr8(RIRBSTS, 0x05); /* Antwort-Zaehler freigeben: sonst stellt der Controller nach RINTCNT Antworten ein */
            if ((e >> 32) & 0x10) /* unaufgeforderte Meldung (z.B. Stecker eingesteckt): hier egal */
                continue;
            return (int64_t)(uint32_t)e;
        }
    }
    return -1;
}

static uint32_t param(int cad, int nid, int p)
{
    int64_t r = cmd(cad, nid, V_GET_PARAM << 8 | (uint32_t)p);
    return r < 0 ? 0 : (uint32_t)r;
}

static int setup_rings(void)
{
    uint64_t c = pmm_alloc_frame(), r = pmm_alloc_frame();
    if (!c || !r)
        return -1;
    corb = (uint32_t *)c;
    rirb = (volatile uint64_t *)r;
    memset(corb, 0, 4096);
    memset((void *)rirb, 0, 4096);

    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    wait32(CORBCTL, 2, 0, 10);
    wr32(CORBLBASE, (uint32_t)c);
    wr32(CORBUBASE, (uint32_t)(c >> 32));
    wr8(CORBSIZE, 2); /* 256 Eintraege */
    wr16(CORBRP, 0x8000);
    WAIT_UNTIL(rd16(CORBRP) & 0x8000, 5); /* manche Controller melden das Bit nicht zurueck */
    wr16(CORBRP, 0);
    WAIT_UNTIL(!(rd16(CORBRP) & 0x8000), 5);
    wr16(CORBWP, 0);
    corb_wp = 0;

    wr32(RIRBLBASE, (uint32_t)r);
    wr32(RIRBUBASE, (uint32_t)(r >> 32));
    wr8(RIRBSIZE, 2);
    wr16(RIRBWP, 0x8000);
    wr16(RINTCNT, 1);
    rirb_rp = 0;
    wr8(CORBCTL, 2);
    wr8(RIRBCTL, 3); /* DMA an, Antwort-Meldung an (QEMU setzt sonst RIRBSTS nie und stellt nach RINTCNT Antworten ein);
                        ohne INTCTL.GIE entsteht daraus kein Interrupt */
    return 0;
}

/* ---------- Codec erkunden ---------- */

static Node *node(int cad, int nid)
{
    int i = nid - node_first[cad];
    return i >= 0 && i < node_count[cad] ? &nodes[cad][i] : 0;
}

static void read_conns(int cad, int nid, Node *n)
{
    uint32_t len = param(cad, nid, P_CONN_LEN);
    int longform = (len >> 7) & 1, count = (int)(len & 0x7F);
    n->nconn = 0;
    for (int i = 0; i < count && n->nconn < 16; i += longform ? 2 : 4) {
        int64_t r = cmd(cad, nid, V_GET_CONN << 8 | (uint32_t)i);
        if (r < 0)
            break;
        for (int k = 0; k < (longform ? 2 : 4) && i + k < count && n->nconn < 16; k++) {
            uint32_t e = longform ? ((uint32_t)r >> (16 * k)) & 0xFFFF : ((uint32_t)r >> (8 * k)) & 0xFF;
            n->conn[n->nconn++] = (uint8_t)(e & (longform ? 0x7FFF : 0x7F)); /* Bereiche (Bit 7/15) selten: nicht aufgeloest */
        }
    }
}

/* Tiefensuche vom Pin zu einem analogen DAC */
static int find_path(int cad, int nid, Path *p, int depth)
{
    Node *n = node(cad, nid);
    if (!n || depth >= 6)
        return 0;
    for (int i = 0; i < depth; i++)
        if (p->node[i] == nid)
            return 0; /* Schleife */
    p->node[depth] = (uint8_t)nid;
    if (n->type == W_OUT && !((n->caps >> 9) & 1)) {
        p->n = depth + 1;
        return 1;
    }
    if (n->type != W_PIN && n->type != W_MIXER && n->type != W_SEL && depth > 0)
        return 0;
    for (int i = 0; i < n->nconn; i++) {
        p->sel[depth] = (uint8_t)i;
        if (find_path(cad, n->conn[i], p, depth + 1))
            return 1;
    }
    return 0;
}

static const char *dev_name(int d)
{
    return d == 0 ? "Line-Out" : d == 1 ? "Lautsprecher" : d == 2 ? "Kopfhoerer" : "?";
}

static void probe_codec(int cad)
{
    uint32_t vendor = param(cad, 0, P_VENDOR);
    uint32_t fg = param(cad, 0, P_NODES);
    int afg = -1;
    for (uint32_t i = 0; i < (fg & 0xFF); i++) {
        int nid = (int)((fg >> 16) & 0xFF) + (int)i;
        if ((param(cad, nid, P_FG_TYPE) & 0xFF) == 1)
            afg = nid;
    }
    kprintf("hda: Codec %d: %04x:%04x%s\n", cad, vendor >> 16, vendor & 0xFFFF, afg < 0 ? " (keine Audio-Funktion)" : "");
    if (afg < 0)
        return;
    cmd(cad, afg, V_SET_POWER << 8 | 0); /* Funktionsgruppe an (D0) */
    delay_us(10000);
    uint32_t sub = param(cad, afg, P_NODES);
    node_first[cad] = (uint8_t)((sub >> 16) & 0xFF);
    node_count[cad] = (uint8_t)((sub & 0xFF) > MAX_NODES ? MAX_NODES : (sub & 0xFF));
    uint32_t afg_pcm = param(cad, afg, P_PCM);
    uint32_t afg_in_amp = param(cad, afg, P_IN_AMP);
    afg_out_amp[cad] = param(cad, afg, P_OUT_AMP);

    for (int i = 0; i < node_count[cad]; i++) {
        int nid = node_first[cad] + i;
        Node *n = &nodes[cad][i];
        n->caps = param(cad, nid, P_WCAPS);
        n->type = (uint8_t)((n->caps >> 20) & 0xF);
        if (n->caps & (1u << 8))
            read_conns(cad, nid, n);
        if (n->type == W_PIN) {
            n->pincaps = param(cad, nid, P_PINCAPS);
            int64_t c = cmd(cad, nid, V_GET_CFG << 8);
            n->cfg = c < 0 ? 0 : (uint32_t)c;
        }
    }

    for (int i = 0; i < node_count[cad] && npaths < MAX_PATHS; i++) {
        Node *n = &nodes[cad][i];
        int nid = node_first[cad] + i;
        uint32_t dev = (n->cfg >> 20) & 0xF, conn = (n->cfg >> 30) & 3;
        if (n->type != W_PIN || !(n->pincaps & (1u << 4)) || ((n->caps >> 9) & 1) || conn == 1 || dev > 2)
            continue;
        Path *p = &paths[npaths];
        memset(p, 0, sizeof(*p));
        p->cad = cad;
        p->dev = (uint8_t)dev;
        if (!find_path(cad, nid, p, 0))
            continue;
        npaths++;

        /* Weg durchschalten: Auswahl, Verstaerker (0 dB, nicht stumm), Pin an, EAPD, Strom */
        for (int k = 0; k < p->n; k++) {
            int id = p->node[k];
            Node *w = node(cad, id);
            if (w->caps & (1u << 10))
                cmd(cad, id, V_SET_POWER << 8 | 0);
            if ((w->type == W_SEL || w->type == W_PIN) && w->nconn > 1 && k + 1 < p->n)
                cmd(cad, id, V_SET_CONN_SEL << 8 | p->sel[k]);
            if ((w->caps & 2) && k + 1 < p->n && w->type == W_MIXER) { /* Mischer: den benutzten Eingang aufmachen */
                uint32_t ia = (w->caps & 8) ? param(cad, id, P_IN_AMP) : afg_in_amp;
                cmd(cad, id, V_SET_AMP << 16 | 0x7000 | (uint32_t)p->sel[k] << 8 | (ia & 0x7F));
            }
            if (w->caps & 4) { /* Ausgangsverstaerker: 0 dB (beim DAC regelt hda_volume) */
                uint32_t oa = (w->caps & 8) ? param(cad, id, P_OUT_AMP) : afg_out_amp[cad];
                cmd(cad, id, V_SET_AMP << 16 | 0xB000 | (oa & 0x7F));
            }
        }
        Node *pin = node(cad, p->node[0]);
        p->pinctl = (uint8_t)(0x40 | (p->dev == 2 && (pin->pincaps & 8) ? 0x80 : 0));
        p->jack = p->dev != 1 && ((pin->cfg >> 30) & 3) != 2 && (pin->pincaps & 4) && !((pin->cfg >> 8) & 1);
        /* Buchse: nicht fest eingebaut (Anschluss 2), kann Stecker erkennen, und die Firmware verbietet es nicht
         * (Default Config Misc Bit 0 = "Jack Detect Override") */
        cmd(cad, p->node[0], V_SET_PIN_CTL << 8 | p->pinctl);
        if (pin->pincaps & (1u << 16))
            cmd(cad, p->node[0], V_SET_EAPD << 8 | 0x02);
        int dac = p->node[p->n - 1];
        if (!rates_ok) {
            Node *d = node(cad, dac);
            rates_ok = (d->caps & 8) ? param(cad, dac, P_PCM) : afg_pcm; /* eigene Werte nur mit "Override" */
            if (!(rates_ok & 0xFFF))
                rates_ok = afg_pcm;
        }
        int64_t sense = (pin->pincaps & 4) ? cmd(cad, p->node[0], V_GET_PIN_SENSE << 8) : -1;
        kprintf("hda:   %s an Pin %#x%s -> DAC %#x (Weg:", dev_name(p->dev), p->node[0],
                sense < 0 ? "" : (sense & 0x80000000) ? " (eingesteckt)" : " (nichts eingesteckt)", dac);
        for (int k = 0; k < p->n; k++)
            kprintf(" %#x", p->node[k]);
        kprintf(")\n");
    }
}

/* ---------- Wiedergabe ---------- */

static uint16_t hda_format(uint32_t rate, uint32_t channels, int *ok)
{
    static const struct {
        uint32_t rate;
        uint16_t fmt;
        int      bit; /* Bit in P_PCM */
    } t[] = {
        {8000, 0x0500, 0},  {11025, 0x4300, 1}, {16000, 0x0200, 2}, {22050, 0x4100, 3}, {32000, 0x0A00, 4},
        {44100, 0x4000, 5}, {48000, 0x0000, 6}, {88200, 0x4800, 7}, {96000, 0x0800, 8},
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (t[i].rate == rate) {
            *ok = (rates_ok >> t[i].bit) & 1;
            return (uint16_t)(t[i].fmt | 1u << 4 | (channels - 1)); /* 16 Bit */
        }
    *ok = 0;
    return 0;
}

static void set_dac_volume(void)
{
    for (int i = 0; i < npaths; i++) {
        Path *p = &paths[i];
        int dac = p->node[p->n - 1];
        Node *d = node(p->cad, dac);
        if (!(d->caps & 4))
            continue;
        uint32_t oa = (d->caps & 8) ? param(p->cad, dac, P_OUT_AMP) : afg_out_amp[p->cad]; /* sonst Werte der Gruppe */
        uint32_t zero_db = oa & 0x7F, gain = zero_db * (uint32_t)volume / 100;
        uint32_t mute = volume == 0 ? 0x80 : 0;
        cmd(p->cad, dac, V_SET_AMP << 16 | 0xB000 | mute | gain);
    }
}

static void stream_stop(void)
{
    wr32(sd + SD_CTL, rd32(sd + SD_CTL) & ~2u & 0x00FFFFFFu);
    wait32(sd + SD_CTL, 2, 0, 20);
    running = 0;
}

/* DACs vom Stream loesen (Stream-Nummer 0), wie Linux beim Schliessen: das naechste Format wird so sauber uebernommen */
static void dacs_release(void)
{
    for (int i = 0; i < npaths; i++)
        cmd(paths[i].cad, paths[i].node[paths[i].n - 1], V_SET_STREAM << 8 | 0);
}

static void stream_reset(void)
{
    stream_stop();
    wr32(sd + SD_CTL, (rd32(sd + SD_CTL) | 1) & 0x00FFFFFFu);
    wait32(sd + SD_CTL, 1, 1, 20);
    delay_us(10);
    wr32(sd + SD_CTL, rd32(sd + SD_CTL) & ~1u & 0x00FFFFFFu);
    wait32(sd + SD_CTL, 1, 0, 20);
    wr8(sd + SD_STS, 0x1C); /* Statusbits loeschen */
}

/* gespielte Bytes aus der DMA-Position nachfuehren */
static void poll_position(void)
{
    if (!running)
        return;
    uint32_t lpib = rd32(sd + SD_LPIB) % RING_BYTES;
    uint64_t now = time_us();
    uint64_t ring_us = (uint64_t)RING_BYTES * 1000000 / (bytes_per_sec ? bytes_per_sec : 1);
    if (now - last_poll_us > ring_us)
        played = written; /* zu lange nicht nachgesehen: Position nicht eindeutig; der Puffer ist sicher leer gespielt */
    else
        played += (lpib + RING_BYTES - last_lpib) % RING_BYTES;
    last_lpib = lpib;
    last_poll_us = now;
    if (played > written)
        played = written; /* Unterlauf: der Controller spielt Stille */
}

static void stream_start(void)
{
    if (running)
        return;
    last_lpib = 0; /* der Start beginnt am Anfang der BDL (LPIB vorher kann noch alt sein: QEMU setzt es erst hier zurueck) */
    last_poll_us = time_us();
    wr32(sd + SD_CTL, (rd32(sd + SD_CTL) | 2) & 0x00FFFFFFu);
    running = 1;
}

/* Auto-Mute wie bei Windows/Linux: steckt an einer Buchse (Kopfhoerer, Line-Out) etwas, sind die eingebauten
 * Lautsprecher aus; sonst an. Abgefragt beim Oeffnen und waehrend der Wiedergabe alle 300 ms. */
static uint64_t automute_us;

/* Gewaehlte Ausgabe (Einstellung im Desktop): -1 automatisch (Bluetooth, wenn verbunden; sonst die Soundkarte mit
 * Auto-Mute), 0..npaths-1 nur dieser Ausgang der Soundkarte (auch bei verbundenem Bluetooth), HDA_OUT_BT Bluetooth */
static int out_sel = -1;
static Event mix_event; /* weckt den Mischer (Definition weiter unten) */

static void automute(int verbose)
{
    automute_us = time_us();
    int plugged = 0;
    for (int i = 0; i < npaths; i++) {
        paths[i].plugged = 0;
        if (paths[i].jack) {
            int64_t sense = cmd(paths[i].cad, paths[i].node[0], V_GET_PIN_SENSE << 8);
            if (sense > 0 && (sense & 0x80000000))
                plugged = paths[i].plugged = 1;
        }
    }
    for (int i = 0; i < npaths; i++) {
        Path *p = &paths[i];
        /* fester Ausgang: nur der; automatisch: alles an, nur Lautsprecher aus, wenn an einer Buchse etwas steckt */
        int off = out_sel >= 0 && out_sel < npaths ? i != out_sel : p->dev == 1 && plugged;
        if (p->muted == off)
            continue;
        p->muted = (uint8_t)off;
        cmd(p->cad, p->node[0], V_SET_PIN_CTL << 8 | (off ? 0 : p->pinctl));
        if (verbose)
            kprintf("hda: %s (Pin %#x) %s%s\n", dev_name(p->dev), p->node[0], off ? "aus" : "an",
                    out_sel >= 0 && out_sel < npaths ? " (gewaehlt)" : plugged ? " (Stecker in einer Buchse)" : "");
    }
}

/* Ton ueber Bluetooth? (automatisch oder ausdruecklich gewaehlt, und die Soundbar ist bereit) */
static int bt_route(void)
{
    return (out_sel < 0 || out_sel == HDA_OUT_BT) && bt_a2dp_active();
}

static void automute_tick(void)
{
    if (time_us() - automute_us > 300000)
        automute(1);
}

int hda_output_info(unsigned i, HdaOutput *o)
{
    memset(o, 0, sizeof(*o));
    if (present && i < (unsigned)npaths) {
        const Path *p = &paths[i];
        ksnprintf(o->name, sizeof(o->name), "%s", p->dev == 0 ? "Line-Out" : p->dev == 1 ? "Lautsprecher" : "Kopfh\xC3\xB6rer");
        o->kind = 0;
        o->plugged = p->plugged;
        o->on = !p->muted && !bt_route(); /* hier kaeme der Ton heraus (auch wenn gerade nichts spielt) */
        o->id = (int)i;
        return 0;
    }
    if (i == (unsigned)(present ? npaths : 0) && bt_a2dp_active()) {
        ksnprintf(o->name, sizeof(o->name), "Bluetooth");
        o->kind = 1;
        o->plugged = 1;
        o->on = bt_route();
        o->id = HDA_OUT_BT;
        return 0;
    }
    return -1;
}

int hda_output_select(int sel)
{
    if (sel != -1 && sel != HDA_OUT_BT && (sel < 0 || sel >= npaths))
        return HDA_ERR_FORMAT;
    out_sel = sel;
    if (present)
        automute(1); /* Pins gleich umschalten */
    event_signal(&mix_event); /* der Mischer prueft die Richtung (Bluetooth/Soundkarte) */
    return 0;
}

int hda_output_get(void)
{
    return out_sel;
}

int hda_present(void)
{
    return present;
}

/* ---------- Mischer ----------
 * Jedes Programm hat eine Stimme mit eigenem Puffer; beim Schreiben wird seine Abtastrate auf die Mischrate
 * (48 kHz, Stereo) umgerechnet. Ein Kernel-Thread addiert alle Stimmen und haelt den Ring der Soundkarte etwa
 * LEAD_MS voraus gefuellt: kurz genug, dass Effekte ohne spuerbare Verzoegerung kommen. Ist keine Stimme mehr offen
 * und alles ausgespielt, geht der Stream aus und der Thread schlaeft, bis wieder Daten kommen. */

#define MAX_VOICES   8
#define VOICE_FRAMES 65536 /* je Stimme 1,37 s bei 48 kHz: Programme koennen genug vorlegen, um kurz zu stocken */
#define LEAD_MS      60
/* Bluetooth: eine Stimme spielt erst, wenn BT_PREROLL_MS vorliegen (der Mischer nimmt beim Start gleich BT_LEAD_MS
 * fuer die Soundbar, dahinter muss Vorrat bleiben, bis das Programm nachliefert) - oder wenn das Programm seit
 * BT_IDLE_US nichts mehr schreibt (kurze Effekte, Ende eines Titels). Laeuft sie leer, waehrend das Programm noch
 * liefert, wird neu vorgepuffert: eine kurze Pause statt Stottern. */
#define BT_PREROLL_MS 500
#define BT_IDLE_US    40000

typedef struct {
    uint32_t pid;
    int      used, channels, vol;  /* vol: 0-256 */
    uint64_t step, pos;            /* Quellrate / Mischrate als 32.32; Position zwischen zwei Quell-Frames */
    int32_t  prev_l, prev_r;
    int16_t *buf;                  /* Stereo-Frames in der Mischrate */
    uint64_t wr, rd;               /* geschrieben / vom Mischer genommen (Frames, laufend) */
    uint64_t done_at;              /* Ring-Position (written), bis zu der der letzte Frame gemischt ist */
    uint64_t mixed;                /* gemischte Bytes (Mischformat) */
    uint64_t last_write_us;        /* letztes hda_write (Bluetooth: liefert das Programm noch nach?) */
    int      ready;                /* Bluetooth: genug vorgepuffert, die Stimme spielt */
} Voice;

static Voice    voices[MAX_VOICES];
static uint32_t mix_rate = 48000;
static Event    mix_event = EVENT_INIT; /* weckt den Mischer, wenn neue Daten kommen */

static Voice *voice_of(uint32_t pid)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && voices[i].pid == pid)
            return &voices[i];
    return 0;
}

/* Stream der Soundkarte starten: Mischrate, 16 Bit Stereo, alle DACs */
static void hw_start(void)
{
    int ok;
    uint16_t fmt = hda_format(mix_rate, 2, &ok);
    stream_reset();
    memset(ring, 0, RING_BYTES);
    uint64_t *bdl = (uint64_t *)(ring + RING_BYTES); /* BDL direkt hinter dem Ringpuffer */
    for (int i = 0; i < BDL_ENTRIES; i++) {
        bdl[i * 2] = ring_phys + (uint64_t)i * (RING_BYTES / BDL_ENTRIES);
        bdl[i * 2 + 1] = (uint64_t)(RING_BYTES / BDL_ENTRIES); /* Laenge, kein Interrupt */
    }
    wr32(sd + SD_BDPL, (uint32_t)(ring_phys + RING_BYTES));
    wr32(sd + SD_BDPU, (uint32_t)((ring_phys + RING_BYTES) >> 32));
    wr32(sd + SD_CBL, RING_BYTES);
    wr16(sd + SD_LVI, BDL_ENTRIES - 1);
    wr16(sd + SD_FMT, fmt);
    wr32(sd + SD_CTL, (rd32(sd + SD_CTL) & 0x000FFFFFu & ~0x1Fu) | (uint32_t)STREAM_TAG << 20);
    for (int i = 0; i < npaths; i++) {
        int dac = paths[i].node[paths[i].n - 1];
        cmd(paths[i].cad, dac, V_SET_FORMAT << 16 | fmt);
        cmd(paths[i].cad, dac, V_SET_STREAM << 8 | STREAM_TAG << 4);
    }
    set_dac_volume();
    automute(1);
    written = played = 0; /* nach dem Start liest der Controller ab dem Anfang des Rings */
    bytes_per_sec = mix_rate * 4;
}

static void hw_stop(void)
{
    stream_stop();
    dacs_release();
}

/* Ein Frame: Summe aller Stimmen (begrenzt); done_pos = Position, ab der ein leer gewordener Stimme alles gemischt hat.
 * Ergebnis: Zahl der Stimmen mit Daten; -1 = keine Stimme offen */
static int bt_mixing; /* der Mischer fuellt gerade den Bluetooth-Strom: nur vorgepufferte Stimmen (ready) spielen */

static int mix_frame(int16_t *lr, uint64_t done_pos)
{
    int32_t l = 0, r = 0;
    int n = 0, open = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        open |= v->used && (!bt_mixing || v->ready);
        if (!v->used || v->rd == v->wr || (bt_mixing && !v->ready))
            continue;
        n++;
        const int16_t *smp = v->buf + (v->rd % VOICE_FRAMES) * 2;
        l += (smp[0] * v->vol) >> 8;
        r += (smp[1] * v->vol) >> 8;
        v->rd++;
        v->mixed += 4;
        if (v->rd == v->wr) {
            v->done_at = done_pos;
            if (bt_mixing && time_us() - v->last_write_us < BT_IDLE_US)
                v->ready = 0; /* leergelaufen, das Programm liefert aber noch: neu vorpuffern statt zu stottern */
        }
    }
    lr[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l); /* begrenzen statt ueberlaufen */
    lr[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    return open ? n : -1;
}

/* Ring bis LEAD_MS vor die Leseposition mit der Summe aller Stimmen fuellen */
static void mix_some(void)
{
    poll_position();
    uint64_t lead = (uint64_t)mix_rate * 4 * LEAD_MS / 1000;
    int16_t *out = (int16_t *)ring;
    const uint32_t ring_frames = RING_BYTES / 4;
    while (written - played < lead) {
        uint64_t frames = (lead - (written - played)) / 4;
        if (frames > 1024)
            frames = 1024;
        if (!frames)
            break;
        uint64_t f0 = written / 4;
        for (uint64_t f = 0; f < frames; f++)
            mix_frame(out + (uint32_t)((f0 + f) % ring_frames) * 2, (f0 + f + 1) * 4);
        written += frames * 4;
    }
    /* dahinter etwas Stille: bleibt der Mischer einmal haengen, spielt der Controller nichts Altes */
    uint64_t guard = (uint64_t)mix_rate * 4 * 30 / 1000, room = RING_BYTES - (written - played) - 1024;
    if (guard > room)
        guard = room;
    for (uint64_t k = 0; k < guard; k += 4) {
        uint32_t idx = (uint32_t)(((written + k) / 4) % ring_frames) * 2;
        out[idx] = out[idx + 1] = 0;
    }
    __asm__ __volatile__("sfence" : : : "memory");
}

/* Bluetooth (A2DP): statt auf die Soundkarte in den Strom fuer die Soundbar mischen - im Takt der Uhr, BT_LEAD_MS
 * voraus. Der Vorlauf fuellt beim Start den Puffer der Soundbar: Funkaussetzer (WLAN und Bluetooth teilen sich die
 * Antenne) bis zu dieser Laenge hoert man dann nicht. Ohne Daten wird nichts geschickt (der Strom pausiert dann); die
 * Uhr beginnt mit dem naechsten Ton neu. Die Gesamtlautstaerke wirkt hier digital (quadratisch, wie ein
 * Lautstaerkeregler empfunden wird). */
#define BT_LEAD_MS 250
static uint64_t bt_t0_us, bt_frames;
static int      bt_gap; /* gerade in einer Luecke (Stimme offen, aber leer) */

static void bt_mix(int data)
{
    if (!data) {
        bt_t0_us = 0;
        return;
    }
    uint64_t now = time_us();
    if (!bt_t0_us) {
        bt_t0_us = now;
        bt_frames = 0;
    }
    uint64_t lead = (uint64_t)mix_rate * BT_LEAD_MS / 1000;
    uint64_t target = (now - bt_t0_us) * mix_rate / 1000000 + lead;
    if (target > bt_frames + lead + mix_rate * 3 / 10) { /* stand der Mischer (System stockte): nicht nachholen - das
                                                           * saugte nur den Vorrat der Programme leer -, Uhr neu stellen */
        kprintf("hda: Bluetooth-Mischer %u ms im Rueckstand - Uhr neu gestellt\n",
                (uint32_t)((target - bt_frames - lead) * 1000 / mix_rate));
        bt_frames = target - lead;
    }
    int32_t gain = bt_abs_volume_active() ? 10000 : volume * volume; /* 0..10000; absolut: regelt die Soundbar */
    static int16_t tmp[512 * 2];
    while (bt_frames < target) {
        uint32_t n = (uint32_t)(target - bt_frames), room = bt_a2dp_room();
        if (n > 512)
            n = 512;
        if (n > room)
            n = room;
        if (!n)
            break;
        uint32_t f = 0;
        int empty = 0;
        for (; f < n; f++) {
            if (mix_frame(tmp + f * 2, 0) <= 0) { /* keine spielbereite Stimme mehr: keine Stille schicken */
                empty = 1;
                break;
            }
            tmp[f * 2] = (int16_t)(tmp[f * 2] * gain / 10000);
            tmp[f * 2 + 1] = (int16_t)(tmp[f * 2 + 1] * gain / 10000);
        }
        if (f)
            bt_a2dp_write(tmp, f);
        bt_frames += f;
        if (empty) {
            int refill = 0; /* leergelaufen, waehrend das Programm noch liefert: Luecke (einmal je Luecke zaehlen) */
            for (int i = 0; i < MAX_VOICES; i++)
                refill |= voices[i].used && !voices[i].ready;
            if (refill && !bt_gap)
                bt_a2dp_underrun();
            bt_gap = refill;
            bt_t0_us = 0; /* Uhr beginnt mit dem naechsten Ton neu (wieder mit Vorlauf fuer die Soundbar) */
            return;
        }
        bt_gap = 0;
    }
}

/* Bluetooth: Stimme v spielbereit? (vorgepuffert oder das Programm liefert nichts mehr nach) */
static int bt_voice_ready(Voice *v)
{
    if (!v->used || v->rd == v->wr)
        return 0;
    if (!v->ready && (v->wr - v->rd >= (uint64_t)mix_rate * BT_PREROLL_MS / 1000 ||
                      time_us() - v->last_write_us >= BT_IDLE_US))
        v->ready = 1;
    return v->ready;
}

static void mixer_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int used = 0, data = 0, waiting = 0;
        int bt = bt_route();
        for (int i = 0; i < MAX_VOICES; i++) {
            used |= voices[i].used;
            if (bt) {
                int r = bt_voice_ready(&voices[i]);
                data |= r;
                waiting |= !r && voices[i].used && voices[i].rd != voices[i].wr;
            } else {
                data |= voices[i].used && voices[i].rd != voices[i].wr;
            }
        }
        if (bt) { /* Soundbar verbunden: Ton ueber Bluetooth */
            if (running)
                hw_stop();
            bt_mixing = 1;
            bt_mix(data);
            bt_mixing = 0;
            event_wait(&mix_event, data || waiting ? 5 : 100);
            continue;
        }
        bt_t0_us = 0;
        if (!running && data) {
            hw_start();
            mix_some();
            stream_start();
        } else if (running && used) {
            mix_some();
            automute_tick();
        } else if (running) { /* keine Stimme mehr: nichts nachschieben (dahinter steht Stille), ausspielen, dann aus */
            poll_position();
            if (played >= written)
                hw_stop();
        }
        if (running)
            thread_sleep_ms(5);
        else
            event_wait(&mix_event, 0); /* ohne Ton: schlafen, bis hda_write Daten bringt */
    }
}

int hda_open(uint32_t pid, uint32_t rate, uint32_t channels)
{
    if (!present)
        return HDA_ERR_NODEV;
    if (channels < 1 || channels > 2 || rate < 8000 || rate > 192000)
        return HDA_ERR_FORMAT;
    Voice *v = voice_of(pid);
    for (int i = 0; !v && i < MAX_VOICES; i++)
        if (!voices[i].used)
            v = &voices[i];
    if (!v)
        return HDA_ERR_BUSY;
    int16_t *buf = v->buf;
    memset(v, 0, sizeof(*v));
    v->buf = buf;
    v->pid = pid;
    v->channels = (int)channels;
    v->vol = 256;
    v->step = ((uint64_t)rate << 32) / mix_rate;
    v->used = 1;
    return 0;
}

int64_t hda_write(uint32_t pid, const void *buf, uint64_t len)
{
    Voice *v = present ? voice_of(pid) : 0;
    if (!v)
        return HDA_ERR_NOTOPEN;
    const int16_t *src = buf;
    uint64_t frames = len / (uint64_t)(v->channels * 2), done = 0;
    uint32_t most = (uint32_t)(((1ULL << 32) + v->step - 1) / v->step) + 1; /* hoechstens so viele Ausgabe-Frames je Eingabe-Frame */
    while (done < frames) {
        if (VOICE_FRAMES - (v->wr - v->rd) < most) { /* Puffer voll: warten, bis der Mischer Platz macht */
            thread_sleep_ms(5);
            if (!v->used || v->pid != pid)
                return (int64_t)(done * (uint64_t)(v->channels * 2)); /* waehrenddessen geschlossen */
            continue;
        }
        while (done < frames && VOICE_FRAMES - (v->wr - v->rd) >= most) {
            int32_t l = src[done * (uint64_t)v->channels], r = v->channels == 2 ? src[done * 2 + 1] : l;
            while ((v->pos >> 32) == 0) { /* lineare Interpolation zwischen vorigem und diesem Quell-Frame */
                int32_t frac = (int32_t)((v->pos >> 16) & 0xFFFF);
                int16_t *d = v->buf + (v->wr % VOICE_FRAMES) * 2;
                d[0] = (int16_t)(v->prev_l + (int32_t)(((int64_t)(l - v->prev_l) * frac) >> 16));
                d[1] = (int16_t)(v->prev_r + (int32_t)(((int64_t)(r - v->prev_r) * frac) >> 16));
                v->wr++;
                v->pos += v->step;
            }
            v->pos -= 1ULL << 32;
            v->prev_l = l;
            v->prev_r = r;
            done++;
        }
        v->last_write_us = time_us();
        if (!running)
            event_signal(&mix_event);
    }
    return (int64_t)(done * (uint64_t)(v->channels * 2));
}

int hda_drain(uint32_t pid)
{
    Voice *v = present ? voice_of(pid) : 0;
    if (!v)
        return HDA_ERR_NOTOPEN;
    uint64_t end = time_ms() + 3000 + (v->wr - v->rd) * 1000 / mix_rate;
    while (time_ms() < end && v->used && v->pid == pid) {
        poll_position();
        if (v->rd == v->wr && (!running || played >= v->done_at))
            break;
        thread_sleep_ms(5);
    }
    thread_sleep_ms(30); /* FIFO des Controllers */
    return 0;
}

void hda_close(uint32_t pid)
{
    Voice *v = present && pid ? voice_of(pid) : 0;
    if (v)
        v->used = 0;
}

uint64_t hda_played(uint32_t pid)
{
    Voice *v = present ? voice_of(pid) : 0;
    return v ? v->mixed : 0;
}

int hda_voice_volume(uint32_t pid, int percent)
{
    Voice *v = present ? voice_of(pid) : 0;
    if (!v)
        return HDA_ERR_NOTOPEN;
    if (percent >= 0)
        v->vol = (percent > 100 ? 100 : percent) * 256 / 100;
    return v->vol * 100 / 256;
}

int hda_volume(int percent)
{
    if (percent >= 0) {
        volume = percent > 100 ? 100 : percent;
        if (present)
            set_dac_volume();
        bt_abs_volume_set(volume); /* Soundbar mit absoluter Lautstaerke: dort einstellen */
    }
    return volume;
}

/* Die Soundbar meldet ihre Lautstaerke (eigene Tasten, Fernbedienung): uebernehmen, ohne sie zurueckzuschicken */
void hda_volume_remote(int percent)
{
    volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (present)
        set_dac_volume();
}

/* ---------- Start ---------- */

void hda_init(void)
{
    if (pci_find_class(0x04, 0x03, -1, 0, &pci) != 0)
        return;
    uint64_t bar = pci_bar_mem(&pci, 0);
    if (!bar || paging_map_mmio(bar, 0x4000) != 0) {
        kprintf("hda: %02x:%02x.%u ohne nutzbaren BAR0\n", pci.bus, pci.dev, pci.fn);
        return;
    }
    pci_enable(&pci, 0, 1, 1);
    pci_set_driver(&pci, "hda");
    regs = (volatile uint8_t *)bar;
    uint16_t gcap = rd16(GCAP);
    uint32_t iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    kprintf("hda: %02x:%02x.%u %04x:%04x @ %#lx, %u Eingabe-/%u Ausgabe-Streams\n", pci.bus, pci.dev, pci.fn,
            pci.vendor, pci.device, (unsigned long)bar, iss, oss);
    if (!oss) {
        kprintf("hda: kein Ausgabe-Stream\n");
        return;
    }
    sd = 0x80 + iss * 0x20;

    /* Controller zuruecksetzen; danach melden sich die Codecs in STATESTS */
    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    wr32(GCTL, rd32(GCTL) & ~1u);
    if (wait32(GCTL, 1, 0, 100))
        kprintf("hda: Reset geht nicht an\n");
    delay_us(100);
    wr32(GCTL, rd32(GCTL) | 1);
    if (wait32(GCTL, 1, 1, 100)) {
        kprintf("hda: Controller kommt nicht aus dem Reset\n");
        return;
    }
    delay_us(1000); /* Codecs brauchen mindestens 521 us */
    uint16_t codecs = 0;
    for (int i = 0; i < 50 && !codecs; i++) {
        codecs = rd16(STATESTS) & 0x7FFF;
        if (!codecs)
            delay_us(1000);
    }
    wr16(STATESTS, codecs);
    wr32(INTCTL, 0);
    if (!codecs) {
        kprintf("hda: kein Codec gefunden\n");
        return;
    }
    if (setup_rings()) {
        kprintf("hda: kein Speicher fuer CORB/RIRB\n");
        return;
    }
    for (int cad = 0; cad < 15; cad++)
        if (codecs & (1u << cad))
            probe_codec(cad);
    if (!npaths) {
        kprintf("hda: kein analoger Ausgang gefunden\n");
        return;
    }
    uint64_t r = pmm_alloc_frames(RING_BYTES / 4096 + 1); /* Ringpuffer + eine Seite fuer die BDL */
    if (!r) {
        kprintf("hda: kein Speicher fuer den Tonpuffer\n");
        return;
    }
    ring = (uint8_t *)r;
    ring_phys = r;
    wr32(SSYNC, 0);
    uint64_t vm = pmm_alloc_frames((uint64_t)MAX_VOICES * VOICE_FRAMES * 4 / 4096);
    if (!vm) {
        kprintf("hda: kein Speicher fuer die Stimmen\n");
        return;
    }
    for (int i = 0; i < MAX_VOICES; i++)
        voices[i].buf = (int16_t *)(vm + (uint64_t)i * VOICE_FRAMES * 4);
    if (!(rates_ok & (1u << 6)) && (rates_ok & (1u << 5)))
        mix_rate = 44100; /* kein 48 kHz: dann 44,1 kHz mischen */
    present = 1;
    stream_reset();
    automute(1);
    thread_create("audio", mixer_thread, 0);
    kprintf("hda: bereit, %d Ausgang/Ausgaenge, Raten %#x, Mischer %u Hz, bis %d Stimmen\n", npaths, rates_ok & 0xFFF,
            mix_rate, MAX_VOICES);
}
