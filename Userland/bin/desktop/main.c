/* Desktop: Eingaben und Hauptschleife (Ueberblick: desktop.h) */

#include "desktop.h"

Win      wins[MAXW];
Win     *order[MAXW]; /* Stapel: order[nord-1] liegt oben und hat den Fokus */
int      nord;
Surface  bg, bg_blur;
Surface *tgt = &gfx_screen; /* Ziel der draw_*-Funktionen */
int      W, H;
int      drag_mode; /* 0 = nichts, 1 = verschieben, 2 = Groesse */
Win     *drag_win;
int      mouse_x, mouse_y;
s64      now_us;

static int  drag_dx, drag_dy;
static s64  last_click_tick;
static int  last_click_x, last_click_y;
static int  quit;
static Win *app_grab; /* Fenster, in dessen Inhalt die Maustaste gedrueckt wurde: bekommt alles bis zum Loslassen */

/* ======================================================================================================================
 * Eingaben
 * ==================================================================================================================== */

static void close_menu(void)
{
    if (!menu_open)
        return;
    damage_menu();
    damage_menubar();
    menu_open = 0;
    menu_hover = -1;
}

void do_action(int a)
{
    close_menu();
    Win *f = focused();
    switch (a) {
    case A_TERM: launch_app("/bin/term", "term", A_TERM, "Terminal"); break;
    case A_FILES: launch_app("/bin/files", "files /", A_FILES, "Dateien"); break;
    case A_CALC: launch_app("/bin/calc", "calc", A_CALC, "Rechner"); break;
    case A_CLOCK: launch_app("/bin/clock", "clock", A_CLOCK, "Uhr"); break;
    case A_ABOUT: launch_app("/bin/about", "about", A_ABOUT, "Info"); break;
    case A_PAINT: launch_app("/bin/paint", "paint", A_PAINT, "Malen"); break;
    case A_SNAKE: launch_app("/bin/snake", "snake", A_SNAKE, "Snake"); break;
    case A_TETRIS: launch_app("/bin/tetris", "tetris", A_TETRIS, "Tetris"); break;
    case A_QUIT: quit = 1; break;
    case A_WIN_NEW: /* noch eins vom aktiven Programm (Text- und Bildansicht brauchen eine Datei: dann ein Terminal) */
        do_action(f && f->app > A_NONE && f->app < A_QUIT ? f->app : A_TERM);
        break;
    case A_WIN_MIN: if (f) minimize(f); break;
    case A_WIN_ZOOM: if (f) zoom_win(f); break;
    case A_WIN_CLOSE: if (f) close_win(f); break;
    }
}

static Win *window_at(int x, int y)
{
    for (int i = nord - 1; i >= 0; i--) {
        Win *w = order[i];
        if (!w->minimized && w->anim != ANIM_CLOSE && x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h)
            return w;
    }
    return 0;
}

static int resize_corner(const Win *w, int x, int y)
{
    return (w->flags & WPF_RESIZABLE) && x >= w->x + w->w - U(16) && y >= w->y + w->h - U(16);
}

static void mouse_down(Event *e)
{
    if (menu_open) {
        int i = menu_hit(e->x, e->y);
        if (i >= 0) {
            do_action(menu_action(i));
            return;
        }
        int m = menubar_hit(e->x, e->y), was = menu_open;
        close_menu();
        if (m && m != was) { /* anderes Menue der Leiste: gleich oeffnen */
            menu_open = m;
            damage_menu();
            damage_menubar();
        }
        return;
    }
    if (e->y < MENUBAR_H) { /* Menueleiste */
        int m = menubar_hit(e->x, e->y);
        if (m) {
            menu_open = m;
            menu_hover = -1;
            damage_menu();
            damage_menubar();
        }
        return;
    }
    int d = dock_hit(e->x, e->y);
    if (d >= 0) {
        dock_click(d);
        return;
    }
    Win *w = window_at(e->x, e->y);
    if (!w)
        return;
    if (w != focused())
        raise_win(w);
    int dbl = e->button == 1 && sys_ticks() - last_click_tick < 40 && e->x - last_click_x < 5 &&
              last_click_x - e->x < 5 && e->y - last_click_y < 5 && last_click_y - e->y < 5;
    if (e->button == 1) {
        last_click_tick = dbl ? 0 : sys_ticks();
        last_click_x = e->x;
        last_click_y = e->y;
    }
    if (e->y < w->y + TITLE_H) {
        if (e->button != 1)
            return;
        int b = title_button_at(w, e->x, e->y);
        if (b == 1) {
            close_win(w);
        } else if (b == 2) {
            minimize(w);
        } else if (b == 3 || dbl) { /* gruener Knopf oder Doppelklick auf die Titelleiste */
            zoom_win(w);
        } else {
            drag_mode = 1;
            drag_win = w;
            drag_dx = e->x - w->x;
            drag_dy = e->y - w->y;
        }
        return;
    }
    if (e->button == 1 && resize_corner(w, e->x, e->y)) {
        drag_mode = 2;
        drag_win = w;
        drag_dx = w->x + w->w - e->x;
        drag_dy = w->y + w->h - e->y;
        return;
    }
    app_input(w, EV_DOWN, 0, e->x, e->y, e->button, 0); /* Inhalt gehoert dem Programm */
    app_grab = w;
}

