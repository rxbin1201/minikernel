/* Desktop: Eingaben und Hauptschleife (Ueberblick: desktop.h) */

#include "desktop.h"

Win      wins[MAXW];
Win     *order[MAXW]; /* Stapel: order[nord-1] liegt oben und hat den Fokus */
int      nord;
Surface  bg;
Surface *tgt = &gfx_screen; /* Ziel der draw_*-Funktionen */
int      W, H, menu_open, menu_hover = -1;
int      drag_mode; /* 0 = nichts, 1 = verschieben, 2 = Groesse */
Win     *drag_win;

static int drag_dx, drag_dy;
static s64 last_click_tick;
static int last_click_x, last_click_y;
static int quit;

/* ======================================================================================================================
 * Eingaben
 * ==================================================================================================================== */

/* Startet ein Vollbild-Programm und kommt danach zurueck */
static void run_fullscreen(const char *path, const char *cmdline)
{
    gfx_suspend();
    s64 pid = sys_fork();
    if (pid == 0) {
        for (int fd = 3; fd < 64; fd++)
            sys_close(fd);
        sys_setpgid(0, 0);
        sys_exec(path, cmdline);
        sys_exit(127);
    }
    if (pid > 0) {
        int code;
        sys_wait((int)pid, &code);
    }
    gfx_resume();
    while (sys_getchar() >= 0) /* Tasten, die fuer das Programm gedacht waren, nicht weiterreichen */
        ;
    damage_all();
}

static void do_action(int a)
{
    damage_menu();
    damage_taskbar();
    menu_open = 0;
    switch (a) {
    case A_TERM: open_terminal(); break;
    case A_FILES: open_files("/"); break;
    case A_CALC: open_calc(); break;
    case A_CLOCK: new_window(W_CLOCK, "Uhr", 260, 300); break;
    case A_ABOUT: new_window(W_ABOUT, "Info", 400, 220); break;
    case A_PAINT: run_fullscreen("/bin/paint", "paint"); break;
    case A_SNAKE: run_fullscreen("/bin/snake", "snake"); break;
    case A_TETRIS: run_fullscreen("/bin/tetris", "tetris"); break;
    case A_QUIT: quit = 1; break;
    }
}

static Win *window_at(int x, int y)
{
    for (int i = nord - 1; i >= 0; i--) {
        Win *w = order[i];
        if (!w->minimized && x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h)
            return w;
    }
    return 0;
}

static void content_click(Win *w, int px, int py, int dbl)
{
    int x, y, cw, ch;
    content_rect(w, &x, &y, &cw, &ch);
    if (w->kind == W_FILES) {
        int i = w->scroll + (py - y) / ROW_H;
        if (i < w->nent) {
            w->sel = i;
            if (dbl)
                files_open_entry(w, i);
        }
    } else if (w->kind == W_CALC) {
        int b = calc_btn_at(w, px, py);
        if (b >= 0)
            calc_key(w, calc_keys[b]);
    }
    win_dirty_all(w);
}

static void mouse_down(Event *e)
{
    int y0 = H - TASKBAR_H;
    if (menu_open) {
        int i = menu_hit(e->x, e->y);
        if (i >= 0) {
            do_action(menu[i].action);
            return;
        }
        damage_menu();
        damage_taskbar();
        menu_open = 0;
        if (e->y < y0)
            return;
    }
    if (e->y >= y0) { /* Taskleiste */
        if (e->x < 78) {
            menu_open = !menu_open;
            menu_hover = -1;
            damage_menu();
            damage_taskbar();
            return;
        }
        int bx = 82;
        for (int i = 0; i < MAXW; i++) {
            Win *w = &wins[i];
            if (!w->used)
                continue;
            if (e->x >= bx && e->x < bx + 146) {
                if (w == focused())
                    minimize(w);
                else
                    raise_win(w);
                return;
            }
            bx += 150;
        }
        return;
    }
    Win *w = window_at(e->x, e->y);
    if (!w)
        return;
    if (w != focused())
        raise_win(w);
    if (e->button != 1)
        return;
    int dbl = sys_ticks() - last_click_tick < 40 && e->x - last_click_x < 5 && last_click_x - e->x < 5 &&
              e->y - last_click_y < 5 && last_click_y - e->y < 5;
    last_click_tick = dbl ? 0 : sys_ticks();
    last_click_x = e->x;
    last_click_y = e->y;
    if (e->y < w->y + TITLE_H) {
        if (e->x >= w->x + w->w - 22) {
            close_win(w);
        } else if (e->x >= w->x + w->w - 44) {
            minimize(w);
        } else {
            drag_mode = 1;
            drag_win = w;
            drag_dx = e->x - w->x;
            drag_dy = e->y - w->y;
        }
        return;
    }
    if (e->x >= w->x + w->w - 14 && e->y >= w->y + w->h - 14) {
        drag_mode = 2;
        drag_win = w;
        drag_dx = w->x + w->w - e->x;
        drag_dy = w->y + w->h - e->y;
        return;
    }
    content_click(w, e->x, e->y, dbl);
}

