#ifndef USERLAND_BIN_DESKTOP_DESKTOP_H
#define USERLAND_BIN_DESKTOP_DESKTOP_H

/* Gemeinsame Typen, Zustand und Funktionen des Desktops (Userland/bin/desktop/). */

#include "gfx.h"
#include "malloc.h"
#include "ui.h"
#include "winproto.h"

/* desktop: grafische Oberflaeche (hell).
 *   Unten die Taskleiste aus vier freistehenden Segmenten: Programme (Strich = laeuft, lang und blau = aktiv) und
 *   minimierte Fenster | Suche, Startmenue, Fenstermenue | Uhrzeit und Datum | Netzwerk, Lautstaerke, Systemmenue.
 *   Fenster: runde Ecken, Schatten, links die drei Knoepfe (schliessen, minimieren, zoomen). Verschieben an der
 *   Titelleiste, Groesse aendern an der Ecke unten rechts (wenn das Programm es erlaubt).
 *   Jedes Fenster gehoert einem eigenen Prozess (client.c, Protokoll in winproto.h): Terminal (term), Dateien (files),
 *   Texteditor (textedit), Musik (music), Bildansicht (view), Rechner (calc), Uhr (clock), Info (about), Malen (paint), Snake und
 *   Tetris. Der Desktop zeichnet nur Rahmen und Taskleiste. Grafikprogramme, die man im Terminal startet,
 *   melden sich ueber den Dienst "desktop" und bekommen ebenfalls ein Fenster. "Zur Konsole" beendet den Desktop.
 * Alle Masse sind fuer 1920x1080 angegeben und werden mit U() (ui.h) an groessere Bildschirme angepasst. */

#define MAXW 16

/* Masse des Desktops (chrome.c: desk_init); Schriften und Farben in ui.h */
extern int MENUBAR_H, TITLE_H, DOCK_H, RADIUS, SHADOW; /* MENUBAR_H: oberer Rand der Fensterflaeche (0) */

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
    /* Rahmen (1 px, dunkel) ist ins Bild eingezeichnet, in den R oberen und unteren Zeilen steht in Byte 3 die Deckung
     * der runden Ecken - so kann auch die GPU das Fenster aufsetzen (gpu.c). Dazu der Schatten als Bild: */
    Surface shd_tb, shd_lr;
    int     shd_w, shd_h, shd_a;      /* fuer diese Groesse und Staerke berechnet */
    /* Das Programm dahinter (client.c) */
    int  pid;
    int  app;                     /* Aktion (A_*) bzw. Symbol (ICON_*), mit der es gestartet wurde */
    char name[32];                /* Programmname (Fenstermenue, Alt+Q) */
    int  flags;                   /* WPF_* */
    int  app_in, app_out;         /* Pipes */
    int  app_w, app_h;            /* Groesse des geteilten Inhalts */
    u32 *app_px;
    int  app_frame, app_done;     /* wartet auf WP_FRAME; Pipe ist zu */
    int  app_move, app_mx, app_my; /* gesammelte Mausbewegung (eine je Bild) */
    int  app_close_n;             /* so oft wurde um das Schliessen gebeten */
    int  req_w, req_h, req_zoom;  /* zuletzt mit WP_RESIZE gemeldete Groesse und Zustand */
    char open_path[WP_PATH_MAX];  /* WP_OPEN: Pfad wird aus Stuecken zusammengesetzt */
} Win;

/* Schattenbilder (gpu.c): Streifen beginnen an Vielfachen von 8 (die GPU skaliert sie in Bloecken von 8 x 8) */
static inline int shadow_b8(void) { return (SHADOW + RADIUS + 7) & ~7; }

/* Hoehe der Titelleiste (0 bei rahmenlosen Fenstern, die zeichnen ihre Knoepfe selbst) */
static inline int win_th(const Win *w) { return (w->flags & WPF_FRAMELESS) ? 0 : TITLE_H; }

