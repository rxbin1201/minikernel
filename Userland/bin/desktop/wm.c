/* Desktop: geaenderte Bereiche, Fensterverwaltung, Zeichnen der Oberflaeche */

#include "desktop.h"

/* ======================================================================================================================
 * Geaenderte Bereiche
 *   damage():   Bildschirmrechteck, das neu zusammengesetzt und angezeigt werden muss
 *   win_dirty(): Teil eines Fensters (Fensterkoordinaten), der in dessen Bild neu gezeichnet werden muss
 * ==================================================================================================================== */

#define MAXD 24

static Clip dmg[MAXD];

static int  ndmg;

void damage(int x, int y, int w, int h)
{
    Clip r = {x < 0 ? 0 : x, y < 0 ? 0 : y, x + w > W ? W : x + w, y + h > H ? H : y + h};
    if (r.x0 >= r.x1 || r.y0 >= r.y1)
        return;
    for (int i = 0; i < ndmg;) { /* ueberlappende Rechtecke zusammenfassen */
        Clip *d = &dmg[i];
        if (d->x0 <= r.x1 && r.x0 <= d->x1 && d->y0 <= r.y1 && r.y0 <= d->y1) {
            if (d->x0 < r.x0) r.x0 = d->x0;
            if (d->y0 < r.y0) r.y0 = d->y0;
            if (d->x1 > r.x1) r.x1 = d->x1;
            if (d->y1 > r.y1) r.y1 = d->y1;
            dmg[i] = dmg[--ndmg];
            i = 0;
        } else {
            i++;
        }
    }
    if (ndmg == MAXD) { /* zu viele: alles zu einem Rechteck */
        for (int i = 0; i < ndmg; i++) {
            if (dmg[i].x0 < r.x0) r.x0 = dmg[i].x0;
            if (dmg[i].y0 < r.y0) r.y0 = dmg[i].y0;
            if (dmg[i].x1 > r.x1) r.x1 = dmg[i].x1;
            if (dmg[i].y1 > r.y1) r.y1 = dmg[i].y1;
        }
        ndmg = 0;
    }
    dmg[ndmg++] = r;
}

void damage_all(void) { damage(0, 0, W, H); }

void damage_taskbar(void) { damage(0, H - TASKBAR_H, W, TASKBAR_H); }

/* Fenster samt Schatten auf dem Bildschirm */
void damage_win(const Win *w)
{
    if (!w->minimized)
        damage(w->x, w->y, w->w + 4, w->h + 4);
}

void win_dirty(Win *w, int x, int y, int ww, int hh)
{
    if (w->rx0 >= w->rx1) {
        w->rx0 = x; w->ry0 = y; w->rx1 = x + ww; w->ry1 = y + hh;
        return;
    }
    if (x < w->rx0) w->rx0 = x;
    if (y < w->ry0) w->ry0 = y;
    if (x + ww > w->rx1) w->rx1 = x + ww;
    if (y + hh > w->ry1) w->ry1 = y + hh;
}

void win_dirty_all(Win *w) { win_dirty(w, 0, 0, w->w, w->h); }

/* ======================================================================================================================
 * Fensterverwaltung
 * ==================================================================================================================== */

void content_rect(const Win *w, int *x, int *y, int *cw, int *ch)
{
    *x = w->x + BORDER;
    *y = w->y + TITLE_H;
    *cw = w->w - 2 * BORDER;
    *ch = w->h - TITLE_H - BORDER;
}

Win *new_window(int kind, const char *title, int w, int h)
{
    for (int i = 0; i < MAXW; i++) {
        if (!wins[i].used) {
            Win *n = &wins[i];
            memset(n, 0, sizeof(*n));
            n->used = 1;
            n->kind = kind;
            snprintf(n->title, sizeof(n->title), "%s", title);
            if (w > W - 20) w = W - 20;
            if (h > H - TASKBAR_H - 20) h = H - TASKBAR_H - 20;
            n->w = w;
            n->h = h;
            static int cascade;
            n->x = 40 + (cascade % 8) * 26;
            n->y = 30 + (cascade % 8) * 22;
            cascade++;
            if (n->x + w > W) n->x = W - w;
            if (n->y + h > H - TASKBAR_H) n->y = H - TASKBAR_H - h;
            n->tr0 = 1;
            order[nord++] = n;
            win_dirty_all(n);
            damage_win(n);
            damage_taskbar();
            return n;
        }
    }
    return 0;
}

