#ifndef GFX_H
#define GFX_H

/* Grafik fuer Programme: Das Programm uebernimmt mit gfx_open() den ganzen Bildschirm (die Konsole pausiert und kommt
 * bei gfx_close() bzw. beim Programmende unveraendert zurueck). Gezeichnet wird in gfx_screen (Speicher des Programms);
 * gfx_present() zeigt ein Rechteck davon an. Den Mauszeiger zeichnet die Bibliothek selbst darueber.
 * Unter dem Desktop gestartet, bekommt das Programm stattdessen ein Fenster (gfx_screen ist dann dessen Inhalt; siehe
 * winproto.h); gfx_open_window() legt dessen Groesse und Titel fest. Ohne Desktop ist beides der ganze Bildschirm.
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

/* Nur im Fenster: EV_CLOSE Fenster soll zu (Schliessen-Knopf, Desktop beendet); EV_FOCUS key = 1 aktiv, 0 nicht mehr;
 * EV_RESIZE neue Groesse (x, y = Breite, Hoehe; gfx_screen hat sie schon): alles neu zeichnen */
enum { EV_NONE, EV_KEY, EV_DOWN, EV_UP, EV_MOVE, EV_WHEEL, EV_CLOSE, EV_FOCUS, EV_RESIZE };

/* EV_KEY mit Umschalttasten: key = Taste (Kleinbuchstabe, Ziffer, '\t' oder KEY_*) | KEY_MOD_*. Alt kommt mit allen
 * Tasten, Shift und Strg nur mit Sondertasten (Pfeile, Pos1, Ende, Bild, Entf; Buchstaben kommen schon gross bzw. als
 * Steuerzeichen). Der Desktop benutzt einige Alt-Kombinationen selbst (Alt+Tab, Alt+W ...). */
#define KEY_MOD_ALT   0x1000
#define KEY_MOD_SHIFT 0x2000
#define KEY_MOD_CTRL  0x4000
#define KEY_BASE(k)   ((k) & 0xFF)

typedef struct {
    int type;
    int key;         /* EV_KEY: Zeichen (UTF-8-Byte), KEY_* oder mit KEY_MOD_ALT */
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
/* ---------- Glatt zeichnen (draw.c): Kantenglaettung und Transparenz, alpha 0-255 ---------- */
u32  gfx_mix(u32 under, u32 over, int alpha);
void gfx_blend_fill(Surface *s, int x, int y, int w, int h, u32 c, int alpha);
void gfx_round_rect(Surface *s, int x, int y, int w, int h, int r, u32 c, int alpha);  /* gefuellt, Radius r */
void gfx_round_frame(Surface *s, int x, int y, int w, int h, int r, u32 c, int alpha); /* 1-px-Rahmen innen */
void gfx_shadow(Surface *s, int x, int y, int w, int h, int r, int blur, int alpha);   /* weicher Schatten aussen */
void gfx_gradient(Surface *s, int x, int y, int w, int h, u32 top, u32 bottom);
void gfx_disc(Surface *s, float cx, float cy, float r, u32 c, int alpha);
void gfx_ring(Surface *s, float cx, float cy, float r, float width, u32 c, int alpha);
void gfx_capsule(Surface *s, float ax, float ay, float bx, float by, float width, u32 c, int alpha);
void gfx_blur(Surface *s, int radius); /* ganzes Bild */
/* Bildausschnitt (sx, sy, w, h) aus src nach (dx, dy) mit abgerundeten Ecken (Radius r, geglaettet) kopieren */
void gfx_blit_round(Surface *dst, const Surface *src, int sx, int sy, int dx, int dy, int w, int h, int r);
void gfx_round_rect_grad(Surface *s, int x, int y, int w, int h, int r, u32 top, u32 bottom, int alpha);
/* ganzes Bild src skaliert (bilinear) mit runden Ecken und Transparenz nach (dx, dy, dw, dh): fuer Animationen */
void gfx_blit_scaled(Surface *dst, const Surface *src, int dx, int dy, int dw, int dh, int alpha, int r);

/* Wartet hoechstens timeout_ms auf ein Ereignis (-1 = ohne Grenze). 0 = keins */
int gfx_wait(Event *e, int timeout_ms);
/* Wartet auf den naechsten Bildwechsel des Monitors (z.B. 100 Hz): damit laeuft eine Programmschleife genau im Takt der
 * Anzeige. Ohne Bildwechsel-Interrupt (z.B. in QEMU) 10 ms. Ergebnis: 1 = echter Bildwechsel, 0 = Ersatz */
int gfx_vsync(void);
int gfx_open(void);
/* Wie gfx_open, unter dem Desktop aber ein Fenster mit w x h Pixeln Inhalt (0 = etwa 60 % des Bildschirms) */
int gfx_open_window(int w, int h, const char *title);
#define GFX_RESIZABLE 1 /* Fenster darf seine Groesse aendern (das Programm verarbeitet EV_RESIZE) */
int gfx_open_window_ex(int w, int h, const char *title, int flags);
/* Unter dem Desktop: Datei oder Ordner mit dem passenden Programm oeffnen lassen. 0 = gesendet */
int gfx_desktop_open(const char *path);
void gfx_set_title(const char *title);
int gfx_windowed(void);  /* 1 = Programm laeuft in einem Fenster des Desktops */
/* 1 = unter dem Desktop gestartet (geht schon vor gfx_open: z.B. um die Fenstergroesse nach gfx_ui_scale zu waehlen) */
int gfx_desktop(void);
/* Massstab der Oberflaeche in Prozent (100, ab 1300 Pixel Bildschirmhoehe 125); unter dem Desktop schon vor gfx_open,
 * sonst erst danach richtig */
int gfx_ui_scale(void);
/* Groesse des ganzen Bildschirms (im Fenster: die des Desktops) */
void gfx_display_size(int *w, int *h);
void gfx_close(void);
/* Bildschirm voruebergehend abgeben (z.B. um ein anderes Grafikprogramm zu starten) und wieder uebernehmen */
void gfx_suspend(void);
int gfx_resume(void);
/* Laedt ein BMP. 0 = ok; Fehlermeldung in err */
int bmp_load(const char *path, Surface *out, char *err, int errmax);
/* Speichert ein Rechteck als 24-Bit-BMP. 0 = ok */
int bmp_save(const char *path, const Surface *s, int x, int y, int w, int h);

#endif
