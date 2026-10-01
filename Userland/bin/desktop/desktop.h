#ifndef USERLAND_BIN_DESKTOP_DESKTOP_H
#define USERLAND_BIN_DESKTOP_DESKTOP_H

/* Gemeinsame Typen, Zustand und Funktionen des Desktops (Userland/bin/desktop/). */

#include "gfx.h"
#include "malloc.h"
#include "ui.h"
#include "winproto.h"

/* desktop: grafische Oberflaeche im Stil von macOS (hell).
 *   Oben die Menueleiste (Logo-Menue, Menue des aktiven Programms, Lautstaerke, Uhr), unten das Dock mit den
 *   Programmen (Punkt = laeuft) und rechts davon die minimierten Fenster.
 *   Fenster: runde Ecken, Schatten, links die drei Knoepfe (schliessen, minimieren, zoomen). Verschieben an der
 *   Titelleiste, Groesse aendern an der Ecke unten rechts (wenn das Programm es erlaubt).
 *   Jedes Fenster gehoert einem eigenen Prozess (client.c, Protokoll in winproto.h): Terminal (term), Dateien (files),
 *   Texteditor (textedit), Musik (music), Bildansicht (view), Rechner (calc), Uhr (clock), Info (about), Malen (paint), Snake und
 *   Tetris. Der Desktop zeichnet nur Rahmen, Menueleiste und Dock. Grafikprogramme, die man im Terminal startet,
 *   melden sich ueber den Dienst "desktop" und bekommen ebenfalls ein Fenster. "Zur Konsole" beendet den Desktop.
 * Alle Masse sind fuer 1920x1080 angegeben und werden mit U() (ui.h) an groessere Bildschirme angepasst. */

#define MAXW 16

/* Masse des Desktops (chrome.c: desk_init); Schriften und Farben in ui.h */
extern int MENUBAR_H, TITLE_H, DOCK_H, RADIUS, SHADOW;

enum { ANIM_NONE, ANIM_OPEN, ANIM_CLOSE, ANIM_MIN, ANIM_RESTORE, ANIM_ZOOM };
enum { SNAP_NONE, SNAP_MAX, SNAP_LEFT, SNAP_RIGHT }; /* ganzer Bildschirm, linke/rechte Haelfte */

typedef struct {
    int  used, x, y, w, h, minimized;
    int  zoomed, zx, zy, zw, zh;  /* angedockt (SNAP_*, 0 = frei); zx.. = Lage davor */
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
    /* Das Programm dahinter (client.c) */
    int  pid;
    int  app;                     /* Aktion (A_*) bzw. Symbol (ICON_*), mit der es gestartet wurde */
    char name[32];                /* Programmname fuer die Menueleiste */
    int  flags;                   /* WPF_* */
    int  app_in, app_out;         /* Pipes */
    int  app_w, app_h;            /* Groesse des geteilten Inhalts */
    u32 *app_px;
    int  app_frame, app_done;     /* wartet auf WP_FRAME; Pipe ist zu */
    int  app_move, app_mx, app_my; /* gesammelte Mausbewegung (eine je Bild) */
    int  app_close_n;             /* so oft wurde um das Schliessen gebeten */
    int  req_w, req_h;            /* zuletzt mit WP_RESIZE verlangte Groesse */
    char open_path[WP_PATH_MAX];  /* WP_OPEN: Pfad wird aus Stuecken zusammengesetzt */
} Win;

/* Aktionen (Menues und Dock). Die ersten sind zugleich die Programmsymbole (ICON_* in ui.h). */
enum { A_NONE, A_TERM, A_FILES, A_CALC, A_CLOCK, A_ABOUT, A_PAINT, A_SNAKE, A_TETRIS, A_EDIT, A_MUSIC, A_QUIT = 20, A_SEP,
       A_WIN_NEW, A_WIN_MIN, A_WIN_ZOOM, A_WIN_CLOSE, A_APP_QUIT, A_SNAP_LEFT, A_SNAP_RIGHT, A_NEXT_WIN,
       A_RESTART, A_POWEROFF };

typedef struct {
    const char *label;
    int         action;
    const char *keys;  /* Tastenkuerzel (nur zur Anzeige) oder 0 */
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

/* ---------- chrome.c: Masse, Hintergrund, Menueleiste, Dock, Menues ---------- */

extern int menu_open;  /* 0 = zu, 1 = Logo-Menue, 2 = Programm-Menue */
extern int menu_hover; /* Eintrag unter der Maus, -1 = keiner */
extern int dock_hover; /* Symbol unter der Maus, -1 = keins */

void desk_init(void);
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
const char *app_name(const Win *w);

/* ---------- wm.c: geaenderte Bereiche, Fenster, Zusammensetzen ---------- */

void damage(int x, int y, int w, int h);
void damage_all(void);
void damage_win(const Win *w);
void win_dirty(Win *w, int x, int y, int ww, int hh);
void win_dirty_all(Win *w);
void content_rect(const Win *w, int *x, int *y, int *cw, int *ch);
Win *new_window(const char *title, int w, int h);
void raise_win(Win *w);
void minimize(Win *w);
void zoom_win(Win *w);       /* maximieren bzw. zurueck */
void snap_win(Win *w, int where);         /* SNAP_*: andocken (nur bei aenderbarer Groesse), SNAP_NONE = zurueck */
void snap_rect(int where, int *r);        /* x, y, w, h dafuer */
void set_snap_preview(int where);         /* Vorschau beim Ziehen an den Rand (SNAP_NONE = keine) */
void lower_win(Win *w);                   /* ganz nach hinten */
void cycle_windows(int dir);              /* Alt+Tab: 1 = naechstes nach vorn, -1 = zurueck */
Win *focused(void);
void close_win(Win *w);      /* fragt das Programm; mit Animation, wenn es weg ist */
void close_win_now(Win *w);
void anim_tick(void);        /* laufende Fenster-Animationen weiterfuehren */
int  title_button_at(const Win *w, int x, int y); /* 1 schliessen, 2 minimieren, 3 zoomen, 0 keiner */
void set_button_hover(Win *w);                    /* Maus ueber den Knoepfen: Symbole zeigen */
void draw_all(void);

/* ---------- dialog.c: Ausschalten / Neu starten ---------- */

extern int dialog_kind;               /* 0 = keiner */
void dialog_open(int action);         /* A_POWEROFF oder A_RESTART */
void draw_dialog(void);
void damage_dialog(void);
void dialog_mouse(int x, int y, int down);
void dialog_key(int k);
void power_tick(void);                /* jedes Bild: laufendes Ausschalten weiterfuehren */

/* ---------- client.c: die Programme hinter den Fenstern ---------- */

void launch_app(const char *path, const char *cmdline, int action, const char *name);
void apps_accept(void); /* Programme, die sich ueber den Dienst "desktop" melden (aus dem Terminal gestartet) */
void open_path(const char *path); /* Datei/Ordner mit dem passenden Programm */
void apps_poll(void);  /* Nachrichten der Programme lesen (jedes Bild) */
void apps_frame(void); /* nach dem Bildwechsel: WP_FRAME, Mausbewegungen, neue Groessen */
void app_input(Win *w, int type, int key, int x, int y, int button, int wheel);
void app_focus(Win *w, int on);
void app_request_close(Win *w);
void app_free(Win *w);
void apps_quit(void);
void draw_app(Win *w, int x, int y, int cw, int ch);

#endif
