#ifndef TTF_H
#define TTF_H

/* TrueType-Schriften mit Kantenglaettung (stb_truetype, gemeinfrei). Die Schriften liegen in /share/fonts:
 * Inter (Oberflaeche, normal und halbfett) und JetBrains Mono (Terminal), beide SIL Open Font License.
 * Groessen in Pixeln (em). Fehlt eine Schrift, zeichnen die Funktionen mit der 8x16-Bitmapschrift. */

#include "gfx.h"

typedef struct Font Font;

extern Font *font_ui, *font_bold, *font_mono;

int   fonts_init(void);            /* laedt die drei Schriften; 0 = alle da */
Font *font_load(const char *path); /* NULL = nicht lesbar */

/* Text mit der linken oberen Ecke der Zeile bei (x, y); Ergebnis: Breite in Pixeln */
int text_draw(Surface *s, Font *f, int size, int x, int y, const char *utf8, u32 color);
int text_width(Font *f, int size, const char *utf8);
int text_height(Font *f, int size); /* Zeilenhoehe */
int text_ascent(Font *f, int size); /* Grundlinie unter der Oberkante */
/* ein Zeichen (fuer Terminals): Ergebnis Vorschub; text_advance ohne Zeichnen */
int text_glyph(Surface *s, Font *f, int size, int x, int y, unsigned cp, u32 color);
int text_advance(Font *f, int size, unsigned cp);

#endif
