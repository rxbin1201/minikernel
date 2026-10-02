#ifndef IGD_INTERNAL_H
#define IGD_INTERNAL_H

#include <stdint.h>
#include "drivers/gpu/igd.h"
#include "drivers/pci.h"

/* Gemeinsam zwischen igd.c (Erkennung, Anzeige, Mauszeiger, Doppelpufferung) und igd_blt.c (Blitter-Engine) */

#define IGD_PTE_VALID 1ULL

extern IgdInfo            igd_state;
extern volatile uint8_t  *igd_regs;
extern volatile uint64_t *igd_ggtt;          /* globale Grafik-Seitentabelle */
extern uint32_t           igd_ggtt_entries;

uint32_t igd_rd(uint32_t off);
void     igd_wr(uint32_t off, uint32_t v);
void     igd_clflush(uint64_t addr, uint64_t len);   /* CPU-Cache zurueckschreiben/verwerfen (GPU liest am Cache vorbei) */
void     igd_ggtt_flush(void);                        /* geaenderte GGTT-Eintraege uebernehmen */
int      igd_ggtt_claim(uint32_t base, uint32_t pages, uint64_t *saved); /* 0 = frei, alte Eintraege gesichert */
int      igd_preflight(const char *what);             /* Vorpruefung fuer igdtest (0 = weiter) */
uint32_t igd_underrun_begin(int pipe);                 /* FIFO-Unterlauf festhalten lassen; Ergebnis: alte Maske */
int      igd_underrun_end(int pipe, uint32_t imr);     /* 1 = Unterlauf seit begin */
int      igd_dpcd_read(int port, uint32_t addr, uint8_t *buf, int len); /* igd_dp.c: DPCD per AUX, len <= 16 */
int      igd_dpcd_write(int port, uint32_t addr, const uint8_t *buf, int len); /* igd_dp.c: 0 = ok */
int      igd_dp_edid(int port, uint8_t *edid);         /* igd_dp.c: EDID per AUX (256 Byte Platz); Bloecke, 0 = keine */
void     igd_irq_init(const PciDevice *d);              /* igd_irq.c: Bildwechsel-Interrupt einrichten */
void     igd_cursor_reapply(void);                     /* igd.c: Zeiger-Ebene nach einem Anschlusswechsel */
void     igd_edid_dump(const uint8_t *edid, int blocks, uint32_t limit_khz, const char *note); /* igd_mode.c */

int  igd_forcewake_get(void);     /* Grafikkern wecken (Render, Blitter, Media); 1 = bestaetigt */
void igd_forcewake_put(void);

/* Blitter im Dauerbetrieb (igd_blt.c): Bild-Updates aus dem Speicher des laufenden Programms */
int  igd_blt_init(void);          /* beim Start; 0 = Blitter uebernimmt */
int  igd_blt_on(void);
int  igd_blt_sync(void);          /* warten, bis alles kopiert ist (vor Schreiben der CPU); 0 = ok */
/* 1 = in Auftrag gegeben (wait = 1: ganzes Bild, kehrt erst zurueck, wenn es fertig ist), 0 = die CPU muss kopieren */
int  igd_blt_copy_user(uint32_t dst, const uint32_t *src, uint32_t pitch, int x, int y, int w, int h, int wait);
int  igd_blt_copy_gtt(uint32_t dst, uint32_t dpitch, uint32_t src, uint32_t spitch, int w, int h); /* 0 = kopiert */
int  igd_blt_copy_gtt_xy(uint32_t dst, uint32_t dpitch, int dx, int dy, uint32_t src, uint32_t spitch, int w, int h);
void igd_blt_report(void);        /* igdtest info */

/* Render-Engine fuer das Zusammensetzen (igd_rcs.c, benutzt von igd_comp.c). Flaechen als Bytes: w = Breite in Bytes,
 * gtt = GGTT-Adresse des ersten Bytes (Ecke des Rechtecks), pitch = Zeilenlaenge der ganzen Flaeche */
