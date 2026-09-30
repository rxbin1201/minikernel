#ifndef USERLAND_BIN_DESKTOP_DESKTOP_H
#define USERLAND_BIN_DESKTOP_DESKTOP_H

/* Gemeinsame Typen, Zustand und Funktionen des Desktops (Userland/bin/desktop/). */

#include "gfx.h"
#include "malloc.h"
#include "ttf.h"

/* desktop: grafische Oberflaeche im Stil von macOS (hell).
 *   Oben die Menueleiste (Logo-Menue, Menue des aktiven Programms, Lautstaerke, Uhr), unten das Dock mit den
 *   Programmen (Punkt = laeuft) und rechts davon die minimierten Fenster.
 *   Fenster: runde Ecken, Schatten, links die drei Knoepfe (schliessen, minimieren, zoomen). Verschieben an der
 *   Titelleiste, Groesse aendern an der Ecke unten rechts.
 *   Programme: Terminal (eine echte Shell), Dateien, Textansicht, Bildansicht, Rechner, Uhr, Info; Malen, Snake und
 *   Tetris laufen im Vollbild. "Zur Konsole" im Logo-Menue beendet den Desktop.
 * Alle Masse sind fuer 1920x1080 angegeben und werden mit U() an groessere Bildschirme angepasst. */

#define MAXW 16

/* Masse (chrome.c: ui_init) */
extern int ui_pct;               /* 100 = Grundgroesse; ab 1300 Pixel Hoehe 125 */
#define U(x) ((x) * ui_pct / 100)
extern int MENUBAR_H, TITLE_H, DOCK_H, ROW_H, RADIUS, SHADOW, FS, FS_SMALL, FS_MONO;
extern int CELL_W, CELL_H;       /* Terminal: Zeichenzelle */

/* Farben (hell) */
#define C_TEXT       0x1D1D1F
#define C_TEXT2      0x6E6E73
#define C_WINDOW     0xFFFFFF
#define C_TITLE_TOP  0xF6F6F6
#define C_TITLE_BOT  0xE9E9E9
#define C_HAIRLINE   0xD6D6D6
#define C_ACCENT     0x0A64D6
#define C_TERM_BG    0x1E1E1E

enum { W_TERM, W_FILES, W_TEXT, W_IMAGE, W_CALC, W_CLOCK, W_ABOUT };
enum { ANIM_NONE, ANIM_OPEN, ANIM_CLOSE, ANIM_MIN, ANIM_RESTORE, ANIM_ZOOM };

typedef struct {
    unsigned short ch;
    unsigned char  fg, bg;
} TCell;

typedef struct {
    char name[128];
    int  is_dir;
    u64  size;
} FileEnt;

typedef struct {
    int  used, kind, x, y, w, h, minimized;
    int  zoomed, zx, zy, zw, zh;  /* gezoomt: vorige Lage */
    /* Animation: das Fensterbild wird von Rechteck from nach to skaliert (und ein-/ausgeblendet) */
    int  anim;                    /* ANIM_* */
    s64  anim_t0;                 /* Beginn (Mikrosekunden) */
    int  anim_ms;
    int  from[4], to[4], last[4]; /* x, y, w, h; last = zuletzt gezeichnet (fuer das Neuzeichnen) */
    char title[80];
    /* Das Fenster wird in ein eigenes Bild gezeichnet (nur wenn sich sein Inhalt aendert); auf den Bildschirm wird es
     * nur kopiert (mit runden Ecken). Neu zu zeichnen ist das Rechteck [rx0, rx1) x [ry0, ry1) in Fensterkoordinaten. */
    Surface buf;
    int     rx0, ry0, rx1, ry1;
    /* Terminal */
    int    pid, to_child, from_child, cols, rows, cx, cy, fg, bg, esc, escn, escp[8], u8need, exited;
    int    tr0, tr1; /* geaenderte Zeilen (tr0 > tr1: keine) */
    unsigned u8cp;
    TCell *cells;
    /* Dateien */
    char     dir[256];
    FileEnt *ents;
    int      nent, scroll, sel;
    /* Text */
    char  *text;
    char **lines;
    int    nlines, top;
    /* Bild */
    Surface img;
    char    err[96];
    /* Rechner (Festkomma, 6 Nachkommastellen) */
    char disp[40];
    s64  acc, cur;
    char op;
    int  fresh, dec;
} Win;