/* Aktionen (Menues und Taskleiste). Die ersten sind zugleich die Programmsymbole (ICON_* in ui.h). */
enum { A_NONE, A_TERM, A_FILES, A_CALC, A_CLOCK, A_ABOUT, A_PAINT, A_SNAKE, A_TETRIS, A_EDIT, A_MUSIC, A_QUIT = 20, A_SEP,
       A_WIN_NEW, A_WIN_MIN, A_WIN_ZOOM, A_WIN_CLOSE, A_APP_QUIT, A_SNAP_LEFT, A_SNAP_RIGHT, A_NEXT_WIN,
       A_RESTART, A_POWEROFF, A_INFO, A_NET_DHCP, A_SEARCH, A_WINSEL = 64 };
/* A_INFO: Zeile nur zum Lesen (Name links, Wert rechts); A_SEARCH: Suchfeld im Startmenue; A_WINSEL + i: Fenster wins[i] */

typedef struct {
    const char *label;
    int         action;
    const char *keys;  /* Tastenkuerzel (nur zur Anzeige) oder 0 */
} MenuItem;

/* ---------- main.c ---------- */

extern Win      wins[MAXW];
extern Win     *order[MAXW]; /* Stapel: order[nord-1] liegt oben und hat den Fokus */
extern int      nord;
extern Surface  bg, bg_blur; /* Hintergrundbild und weichgezeichnete Kopie (Milchglas von Taskleiste und Menues) */
extern Surface *tgt;         /* Ziel der draw_*-Funktionen */
extern int      W, H;
extern int      drag_mode;   /* 0 = nichts, 1 = verschieben, 2 = Groesse */
extern Win     *drag_win;
extern int      mouse_x, mouse_y;
extern s64      now_us;      /* Zeit dieses Bildes (Mikrosekunden) */
void do_action(int a);
void window_cmd(Win *w, int cmd); /* WP_WINCMD eines rahmenlosen Fensters */

/* ---------- chrome.c: Masse, Hintergrund, Taskleiste, Menues ---------- */

extern int menu_open;  /* 0 = zu, 1 = Start, 2 = Fenster, 3 = Netzwerk, 4 = System */
extern int menu_hover; /* Eintrag unter der Maus, -1 = keiner */
extern int dock_hover; /* Symbol unter der Maus, -1 = keins */

void desk_init(void);
void make_background(void);
void draw_dock(void);
void draw_menu(void);
void damage_menubar(void);      /* = damage_dock (Uhrzeit, Fokus, Netzwerk stehen in der Taskleiste) */
void damage_dock(void);
void dock_zone(int *r);
void damage_dock_seg(int i);     /* nur ein Segment der Taskleiste neu (0 Programme, 2 Uhr, 3 Netz/Ton/System) */
void damage_menu_button(void);   /* Hervorhebung des Menue-Knopfs und Name unter der Maus */          /* x, y, w, h: Streifen unten mit Taskleiste und den Namen darueber */
int  menu_zone(int *r);          /* offenes Menue samt Schatten; 0 = keins */
void damage_menu(void);
int  menubar_hit(int x, int y);  /* Knopf mit Menue: 1 Start, 2 Fenster, 3 Netzwerk, 4 System, 5 Suche; 0 = keiner */
int  menu_current(void);         /* offenes Menue wie menubar_hit (5 = Start ueber die Suche) */
void open_menu(int m);
int  menu_key(int k);            /* Taste fuer das offene Menue (Pfeile, Enter, Suche); 1 = verbraucht */
int  dock_wheel(int x, int y, int delta); /* Mausrad ueber der Lautstaerke; 1 = verbraucht */
int  menu_hit(int x, int y);     /* Eintrag im offenen Menue, -1 = keiner */
int  menu_inside(int x, int y);  /* liegt der Punkt im offenen Menue? */
void net_tick(void);             /* einmal je Sekunde den Netzwerkzustand holen (Symbol, Menue) */
void net_dhcp(void);             /* Adresse neu anfragen */
int  menu_action(int i);
int  dock_hit(int x, int y);     /* Slot oder Knopf der Taskleiste, -1 = nichts */
void dock_click(int i);
void dock_hover_at(int x, int y);
int  dock_top(void);             /* Oberkante der Taskleiste (Fenster enden darueber) */
void dock_slot_of(const Win *w, int *x, int *y, int *size); /* wo das minimierte Fenster in der Leiste liegt */