typedef struct {
    uint32_t gtt, w, h, pitch;
    uint32_t mocs; /* Cache-Steuerung im Surface State: 0 = uncached, IGD_MOCS_WB = im LLC (write-back) */
} IgdSurf;
#define IGD_MOCS_WB 4 /* Gen9: Tabellenindex 2 << 1 (fuer den Fall "Index direkt" ist Eintrag 4 genauso gesetzt) */
enum { IGD_BLK_8X8, IGD_BLK_1X8, IGD_BLK_8X1, IGD_BLK_1X1 }; /* Pixel je Thread: die Bloecke muessen genau aufgehen */
enum { IGD_OP_COPY, IGD_OP_BLEND, IGD_OP_VSCALE, IGD_OP_HSCALE };
typedef struct {
    int      op;               /* IGD_OP_*: kopieren, Quelle mit ihrem Alpha (Byte 3) mal galpha/256 ueber das Ziel
                                * mischen, senkrecht / waagerecht skalieren (nur IGD_BLK_8X8) */
    uint32_t galpha;           /* mischen: Deckung 0-256 */
    uint32_t step;             /* skalieren: Quellzeile bzw. -spalte = Anfang + (i * step >> 8) */
    int      shape;            /* IGD_BLK_* */
    uint32_t gx, gy;           /* Bloecke (Threads) in x und y */
    IgdSurf  dst, src;         /* ganze Flaechen (Anfang an 4 KiB, Zeilenlaenge Vielfaches von 64 Byte) */
    uint32_t dx, dy, sx, sy;   /* linke obere Ecke des Rechtecks in Ziel und Quelle (x in Bytes) */
} IgdCompOp;
#define IGD_COMP_MAX_OPS 120
/* Auftraege der Reihe nach ausfuehren (jeder sieht die Ergebnisse der vorigen). Vorher wird ein noch offener Auftrag
 * abgewartet. async = 0: auch diesen abwarten (GPU-Zeit in *us); async = 1: nur abschicken (igd_rcs_comp_wait).
 * 0 = fertig bzw. abgeschickt, -1 = Engine gerade belegt (igdtest), -2 = Engine-Fehler */
int igd_rcs_comp(const IgdCompOp *ops, int n, uint64_t *us, int async);
/* offenen Auftrag abwarten (0 = keiner mehr offen; GPU-Zeit des Auftrags in *us, 0 wenn schon gemeldet) */
int igd_rcs_comp_wait(uint64_t *us);
int igd_front_surface(uint32_t *gtt, uint32_t *pitch, uint32_t *w, uint32_t *h); /* angezeigter Puffer, 0 = ok */

/* 3D fuer Programme (igd_rcs.c, Systemaufruf in igd_comp.c): erst auf Wunsch Farbe und/oder Tiefe loeschen, dann
 * Dreiecke (je Eckpunkt x, y, z, u, v, Normale x, y, z, r, g, b, a als float) mit Matrix und Licht im Vertex-Shader
 * (Helligkeit = amb + dif * saturate(Normale . l)) und Textur mal Farbe im Pixel-Shader. Gleitkommawerte als Bitmuster.
 * Ziel: Zeichenbereich w x h ab rt_gtt (Anfang an 64 Byte, Zeilenlaenge pitch); Tiefenpuffer D32_FLOAT, Y-Kacheln.
 * 0 = fertig, -1 = Engine-Fehler (zurueckgesetzt) */
#define IGD_3D_DEPTH       1 /* Tiefentest "kleiner" mit Schreiben */
#define IGD_3D_LINEAR      2 /* Textur bilinear (sonst naechster Texel) */
#define IGD_3D_CLEAR_COLOR 4
#define IGD_3D_CLEAR_DEPTH 8
#define IGD_3D_MAX_VERT    1365 /* passt in 64 KiB (48 Bytes je Eckpunkt), Vielfaches von 3 */
typedef struct {
    uint32_t        rt_gtt, w, h, pitch, mocs;
    uint32_t        depth_gtt, depth_pitch;
    uint32_t        tex_gtt, tex_w, tex_h, tex_pitch;
    uint32_t        flags, clear_color, clear_depth;
    uint32_t        m[16], l[3], amb, dif;
    uint32_t        nvert;
    const uint32_t *verts;
} IgdDraw3d;
int igd_rcs_draw3d(const IgdDraw3d *g);

/* Doppelpufferung (igd.c): A = Framebuffer der Firmware, B = zweiter Puffer im RAM */
extern int       igd_flip_ready;
extern uint32_t  igd_scr_w, igd_scr_h, igd_scr_stride;
extern uint32_t  igd_surf_a, igd_surf_b;
extern uint8_t  *igd_buf_a, *igd_buf_b;

#endif