static void mouse_move(Event *e)
{
    if (menu_open) { /* Hervorhebung im Menue */
        int h = menu_hit(e->x, e->y);
        if (h != menu_hover) {
            menu_hover = h;
            damage_menu();
        }
    }
    if (!drag_mode)
        return;
    Win *w = drag_win;
    damage_win(w); /* alte Stelle */
    if (drag_mode == 1) {
        w->x = e->x - drag_dx;
        w->y = e->y - drag_dy;
        if (w->y < 0) w->y = 0;
        if (w->y > H - TASKBAR_H - TITLE_H) w->y = H - TASKBAR_H - TITLE_H;
        if (w->x > W - 60) w->x = W - 60;
        if (w->x + w->w < 60) w->x = 60 - w->w;
    } else {
        int nw = e->x + drag_dx - w->x, nh = e->y + drag_dy - w->y;
        w->w = nw < 180 ? 180 : nw;
        w->h = nh < 120 ? 120 : nh;
        if (w->kind == W_TERM)
            term_alloc(w);
        win_dirty_all(w);
    }
    damage_win(w); /* neue Stelle */
}

static void key(int k)
{
    if (k == 0x1B && menu_open) {
        damage_menu();
        damage_taskbar();
        menu_open = 0;
        return;
    }
    Win *w = focused();
    if (!w)
        return;
    if (w->kind == W_TERM) {
        term_key(w, k);
    } else if (w->kind == W_CALC) {
        char s[2] = {(char)k, 0};
        if (k == '\b' || k == 0x7F)
            calc_key(w, "\xE2\x86\x90");
        else if (k == 'c' || k == 'C' || k == 0x1B)
            calc_key(w, "C");
        else if (k == ',')
            calc_key(w, ".");
        else
            calc_key(w, s);
    } else if (w->kind == W_FILES) {
        if (k == KEY_DOWN && w->sel + 1 < w->nent) w->sel++;
        else if (k == KEY_UP && w->sel > 0) w->sel--;
        else if (k == '\n') files_open_entry(w, w->sel);
        else if (k == '\b' || k == 0x7F) files_open_entry(w, 0 < w->nent && strcmp(w->ents[0].name, "..") == 0 ? 0 : -1);
        int x, y, cw, ch;
        content_rect(w, &x, &y, &cw, &ch);
        int vis = ch / ROW_H;
        if (w->sel < w->scroll) w->scroll = w->sel;
        if (w->sel >= w->scroll + vis) w->scroll = w->sel - vis + 1;
        win_dirty_all(w);
    } else if (w->kind == W_TEXT) {
        int x, y, cw, ch;
        content_rect(w, &x, &y, &cw, &ch);
        int vis = (ch - 4) / 16;
        if (k == KEY_DOWN) w->top++;
        else if (k == KEY_UP) w->top--;
        else if (k == KEY_PGDN || k == ' ') w->top += vis;
        else if (k == KEY_PGUP) w->top -= vis;
        else if (k == KEY_HOME) w->top = 0;
        else if (k == KEY_END) w->top = w->nlines;
        if (w->top > w->nlines - vis) w->top = w->nlines - vis;
        if (w->top < 0) w->top = 0;
        win_dirty_all(w);
    }
}

static void wheel(Event *e)
{
    Win *w = window_at(e->x, e->y);
    if (!w)
        return;
    int x, y, cw, ch;
    content_rect(w, &x, &y, &cw, &ch);
    if (w->kind == W_FILES) {
        w->scroll -= e->wheel * 3;
        if (w->scroll > w->nent - ch / ROW_H) w->scroll = w->nent - ch / ROW_H;
        if (w->scroll < 0) w->scroll = 0;
    } else if (w->kind == W_TEXT) {
        int vis = (ch - 4) / 16;
        w->top -= e->wheel * 3;
        if (w->top > w->nlines - vis) w->top = w->nlines - vis;
        if (w->top < 0) w->top = 0;
    }
    win_dirty_all(w);
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
    make_background();
    open_terminal();
    damage_all();
    draw_all();

    s64 last_sec = -1, last_min = -1;
    Win *last_focus = focused();
    while (!quit) {
        Event e;
        int n = 0;
        while (n < 64 && gfx_poll(&e)) {
            n++;
            if (e.type == EV_KEY) key(e.key);
            else if (e.type == EV_DOWN) mouse_down(&e);
            else if (e.type == EV_UP) drag_mode = 0;
            else if (e.type == EV_MOVE) mouse_move(&e);
            else if (e.type == EV_WHEEL) wheel(&e);
        }
        for (int i = 0; i < MAXW; i++) {
            Win *w = &wins[i];
            if (w->used && w->kind == W_TERM) {
                term_poll(w);
                if (w->exited)
                    close_win(w);
                else
                    term_flush(w);
            }
        }
        Win *f = focused();
        if (f != last_focus) { /* Titelleiste (aktiv/inaktiv) und Terminal-Cursor beider Fenster */
            for (int i = 0; i < nord; i++)
                if (order[i] == last_focus)
                    win_dirty_all(last_focus);
            if (f)
                win_dirty_all(f);
            damage_taskbar();
            last_focus = f;
        }
        s64 now = sys_time();
        if (now != last_sec) { /* Uhren jede Sekunde */
            last_sec = now;
            for (int i = 0; i < nord; i++)
                if (order[i]->kind == W_CLOCK || order[i]->kind == W_ABOUT)
                    win_dirty_all(order[i]);
            if (now / 60 != last_min) { /* Uhrzeit in der Taskleiste */
                last_min = now / 60;
                damage(W - 60, H - TASKBAR_H, 60, TASKBAR_H);
            }
        }
        draw_all(); /* direkt nach dem Bildwechsel: was sich geaendert hat, steht bis zum naechsten Bild */
        gfx_vsync();
    }
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used)
            close_win(&wins[i]);
    gfx_close();
    sys_exit(0);
}