/* Aktionen (Menues und Dock) */
enum { A_NONE, A_TERM, A_FILES, A_CALC, A_CLOCK, A_ABOUT, A_PAINT, A_SNAKE, A_TETRIS, A_QUIT, A_SEP,
       A_WIN_NEW, A_WIN_MIN, A_WIN_ZOOM, A_WIN_CLOSE };

typedef struct {
    const char *label;
    int         action;
} MenuItem;

/* ---------- main.c ---------- */

extern Win      wins[MAXW];
extern Win     *order[MAXW]; /* Stapel: order[nord-1] liegt oben und hat den Fokus */
extern int      nord;
extern Surface  bg, bg_blur; /* Hintergrundbild und weichgezeichnete Kopie (Milchglas von Menueleiste und Dock) */
extern Surface *tgt;         /* Ziel der draw_*-Funktionen */
extern int      W, H;
extern int      drag_mode;   /* 0 = nichts, 1 = verschieben, 2 = Groesse */
extern Win     *drag_win;
extern int      mouse_x, mouse_y;
extern s64      now_us;      /* Zeit dieses Bildes (Mikrosekunden) */
void do_action(int a);

/* ---------- chrome.c: Masse, Hintergrund, Menueleiste, Dock, Menues, Symbole ---------- */

extern int menu_open;  /* 0 = zu, 1 = Logo-Menue, 2 = Programm-Menue */
extern int menu_hover; /* Eintrag unter der Maus, -1 = keiner */
extern int dock_hover; /* Symbol unter der Maus, -1 = keins */

void ui_init(void);
void make_background(void);
void draw_menubar(void);
void draw_dock(void);
void draw_menu(void);
void damage_menubar(void);
void damage_dock(void);
void damage_menu(void);
int  menubar_hit(int x, int y);  /* 1 = Logo, 2 = Programmname, 0 = sonst */
int  menu_hit(int x, int y);     /* Eintrag im offenen Menue, -1 = keiner */
int  menu_action(int i);
int  dock_hit(int x, int y);     /* Symbol, -1 = keins */
void dock_click(int i);
void dock_hover_at(int x, int y);
int  dock_top(void);             /* Oberkante des Docks (Fenster enden darueber) */
void dock_slot_of(const Win *w, int *x, int *y, int *size); /* wo das minimierte Fenster im Dock liegt */
void dock_tick(s64 dt_us);       /* Vergroesserung unter der Maus weich nachfuehren */
void draw_app_icon(Surface *s, int kind, int x, int y, int size); /* Programmsymbol (W_* oder A_*) */
const char *app_name(const Win *w);

/* ---------- wm.c: geaenderte Bereiche, Fenster, Zusammensetzen ---------- */

void damage(int x, int y, int w, int h);
void damage_all(void);
void damage_win(const Win *w);
void win_dirty(Win *w, int x, int y, int ww, int hh);
void win_dirty_all(Win *w);
void content_rect(const Win *w, int *x, int *y, int *cw, int *ch);
Win *new_window(int kind, const char *title, int w, int h);
void raise_win(Win *w);
void minimize(Win *w);
void zoom_win(Win *w);
Win *focused(void);
void close_win(Win *w);      /* mit Animation; close_win_now sofort */
void close_win_now(Win *w);
void anim_tick(void);        /* laufende Fenster-Animationen weiterfuehren */
int  title_button_at(const Win *w, int x, int y); /* 1 schliessen, 2 minimieren, 3 zoomen, 0 keiner */
void set_button_hover(Win *w);                    /* Maus ueber den Knoepfen: Symbole zeigen */
void draw_all(void);

/* ---------- term.c ---------- */

void term_alloc(Win *w);
void open_terminal(void);
void term_poll(Win *w);
void term_flush(Win *w);
void term_key(Win *w, int k);
void draw_terminal(Win *w, int x, int y, int cw, int ch);

/* ---------- apps.c: Dateien, Text, Bild, Rechner, Uhr, Info ---------- */

extern const char *const calc_keys[20];

void open_files(const char *dir);
void files_open_entry(Win *w, int i);
void open_calc(void);
void open_clock(void);
void open_about(void);
void calc_key(Win *w, const char *k);
int  calc_btn_at(Win *w, int px, int py);
void draw_files(Win *w, int x, int y, int cw, int ch);
void draw_text(Win *w, int x, int y, int cw, int ch);
void draw_image(Win *w, int x, int y, int cw, int ch);
void draw_calc(Win *w, int x, int y, int cw, int ch);
void draw_clock(Win *w, int x, int y, int cw, int ch);
void draw_about(Win *w, int x, int y, int cw, int ch);

#endif
