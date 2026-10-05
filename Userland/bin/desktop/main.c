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
Settings cfg;

static int  drag_dx, drag_dy, press_x, press_y;
static int  snap_zone; /* beim Ziehen an den Rand: SNAP_* */
static s64  last_click_tick;
static int  mouse_held; /* linke Maustaste gedrueckt */
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
    damage_menu_button();
    menu_open = 0;
    menu_hover = -1;
}

void do_action(int a)
{
    if (is_wlan_action(a)) { /* WLAN-Menue: bleibt meist offen (Passwort, Fortschritt) */
        if (!wlan_action(a))
            close_menu();
        return;
    }
    close_menu();
    if (a >= A_WINSEL && a < A_WINSEL + MAXW) { /* Fenster aus dem Fenstermenue nach vorn */
        if (wins[a - A_WINSEL].used)
            raise_win(&wins[a - A_WINSEL]);
        return;
    }
    Win *f = focused();
    switch (a) {
    case A_TERM: launch_app("/bin/term", "term", A_TERM, "Terminal"); break;
    case A_FILES: launch_app("/bin/files", "files", A_FILES, "Dateien"); break; /* beginnt auf der Platte */
    case A_EDIT: launch_app("/bin/textedit", "textedit", A_EDIT, "Texteditor"); break;
    case A_MUSIC: launch_app("/bin/music", "music", A_MUSIC, "Musik"); break;
    case A_CALC: launch_app("/bin/calc", "calc", A_CALC, "Rechner"); break;
    case A_CLOCK: launch_app("/bin/clock", "clock", A_CLOCK, "Uhr"); break;
    case A_ABOUT: launch_app("/bin/about", "about", A_ABOUT, "Info"); break;
    case A_PAINT: launch_app("/bin/paint", "paint", A_PAINT, "Malen"); break;
    case A_SNAKE: launch_app("/bin/snake", "snake", A_SNAKE, "Snake"); break;
    case A_TETRIS: launch_app("/bin/tetris", "tetris", A_TETRIS, "Tetris"); break;
    case A_SETTINGS: launch_app("/bin/settings", "settings", A_SETTINGS, "Einstellungen"); break;
    case A_QUIT: quit = 1; break;
    case A_WIN_NEW: /* noch eins vom aktiven Programm (Text- und Bildansicht brauchen eine Datei: dann ein Terminal) */
        do_action(f && f->app > A_NONE && f->app < A_QUIT ? f->app : A_TERM);
        break;
    case A_WIN_MIN: if (f) minimize(f); break;
    case A_WIN_ZOOM: if (f) zoom_win(f); break;
    case A_WIN_CLOSE: if (f) close_win(f); break;
    case A_APP_QUIT: /* alle Fenster dieses Programms */
        if (f)
            for (int i = nord - 1; i >= 0; i--)
                if (order[i]->app == f->app && strcmp(order[i]->name, f->name) == 0)
                    close_win(order[i]);
        break;
    case A_SNAP_LEFT: if (f) snap_win(f, f->zoomed == SNAP_LEFT ? SNAP_NONE : SNAP_LEFT); break;
    case A_SNAP_RIGHT: if (f) snap_win(f, f->zoomed == SNAP_RIGHT ? SNAP_NONE : SNAP_RIGHT); break;
    case A_NEXT_WIN: cycle_windows(1); break;
    case A_RESTART:
    case A_POWEROFF: dialog_open(a); break;
    case A_NET_DHCP: net_dhcp(); break;
    }
}