void raise_win(Win *w)
{
    int i = 0;
    while (i < nord && order[i] != w)
        i++;
    if (i == nord)
        return;
    for (; i < nord - 1; i++)
        order[i] = order[i + 1];
    order[nord - 1] = w;
    w->minimized = 0;
    damage_win(w);
    damage_taskbar();
}

void minimize(Win *w)
{
    damage_win(w);
    damage_taskbar();
    w->minimized = 1;
}

Win *focused(void)
{
    for (int i = nord - 1; i >= 0; i--)
        if (!order[i]->minimized)
            return order[i];
    return 0;
}

void close_win(Win *w)
{
    if (w->kind == W_TERM) {
        if (w->to_child >= 0)
            sys_close(w->to_child);
        if (w->from_child >= 0)
            sys_close(w->from_child);
        if (w->pid > 0) {
            sys_kill(w->pid);
            int code;
            sys_wait(w->pid, &code);
        }
        u_free(w->cells);
    }
    if (w->kind == W_FILES)
        u_free(w->ents);
    if (w->kind == W_TEXT) {
        u_free(w->text);
        u_free(w->lines);
    }
    if (w->kind == W_IMAGE && w->img.px)
        surface_free(&w->img);
    damage_win(w);
    damage_taskbar();
    if (w->buf.px)
        surface_free(&w->buf);
    int i = 0;
    while (i < nord && order[i] != w)
        i++;
    for (; i < nord - 1; i++)
        order[i] = order[i + 1];
    if (nord)
        nord--;
    w->used = 0;
    if (drag_win == w)
        drag_mode = 0;
}

/* ======================================================================================================================
 * Zeichnen der ganzen Oberflaeche
 * ==================================================================================================================== */

void make_background(void)
{
    surface_new(&bg, W, H);
    for (int y = 0; y < H; y++) {
        int t = y * 256 / H;
        u32 c = RGB(40 - 25 * t / 256, 95 - 60 * t / 256, 165 - 90 * t / 256);
        for (int x = 0; x < W; x++)
            bg.px[(u64)y * (u64)W + (u64)x] = c;
    }
    gfx_no_clip();
    const char *logo = "MiniKernel";
    int lw = gfx_text_width(logo) * 4;
    gfx_text_scaled(&bg, (W - lw) / 2, (H - TASKBAR_H) / 2 - 32, logo, RGB(70, 120, 190), GFX_TRANSPARENT, 4);
}

static void draw_window(Win *w)
{
    Surface *s = tgt;
    int active = w == focused();
    gfx_no_clip();
    gfx_fill(s, w->x, w->y, w->w, w->h, RGB(205, 208, 218));
    gfx_bevel(s, w->x, w->y, w->w, w->h, 1);
    for (int i = 0; i < TITLE_H - 2; i++) { /* Titelleiste mit Verlauf */
        u32 c = active ? RGB(30 + i * 2, 80 + i * 3, 170 + i * 2) : RGB(120 + i, 125 + i, 140 + i);
        gfx_fill(s, w->x + 1, w->y + 1 + i, w->w - 2, 1, c);
    }
    gfx_set_clip(w->x, w->y, w->w - 48, TITLE_H);
    gfx_text(s, w->x + 8, w->y + 3, w->title, RGB(255, 255, 255), GFX_TRANSPARENT);
    gfx_no_clip();
    int bx = w->x + w->w - 22; /* Schliessen */
    gfx_fill(s, bx, w->y + 3, 18, 16, RGB(210, 60, 60));
    gfx_bevel(s, bx, w->y + 3, 18, 16, 1);
    gfx_text(s, bx + 5, w->y + 3, "x", RGB(255, 255, 255), GFX_TRANSPARENT);
    bx -= 22; /* Minimieren */
    gfx_fill(s, bx, w->y + 3, 18, 16, RGB(200, 204, 214));
    gfx_bevel(s, bx, w->y + 3, 18, 16, 1);
    gfx_text(s, bx + 5, w->y + 1, "_", RGB(20, 20, 30), GFX_TRANSPARENT);

    int x, y, cw, ch;
    content_rect(w, &x, &y, &cw, &ch);
    gfx_set_clip(x, y, cw, ch);
    switch (w->kind) {
    case W_TERM:  draw_terminal(w, x, y, cw, ch); break;
    case W_FILES: draw_files(w, x, y, cw, ch); break;
    case W_TEXT:  draw_text(w, x, y, cw, ch); break;
    case W_IMAGE: draw_image(w, x, y, cw, ch); break;
    case W_CALC:  draw_calc(w, x, y, cw, ch); break;
    case W_CLOCK: draw_clock(w, x, y, cw, ch); break;
    case W_ABOUT: draw_about(w, x, y, cw, ch); break;
    }
    gfx_no_clip();
    for (int i = 0; i < 3; i++) /* Griff zum Vergroessern */
        gfx_line(s, w->x + w->w - 4 - i * 4, w->y + w->h - 2, w->x + w->w - 2, w->y + w->h - 4 - i * 4, RGB(100, 100, 115));
}

