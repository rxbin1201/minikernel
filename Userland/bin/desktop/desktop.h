#ifndef USERLAND_BIN_DESKTOP_DESKTOP_H
#define USERLAND_BIN_DESKTOP_DESKTOP_H

/* Gemeinsame Typen, Zustand und Funktionen des Desktops (Userland/bin/desktop/). */

#include "gfx.h"
#include "malloc.h"

/* desktop: grafische Oberflaeche mit Fenstern.
 *   Fenster: Terminal (eine echte Shell), Dateien, Textansicht, Bildansicht, Rechner, Uhr, Info.
 *   Fenster verschieben (Titelleiste ziehen), Groesse aendern (Ecke unten rechts), minimieren (_), schliessen (x).
 *   Taskleiste mit Startmenue, Fensterknoepfen und Uhr. Vollbild-Programme (Malen, Snake, Tetris) startet das Startmenue.
 *   "Zur Konsole" im Startmenue beendet den Desktop. */

#define TITLE_H   22
#define BORDER    3
#define TASKBAR_H 30
#define MAXW      16
#define ROW_H     18

enum { W_TERM, W_FILES, W_TEXT, W_IMAGE, W_CALC, W_CLOCK, W_ABOUT };

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
    char title[80];
    /* Das Fenster wird in ein eigenes Bild gezeichnet (nur wenn sich sein Inhalt aendert); auf den Bildschirm wird es
     * nur kopiert. Neu zu zeichnen ist das Rechteck [rx0, rx1) x [ry0, ry1) in Fensterkoordinaten. */
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

/* Startmenue */
typedef struct {
    const char *label;
    int         action;
} MenuItem;

enum { A_TERM, A_FILES, A_CALC, A_CLOCK, A_ABOUT, A_PAINT, A_SNAKE, A_TETRIS, A_QUIT, A_SEP };

/* ---------- main.c ---------- */

extern Win      wins[MAXW];
extern Win     *order[MAXW]; /* Stapel: order[nord-1] liegt oben und hat den Fokus */
extern int      nord;
extern Surface  bg;
extern Surface *tgt;         /* Ziel der draw_*-Funktionen */
extern int      W, H, menu_open, menu_hover;
extern int      drag_mode;   /* 0 = nichts, 1 = verschieben, 2 = Groesse */
extern Win     *drag_win;

/* ---------- wm.c: geaenderte Bereiche, Fenster, Zeichnen der Oberflaeche ---------- */

extern const MenuItem menu[];

void damage(int x, int y, int w, int h);
void damage_all(void);
void damage_taskbar(void);
void damage_win(const Win *w);
void damage_menu(void);
void win_dirty(Win *w, int x, int y, int ww, int hh);
void win_dirty_all(Win *w);
void content_rect(const Win *w, int *x, int *y, int *cw, int *ch);
Win *new_window(int kind, const char *title, int w, int h);
void raise_win(Win *w);
void minimize(Win *w);
Win *focused(void);
void close_win(Win *w);
void make_background(void);
int  menu_hit(int x, int y);
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
void calc_key(Win *w, const char *k);
int  calc_btn_at(Win *w, int px, int py);
void draw_files(Win *w, int x, int y, int cw, int ch);
void draw_text(Win *w, int x, int y, int cw, int ch);
void draw_image(Win *w, int x, int y, int cw, int ch);
void draw_calc(Win *w, int x, int y, int cw, int ch);
void draw_clock(Win *w, int x, int y, int cw, int ch);
void draw_about(Win *w, int x, int y, int cw, int ch);

#endif