/* Symbole in den drei Knoepfen, wenn die Maus darueber steht (auch nach Klicks, die Fenster bewegen) */
static void update_button_hover(int x, int y)
{
    Win *w = window_at(x, y);
    set_button_hover(w && y < w->y + TITLE_H && x < w->x + U(70) ? w : 0);
}

static void mouse_move(Event *e)
{
    mouse_x = e->x;
    mouse_y = e->y;
    if (menu_open) { /* Hervorhebung im Menue */
        int h = menu_hit(e->x, e->y);
        if (h != menu_hover) {
            menu_hover = h;
            damage_menu();
        }
    }
    if (!drag_mode) {
        dock_hover_at(e->x, e->y);
        update_button_hover(e->x, e->y);
        Win *a = app_grab ? app_grab : window_at(e->x, e->y);
        if (a && (app_grab || e->y >= a->y + TITLE_H))
            app_input(a, EV_MOVE, 0, e->x, e->y, 0, 0);
        return;
    }
    Win *w = drag_win;
    damage_win(w); /* alte Stelle */
    if (drag_mode == 1) {
        w->x = e->x - drag_dx;
        w->y = e->y - drag_dy;
        w->zoomed = 0;
        if (w->y < MENUBAR_H) w->y = MENUBAR_H;
        if (w->y > H - TITLE_H) w->y = H - TITLE_H;
        if (w->x > W - U(60)) w->x = W - U(60);
        if (w->x + w->w < U(60)) w->x = U(60) - w->w;
    } else { /* Groesse: das Programm erfaehrt sie nach dem Bild (apps_frame) und zeichnet neu */
        int nw = e->x + drag_dx - w->x, nh = e->y + drag_dy - w->y;
        w->w = nw < U(200) ? U(200) : nw;
        w->h = nh < U(140) ? U(140) : nh;
        w->zoomed = 0;
        win_dirty_all(w);
    }
    damage_win(w); /* neue Stelle */
}

static void key(int k)
{
    if (k == 0x1B && menu_open) {
        close_menu();
        return;
    }
    Win *w = focused();
    if (w)
        app_input(w, EV_KEY, k, 0, 0, 0, 0);
}

static void wheel(Event *e)
{
    Win *w = window_at(e->x, e->y);
    if (w)
        app_input(w, EV_WHEEL, 0, e->x, e->y, 0, e->wheel);
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (gfx_open() != 0)
        sys_exit(1);
    sys_tty_fg(0); /* Strg+C geht an die Fenster, nicht an den Desktop */
    W = gfx_screen.w;
    H = gfx_screen.h;
    now_us = sys_time_us();
    desk_init();
    make_background();
    sys_service_register("desktop"); /* Grafikprogramme aus dem Terminal finden ihn so */
    do_action(A_TERM);
    damage_all();
    draw_all();

    s64 last_min = -1;
    Win *last_focus = focused();
    while (!quit) {
        s64 prev_us = now_us;
        now_us = sys_time_us();
        Event e;
        int n = 0;
        while (n < 64 && gfx_poll(&e)) {
            n++;
            if (e.type == EV_KEY) key(e.key);
            else if (e.type == EV_DOWN) { mouse_down(&e); update_button_hover(e.x, e.y); }
            else if (e.type == EV_UP) {
                drag_mode = 0;
                if (app_grab && app_grab->used)
                    app_input(app_grab, EV_UP, 0, e.x, e.y, e.button, 0);
                app_grab = 0;
            }
            else if (e.type == EV_MOVE) mouse_move(&e);
            else if (e.type == EV_WHEEL) wheel(&e);
        }
        apps_poll();
        if (app_grab && !app_grab->used)
            app_grab = 0;
        Win *f = focused();
        if (f != last_focus) { /* Titelleiste und Schatten (aktiv kraeftiger) beider Fenster; die Programme erfahren es */
            for (int i = 0; i < nord; i++)
                if (order[i] == last_focus) {
                    win_dirty(last_focus, 0, 0, last_focus->w, TITLE_H);
                    damage_win(last_focus);
                    app_focus(last_focus, 0);
                }
            if (f) {
                win_dirty(f, 0, 0, f->w, TITLE_H);
                damage_win(f);
                app_focus(f, 1);
            }
            damage_menubar();
            last_focus = f;
        }
        s64 now = sys_time();
        if (now / 60 != last_min) { /* Uhrzeit in der Menueleiste */
            last_min = now / 60;
            damage_menubar();
        }
        anim_tick();
        dock_tick(now_us - prev_us);
        draw_all(); /* direkt nach dem Bildwechsel: was sich geaendert hat, steht bis zum naechsten Bild */
        gfx_vsync();
        apps_frame();
    }
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used)
            close_win_now(&wins[i]);
    apps_quit();
    gfx_close();
    sys_exit(0);
}