const MenuItem menu[] = {
    {"Terminal", A_TERM}, {"Dateien", A_FILES}, {"Rechner", A_CALC}, {"Uhr", A_CLOCK}, {"Info", A_ABOUT},
    {"", A_SEP}, {"Malen (Vollbild)", A_PAINT}, {"Snake (Vollbild)", A_SNAKE}, {"Tetris (Vollbild)", A_TETRIS},
    {"", A_SEP}, {"Zur Konsole", A_QUIT},
};

#define NMENU ((int)(sizeof(menu) / sizeof(menu[0])))

#define MENU_W 200

static int menu_item_y(int i)
{
    int y = H - TASKBAR_H - 6;
    for (int k = NMENU - 1; k >= i; k--)
        y -= menu[k].action == A_SEP ? 8 : 24;
    return y;
}

static void draw_taskbar(void)
{
    Surface *s = tgt;
    int y = H - TASKBAR_H;
    gfx_no_clip();
    gfx_fill(s, 0, y, W, TASKBAR_H, RGB(30, 34, 46));
    gfx_fill(s, 0, y, W, 1, RGB(90, 100, 130));
    gfx_fill(s, 4, y + 4, 70, TASKBAR_H - 8, menu_open ? RGB(60, 140, 60) : RGB(50, 110, 50));
    gfx_bevel(s, 4, y + 4, 70, TASKBAR_H - 8, !menu_open);
    gfx_text(s, 19, y + 7, "Start", RGB(255, 255, 255), GFX_TRANSPARENT);
    int bx = 82;
    Win *f = focused();
    for (int i = 0; i < MAXW; i++) {
        Win *w = &wins[i];
        if (!w->used)
            continue;
        if (bx + 150 > W - 70)
            break;
        u32 c = w == f ? RGB(80, 95, 130) : w->minimized ? RGB(40, 44, 58) : RGB(55, 62, 82);
        gfx_fill(s, bx, y + 4, 146, TASKBAR_H - 8, c);
        gfx_bevel(s, bx, y + 4, 146, TASKBAR_H - 8, w != f);
        gfx_set_clip(bx, y, 140, TASKBAR_H);
        gfx_text(s, bx + 6, y + 7, w->title, RGB(230, 230, 240), GFX_TRANSPARENT);
        gfx_no_clip();
        bx += 150;
    }
    s64 now = sys_time();
    if (now > 0) {
        DateTime dt;
        time_to_date((u64)now, &dt);
        char t[16];
        snprintf(t, sizeof(t), "%02d:%02d", dt.hour, dt.min);
        gfx_text(s, W - 50, y + 7, t, RGB(230, 230, 240), GFX_TRANSPARENT);
    }
    if (menu_open) {
        int my = menu_item_y(0) - 4;
        gfx_fill(s, 4, my, MENU_W, y - my, RGB(235, 237, 245));
        gfx_bevel(s, 4, my, MENU_W, y - my, 1);
        for (int i = 0; i < NMENU; i++) {
            int iy = menu_item_y(i);
            if (menu[i].action == A_SEP) {
                gfx_fill(s, 10, iy + 3, MENU_W - 12, 1, RGB(160, 160, 175));
                continue;
            }
            int hover = i == menu_hover;
            if (hover)
                gfx_fill(s, 6, iy, MENU_W - 4, 24, RGB(60, 110, 200));
            gfx_text(s, 16, iy + 4, menu[i].label, hover ? RGB(255, 255, 255) : RGB(20, 20, 30), GFX_TRANSPARENT);
        }
    }
}

