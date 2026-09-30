#ifndef GFX_H
#define GFX_H

/* Grafik fuer Programme: Das Programm uebernimmt mit gfx_open() den ganzen Bildschirm (die Konsole pausiert und kommt
 * bei gfx_close() bzw. beim Programmende unveraendert zurueck). Gezeichnet wird in gfx_screen (Speicher des Programms);
 * gfx_present() zeigt ein Rechteck davon an. Den Mauszeiger zeichnet die Bibliothek selbst darueber.
 * Farben: 0x00RRGGBB (RGB(r, g, b)). Text: 8x16-Schrift des Kernels, UTF-8. */

#include "libc.h"

typedef unsigned int u32;

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
#define GFX_TRANSPARENT 0xFF000000u

typedef struct {
    u32 *px;
    int  w, h;
} Surface;

typedef struct {
    int x0, y0, x1, y1; /* Zeichnen nur in [x0, x1) x [y0, y1) */
} Clip;

extern Surface gfx_screen; /* hierhin zeichnet das Programm */
extern Clip    gfx_clip;   /* aktuelles Clip-Rechteck (gfx_set_clip) */

enum { EV_NONE, EV_KEY, EV_DOWN, EV_UP, EV_MOVE, EV_WHEEL };

typedef struct {
    int type;
    int key;         /* EV_KEY: Zeichen (UTF-8-Byte) oder KEY_* */
    int x, y;        /* Mausposition */
    int button;      /* EV_DOWN/EV_UP: 1 links, 2 rechts */
    int wheel;       /* EV_WHEEL: > 0 = nach oben */
} Event;

void *gfx_alloc(u64 bytes);
void gfx_free(void *p, u64 bytes);
int surface_new(Surface *s, int w, int h);
void surface_free(Surface *s);
void gfx_set_clip(int x, int y, int w, int h);
void gfx_no_clip(void);
/* Alles Zeichnen (auch nach gfx_no_clip) auf ein Rechteck beschraenken, z.B. um nur einen geaenderten Bereich neu zu
 * zeichnen; gfx_reset_base_clip() hebt das wieder auf */
void gfx_set_base_clip(int x, int y, int w, int h);
void gfx_reset_base_clip(void);
void gfx_pixel(Surface *s, int x, int y, u32 c);
u32 gfx_get(Surface *s, int x, int y);
void gfx_fill(Surface *s, int x, int y, int w, int h, u32 c);
void gfx_rect(Surface *s, int x, int y, int w, int h, u32 c);
/* Rahmen mit Lichtkante (3D): raised = 1 erhaben, 0 eingedrueckt */
void gfx_bevel(Surface *s, int x, int y, int w, int h, int raised);
void gfx_line(Surface *s, int x0, int y0, int x1, int y1, u32 c);
void gfx_fill_circle(Surface *s, int cx, int cy, int r, u32 c);
/* Dicke Linie (Pinsel mit Durchmesser d): Kreise entlang der Linie */
void gfx_thick_line(Surface *s, int x0, int y0, int x1, int y1, int d, u32 c);
void gfx_circle(Surface *s, int cx, int cy, int r, u32 c);
/* Ellipse im Rechteck (x0, y0) - (x1, y1) */
void gfx_ellipse(Surface *s, int x0, int y0, int x1, int y1, u32 c, int filled);
const unsigned char *gfx_glyph(unsigned cp);
unsigned gfx_utf8_next(const char **sp);
/* Ein Zeichen zeichnen */
void gfx_char(Surface *s, int x, int y, unsigned cp, u32 fg, u32 bg, int scale);
/* Zeichnet UTF-8-Text (scale = Vergroesserung); bg = GFX_TRANSPARENT: ohne Hintergrund. Liefert die Breite in Pixeln. */
int gfx_text_scaled(Surface *s, int x, int y, const char *t, u32 fg, u32 bg, int scale);
int gfx_text(Surface *s, int x, int y, const char *t, u32 fg, u32 bg);
int gfx_text_width(const char *t);
/* Bild (Surface) in ein Rechteck skalieren (naechster Nachbar) */
void gfx_draw_scaled(Surface *dst, const Surface *src, int dx, int dy, int dw, int dh);
/* Rechteck aus gfx_screen in die Anzeige uebernehmen, Zeiger darueber zeichnen */
void gfx_compose(int x, int y, int w, int h);
void gfx_present(int x, int y, int w, int h);
void gfx_present_all(void);
void gfx_move_cursor(int x, int y);
void gfx_show_cursor(int visible);
/* Naechstes Ereignis; 0 = keins (nicht blockierend) */
int gfx_poll(Event *e);
/* Wartet hoechstens timeout_ms auf ein Ereignis (-1 = ohne Grenze). 0 = keins */
int gfx_wait(Event *e, int timeout_ms);
int gfx_open(void);
void gfx_close(void);
/* Bildschirm voruebergehend abgeben (z.B. um ein anderes Grafikprogramm zu starten) und wieder uebernehmen */
void gfx_suspend(void);
int gfx_resume(void);
/* Laedt ein BMP. 0 = ok; Fehlermeldung in err */
int bmp_load(const char *path, Surface *out, char *err, int errmax);
/* Speichert ein Rechteck als 24-Bit-BMP. 0 = ok */
int bmp_save(const char *path, const Surface *s, int x, int y, int w, int h);

#endif
