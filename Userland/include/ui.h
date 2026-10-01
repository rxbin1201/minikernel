#ifndef UI_H
#define UI_H

/* Gemeinsames Aussehen des Desktops und seiner Fenster-Programme (hell, im Stil von macOS): Massstab, Masse, Farben,
 * Schriften, Programmsymbole. Alle Masse sind fuer 1920x1080 angegeben; U() passt sie an groessere Bildschirme an. */

#include "gfx.h"
#include "ttf.h"

extern int ui_pct; /* 100 = Grundgroesse; ab 1300 Pixel Bildschirmhoehe 125 */
#define U(x) ((x) * ui_pct / 100)

extern int FS, FS_SMALL, FS_MONO; /* Schriftgroessen: normal, klein, Terminal */
extern int ROW_H;                 /* Zeile in Listen */
extern int CELL_W, CELL_H;        /* Zeichenzelle der Terminalschrift */

/* Massstab setzen (0 = gfx_ui_scale()), Schriften laden, Masse berechnen */
void ui_setup(int pct);

#define C_TEXT       0x1D1D1F
#define C_TEXT2      0x6E6E73
#define C_WINDOW     0xFFFFFF
#define C_TITLE_TOP  0xF6F6F6
#define C_TITLE_BOT  0xE9E9E9
#define C_HAIRLINE   0xD6D6D6
#define C_ACCENT     0x0A64D6
#define C_TERM_BG    0x1E1E1E

/* Programmsymbole; die Nummern sind zugleich die Aktionen des Desktops (A_TERM = ICON_TERM ...) */
enum { ICON_NONE, ICON_TERM, ICON_FILES, ICON_CALC, ICON_CLOCK, ICON_ABOUT, ICON_PAINT, ICON_SNAKE, ICON_TETRIS,
       ICON_EDIT, ICON_MUSIC, ICON_TEXT = 100, ICON_IMAGE = 101 };
void ui_app_icon(Surface *s, int icon, int x, int y, int size);

/* Kleine Helfer */
float ui_sin(float x); /* ohne Bibliothek, genau genug fuer Grafik */
static inline float ui_cos(float x) { return ui_sin(x + 1.5707963f); }
/* Schmale abgerundete Bildlaufleiste am rechten Rand von (x, y, h): total Zeilen, visible sichtbar, top oben */
void ui_scrollbar(Surface *s, int x, int y, int h, int total, int visible, int top);
/* Symbole fuer Dateilisten */
void ui_folder_icon(Surface *s, int x, int y, int sz);
void ui_doc_icon(Surface *s, int x, int y, int sz, u32 accent);

#endif