/* Menueeintrag unter (x, y), -1 = keiner */
int menu_hit(int x, int y)
{
    for (int i = 0; i < NMENU; i++) {
        int iy = menu_item_y(i);
        if (menu[i].action != A_SEP && x >= 4 && x < 4 + MENU_W && y >= iy && y < iy + 24)
            return i;
    }
    return -1;
}

void damage_menu(void)
{
    int my = menu_item_y(0) - 4;
    damage(0, my, MENU_W + 8, H - my);
}

/* Geaenderten Teil eines Fensters in sein eigenes Bild zeichnen und auf dem Bildschirm als geaendert melden */
static void render_window(Win *w)
{
    if (!w->buf.px || w->buf.w != w->w || w->buf.h != w->h) {
        if (w->buf.px)
            surface_free(&w->buf);
        if (surface_new(&w->buf, w->w, w->h) != 0) {
            w->buf.px = 0;
            return;
        }
        win_dirty_all(w);
    }
    if (w->rx0 >= w->rx1)
        return;
    int sx = w->x, sy = w->y;
    w->x = w->y = 0; /* draw_window zeichnet an (w->x, w->y): hier die linke obere Ecke des Fensterbildes */
    tgt = &w->buf;
    gfx_set_base_clip(w->rx0, w->ry0, w->rx1 - w->rx0, w->ry1 - w->ry0);
    draw_window(w);
    gfx_reset_base_clip();
    tgt = &gfx_screen;
    w->x = sx;
    w->y = sy;
    if (!w->minimized)
        damage(w->x + w->rx0, w->y + w->ry0, w->rx1 - w->rx0, w->ry1 - w->ry0);
    w->rx0 = w->rx1 = 0;
}

/* Rechteck des Bildschirms aus Hintergrund, Fensterbildern und Taskleiste zusammensetzen und anzeigen */
static void compose(const Clip *r)
{
    int x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
    for (int y = y0; y < y1; y++)
        memcpy(gfx_screen.px + (u64)y * (u64)W + (u64)x0, bg.px + (u64)y * (u64)W + (u64)x0, (u64)(x1 - x0) * 4);
    gfx_set_base_clip(x0, y0, x1 - x0, y1 - y0);
    for (int i = 0; i < nord; i++) {
        Win *w = order[i];
        if (w->minimized || !w->buf.px)
            continue;
        gfx_fill(&gfx_screen, w->x + 4, w->y + 4, w->w, w->h, RGB(10, 20, 40)); /* Schatten */
        int ax = w->x > x0 ? w->x : x0, ay = w->y > y0 ? w->y : y0;
        int bx = w->x + w->w < x1 ? w->x + w->w : x1, by = w->y + w->h < y1 ? w->y + w->h : y1;
        for (int y = ay; y < by; y++)
            memcpy(gfx_screen.px + (u64)y * (u64)W + (u64)ax, w->buf.px + (u64)(y - w->y) * (u64)w->w + (u64)(ax - w->x),
                   bx > ax ? (u64)(bx - ax) * 4 : 0);
    }
    if (y1 > H - TASKBAR_H || menu_open)
        draw_taskbar();
    gfx_reset_base_clip();
    gfx_present(x0, y0, x1 - x0, y1 - y0);
}

void draw_all(void)
{
    for (int i = 0; i < nord; i++)
        render_window(order[i]);
    for (int i = 0; i < ndmg; i++)
        compose(&dmg[i]);
    ndmg = 0;
}