/* Wunsch eines rahmenlosen Fensters (es zeichnet seine Knoepfe selbst) */
void window_cmd(Win *w, int cmd)
{
    if (!w->used || !(w->flags & WPF_FRAMELESS))
        return;
    switch (cmd) {
    case WPC_MOVE: /* wie ein Griff an der Titelleiste - nur, solange die Taste noch gedrueckt ist */
        if (!mouse_held || drag_mode)
            return;
        if (app_grab == w) /* das Programm bekommt die Taste los, ab jetzt zieht der Desktop */
            app_input(w, EV_UP, 0, mouse_x, mouse_y, 1, 0);
        app_grab = 0;
        drag_mode = 1;
        drag_win = w;
        drag_dx = mouse_x - w->x;
        drag_dy = mouse_y - w->y;
        press_x = mouse_x;
        press_y = mouse_y;
        break;
    case WPC_MINIMIZE: minimize(w); break;
    case WPC_ZOOM: zoom_win(w); break;
    case WPC_CLOSE: close_win(w); break;
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

/* Tastenkuerzel des Desktops (linke Alt-Taste); 0 = keins, die Taste geht an das Programm */
static int shortcut(int k)
{
    int c = k & 0xFF, shift = k & KEY_MOD_SHIFT;
    Win *f = focused();
    switch (c) {
    case '\t': cycle_windows(shift ? -1 : 1); return 1;
    case 'w': do_action(A_WIN_CLOSE); return 1;
    case 'q': do_action(A_APP_QUIT); return 1;
    case 'm': do_action(A_WIN_MIN); return 1;
    case 'n': do_action(A_WIN_NEW); return 1;
    case 'f': do_action(A_WIN_ZOOM); return 1;
    case KEY_LEFT: do_action(A_SNAP_LEFT); return 1;
    case KEY_RIGHT: do_action(A_SNAP_RIGHT); return 1;
    case KEY_UP: if (f) snap_win(f, SNAP_MAX); return 1;
    case KEY_DOWN: if (f) snap_win(f, SNAP_NONE); return 1;
    }
    return 0;
}

static int resize_corner(const Win *w, int x, int y)
{
    return (w->flags & WPF_RESIZABLE) && x >= w->x + w->w - U(16) && y >= w->y + w->h - U(16);
}

static void mouse_down(Event *e)
{
    if (dialog_kind) { /* Rueckfrage offen: nur ihre Knoepfe */
        dialog_mouse(e->x, e->y, e->button == 1);
        return;
    }
    if (menu_open) {
        int i = menu_hit(e->x, e->y);
        if (i >= 0) {
            do_action(menu_action(i));
            return;
        }
        if (menu_inside(e->x, e->y)) /* Infozeile oder Trennstrich: Menue bleibt offen */
            return;
        int m = menubar_hit(e->x, e->y), was = menu_current();
        close_menu();
        if (m && m != was) /* anderer Knopf der Leiste: dessen Menue gleich oeffnen */
            open_menu(m);
        return;
    }
    int m = menubar_hit(e->x, e->y);
    if (m) { /* Knopf der Taskleiste mit Menue */
        open_menu(m);
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
    if (e->y < w->y + win_th(w)) {
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
            press_x = e->x;
            press_y = e->y;
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
    app_input(w, EV_DOWN, e->key, e->x, e->y, e->button, 0); /* Inhalt gehoert dem Programm (key: Umschalttasten) */
    app_grab = w;
}

/* Symbole in den drei Knoepfen, wenn die Maus darueber steht (auch nach Klicks, die Fenster bewegen) */
static void update_button_hover(int x, int y)
{
    Win *w = window_at(x, y);
    set_button_hover(w && y < w->y + win_th(w) && x < w->x + U(70) ? w : 0);
}

static void mouse_move(Event *e)
{
    mouse_x = e->x;
    mouse_y = e->y;
    if (dialog_kind) {
        dialog_mouse(e->x, e->y, 0);
        return;
    }
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
        int inside = a && (app_grab || e->y >= a->y + win_th(a));
        static Win *last_in; /* Maus hat ein Fenster verlassen: noch eine Bewegung (ausserhalb), damit es Hervorhebungen loescht */
        if (last_in && last_in != (inside ? a : 0) && last_in->used)
            app_input(last_in, EV_MOVE, 0, e->x, e->y, 0, 0);
        last_in = inside ? a : 0;
        if (inside)
            app_input(a, EV_MOVE, 0, e->x, e->y, 0, 0);
        return;
    }
    Win *w = drag_win;
    if (drag_mode == 1 && w->zoomed) { /* angedockt: erst ab ein paar Pixeln loesen, dann wieder in alter Groesse */
        int dx = e->x - press_x, dy = e->y - press_y;
        if (dx * dx + dy * dy < U(8) * U(8))
            return;
        damage_win(w);
        drag_dx = drag_dx * w->zw / (w->w ? w->w : 1); /* Maus bleibt an derselben Stelle der Titelleiste */
        w->w = w->zw;
        w->h = w->zh;
        w->zoomed = 0;
        win_dirty_all(w);
    }
    damage_win(w); /* alte Stelle */
    if (drag_mode == 1) {
        w->x = e->x - drag_dx;
        w->y = e->y - drag_dy;
        if (w->flags & WPF_RESIZABLE) { /* an den Rand gezogen: Vorschau zum Andocken */
            snap_zone = e->x <= U(2) ? SNAP_LEFT : e->x >= W - 1 - U(2) ? SNAP_RIGHT : e->y <= U(3) ? SNAP_MAX : SNAP_NONE;
            set_snap_preview(snap_zone);
        }
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
    if (dialog_kind) {
        dialog_key(k);
        return;
    }
    if (k == 0x1B && menu_open) {
        close_menu();
        return;
    }
    if (menu_key(k))
        return;
    if ((k & KEY_MOD_ALT) && shortcut(k)) {
        close_menu();
        return;
    }
    Win *w = focused();
    if (w)
        app_input(w, EV_KEY, k, 0, 0, 0, 0);
}

static void wheel(Event *e)
{
    if (dock_wheel(e->x, e->y, e->wheel))
        return;
    Win *w = window_at(e->x, e->y);
    if (w)
        app_input(w, EV_WHEEL, e->key, e->x, e->y, 0, e->wheel);
}

/* Einstellungen neu lesen (das Programm "Einstellungen" hat sie gespeichert) und uebernehmen. Die Groesse der
 * Oberflaeche gilt erst beim naechsten Start: die Titelleisten zeichnen die Programme selbst in ihrer Groesse */
void settings_changed(void)
{
    Settings old = cfg;
    settings_load(&cfg);
    if (cfg.cursor != old.cursor)
        gfx_set_cursor_size(cfg.cursor);
    if (cfg.wallpaper != old.wallpaper) {
        gpu_wait(); /* die GPU liest vielleicht noch aus dem alten Bild */
        make_background();
        damage_all();
    }
    if (cfg.dock != old.dock || cfg.date != old.date) {
        dock_metrics();
        fit_windows();
    } else if (cfg.seconds != old.seconds) {
        damage_dock();
    }
}

/* Aufloesung umschalten: vorgemerkt (WP_SETMODE kommt mitten im Lesen der Programme) und in der Hauptschleife
 * ausgefuehrt - den Bildschirm kurz abgeben (der Kernel schaltet nur um, wenn kein Programm ihn hat), dann mit der
 * neuen Groesse wieder holen und alles neu einpassen. Klappt das Wiederholen nicht, endet der Desktop. */
static int mode_req[3];

void change_mode(int w, int h, int hz100)
{
    mode_req[0] = w;
    mode_req[1] = h;
    mode_req[2] = hz100;
}

static void apply_mode(void)
{
    int w = mode_req[0], h = mode_req[1], hz100 = mode_req[2];
    mode_req[0] = 0;
    if (w <= 0 || h <= 0)
        return;
    gpu_wait();
    ov_free(); /* alles, was so gross wie der Bildschirm ist */
    anim_temps_free();
    gpu_quit();
    gfx_close();
    sys_setmode((u64)w, (u64)h, (u64)hz100);
    if (gfx_open() != 0)
        sys_exit(1);
    gpu_init();
    gfx_set_cursor_size(cfg.cursor);
    W = gfx_screen.w;
    H = gfx_screen.h;
    dock_metrics();
    make_background();
    fit_windows();
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    settings_load(&cfg);
    if (gfx_open() != 0)
        sys_exit(1);
    gfx_set_cursor_size(cfg.cursor);
    gpu_init(); /* Zusammensetzen auf der GPU, wenn es eine passende gibt */
    sys_tty_fg(0); /* Strg+C geht an die Fenster, nicht an den Desktop */
    W = gfx_screen.w;
    H = gfx_screen.h;
    now_us = sys_time_us();
    wlan_init(); /* vor desk_init: der WLAN-Knopf gehoert zur Taskleiste */
    desk_init();
    make_background();
    sys_service_register("desktop"); /* Grafikprogramme aus dem Terminal finden ihn so */
    do_action(A_TERM);
    damage_all();
    draw_all();

    s64 last_min = -1, last_sec = -1;
    Win *last_focus = focused();
    while (!quit) {
        now_us = sys_time_us();
        Event e;
        int n = 0;
        while (n < 64 && gfx_poll(&e)) {
            n++;
            if (e.type == EV_KEY) key(e.key);
            else if (e.type == EV_DOWN) {
                mouse_x = e.x;
                mouse_y = e.y;
                mouse_held |= e.button == 1;
                mouse_down(&e);
                update_button_hover(e.x, e.y);
            }
            else if (e.type == EV_UP) {
                mouse_held = 0;
                if (drag_mode == 1 && snap_zone && drag_win && drag_win->used)
                    snap_win(drag_win, snap_zone);
                snap_zone = SNAP_NONE;
                set_snap_preview(SNAP_NONE);
                drag_mode = 0;
                if (app_grab && app_grab->used)
                    app_input(app_grab, EV_UP, e.key, e.x, e.y, e.button, 0);
                app_grab = 0;
            }
            else if (e.type == EV_MOVE) mouse_move(&e);
            else if (e.type == EV_WHEEL) wheel(&e);
        }
        apps_poll();
        if (mode_req[0])
            apply_mode();
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
            damage_dock_seg(0); /* nur die Striche unter den Programmen */
            last_focus = f;
        }
        s64 now = sys_time();
        if (now / 60 != last_min || (cfg.seconds && now != last_sec)) { /* Uhrzeit in der Taskleiste */
            last_min = now / 60;
            last_sec = now;
            damage_dock_seg(2); /* nur die Uhr */
        }
        power_tick();
        net_tick();
        wlan_tick();
        anim_tick();
        draw_all(); /* direkt nach dem Bildwechsel: was sich geaendert hat, steht bis zum naechsten Bild */
        /* Programme, die auf ein Bild warten (gfx_vsync), schon jetzt weiterzeichnen lassen - sonst kaeme ihr naechstes
         * Bild erst nach dem uebernaechsten Bildwechsel an (die Runde liest Nachrichten gleich nach dem Warten) und sie
         * liefen nur mit dem halben Takt. Vorher die GPU abwarten: sie liest dann nicht mehr aus ihren Puffern. */
        gpu_wait();
        apps_frame();
        gfx_vsync();
    }
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used)
            close_win_now(&wins[i]);
    apps_quit();
    gpu_quit();
    gfx_close();
    sys_exit(0);
}