/* ---------- wm.c: geaenderte Bereiche, Fenster, Zusammensetzen ---------- */

void damage(int x, int y, int w, int h);
void damage_all(void);
void damage_win(const Win *w);
void overlay_dirty(int x, int y, int w, int h); /* Taskleiste/Menue/Dialog haben sich dort geaendert (Ebene fuer die GPU) */
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

/* ---------- gpu.c: Zusammensetzen auf der GPU ---------- */

extern int gpu_mode;   /* 0 = CPU, 1 = GPU, 2 = CPU-Ersatz im Kernel (gpucomp=soft) */
extern FrameProf prof; /* Aufschluesselung des laufenden Bildes (an den Kernel: igdtest comp) */
int  gpu_init(void);   /* nach gfx_open: Bildschirmbild in geteilten Speicher */
void gpu_quit(void);   /* vor gfx_close */
int  gsurf_new(Surface *s, int w, int h); /* Flaeche, die die GPU benutzen kann (sonst wie surface_new) */
void gsurf_free(Surface *s);
int  gsurf_handle(const Surface *s);      /* Nummer beim Kernel, 0 = nur CPU */
int  gsurf_width(int w);                  /* Breite fuer gsurf_new: die GPU braucht Vielfache von 16 Pixeln */
int  gsurf_height(int h);                 /* Hoehe fuer Fensterbilder: Vielfache von 8 (Skalieren in Bloecken) */
void gq_copy(const Surface *d, int dx, int dy, const Surface *s, int sx, int sy, int w, int h, const Clip *c);
void gq_blend(const Surface *d, int dx, int dy, const Surface *s, int sx, int sy, int w, int h, const Clip *c);
void gq_blend_a(const Surface *d, int dx, int dy, const Surface *s, int sx, int sy, int w, int h, int alpha,
                const Clip *c);       /* mischen mit Deckung alpha (0-256) */
void gq_scale(int vertical, const Surface *d, int dx, int dy, const Surface *s, int sx, int sy, int w, int h, int step);
Surface *anim_temps(void);  /* 4 Hilfsflaechen fuer Animationen (Bild senkrecht, waagerecht, Schatten oben/unten,
                             * links/rechts), 0 = keine */
void gq_cancel(void);
int  gq_submit(void);  /* 0 = alles fertig gezeichnet, sonst muss die CPU den Bereich zeichnen */
void gq_present(int x, int y, int w, int h); /* Rechteck des Bildschirmbilds von der GPU anzeigen lassen */
int  gq_submit_async(void); /* abschicken ohne zu warten (0 = angenommen); gpu_wait vor dem naechsten Bild */
void gpu_wait(void);        /* die zuletzt abgeschickte Liste abwarten */
void gpu_stat(int gpu, s64 us, s64 px);
int  shadow_ready(Win *w, int alpha);     /* Schattenbild passend zu Groesse und Staerke; 0 = geht nicht */
void shadow_free(Win *w);

/* ---------- dialog.c: Ausschalten / Neu starten ---------- */

extern int dialog_kind;               /* 0 = keiner */
void dialog_open(int action);         /* A_POWEROFF oder A_RESTART */
void dialog_zone(int *r);             /* Kasten samt Schatten (x, y, w, h) */
#define DIALOG_DIM 70                 /* so stark wird beim Dialog abgedunkelt */
extern int ov_pass;                   /* gerade wird die Ebene fuer die GPU gezeichnet (wm.c) */
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
