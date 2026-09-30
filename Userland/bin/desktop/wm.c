/* Desktop: geaenderte Bereiche, Fensterverwaltung, Zeichnen und Zusammensetzen der Oberflaeche */

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

static int shadow_dy(void) { return SHADOW / 3; }

/* Fenster samt Schatten auf dem Bildschirm */
void damage_win(const Win *w)
{
    if (!w->minimized)
        damage(w->x - SHADOW, w->y - SHADOW + shadow_dy(), w->w + 2 * SHADOW, w->h + 2 * SHADOW);
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
    *x = w->x;
    *y = w->y + TITLE_H;
    *cw = w->w;
    *ch = w->h - TITLE_H;
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
            int area = dock_top() - MENUBAR_H;
            if (w > W - U(40)) w = W - U(40);
            if (h > area - U(20)) h = area - U(20);
            n->w = w;
            n->h = h;
            static int cascade;
            static const int px[8] = {0, 3, 1, 4, 2, 5, 1, 3}, py[8] = {0, 1, 3, 0, 2, 3, 1, 2}; /* verteilt, nicht gestapelt */
            n->x = W / 8 + px[cascade % 8] * (W - w - W / 4) / 5;
            n->y = MENUBAR_H + U(30) + py[cascade % 8] * (dock_top() - MENUBAR_H - h - U(40)) / 3;
            cascade++;
            if (n->x < U(10)) n->x = U(10);
            if (n->x + w > W) n->x = W - w;
            if (n->y + h > dock_top()) n->y = dock_top() - h;
            if (n->y < MENUBAR_H) n->y = MENUBAR_H;
            n->tr0 = 1;
            order[nord++] = n;
            win_dirty_all(n);
            damage_win(n);
            damage_menubar();
            damage_dock();
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
    Win *old = focused();
    for (; i < nord - 1; i++)
        order[i] = order[i + 1];
    order[nord - 1] = w;
    if (w->minimized) {
        w->minimized = 0;
        damage_dock();
    }
    if (old && old != w)
        win_dirty(old, 0, 0, old->w, TITLE_H); /* Titelleiste: jetzt inaktiv */
    win_dirty(w, 0, 0, w->w, TITLE_H);
    damage_win(w);
    damage_menubar();
}

void minimize(Win *w)
{
    damage_win(w);
    w->minimized = 1;
    damage_dock();
    damage_menubar();
    Win *f = focused();
    if (f)
        win_dirty(f, 0, 0, f->w, TITLE_H);
}

void zoom_win(Win *w)
{
    damage_win(w);
    if (w->zoomed) {
        w->x = w->zx; w->y = w->zy; w->w = w->zw; w->h = w->zh;
        w->zoomed = 0;
    } else {
        w->zx = w->x; w->zy = w->y; w->zw = w->w; w->zh = w->h;
        w->x = U(6);
        w->y = MENUBAR_H + U(6);
        w->w = W - U(12);
        w->h = dock_top() - w->y - U(2);
        w->zoomed = 1;
    }
    if (w->kind == W_TERM)
        term_alloc(w);
    win_dirty_all(w);
    damage_win(w);
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
    damage_dock();
    damage_menubar();
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
    Win *f = focused();
    if (f)
        win_dirty(f, 0, 0, f->w, TITLE_H);
}

/* ======================================================================================================================
 * Fenster zeichnen: Titelleiste mit den drei Knoepfen, dann der Inhalt
 * ==================================================================================================================== */

static Win *hover_btns; /* Fenster, ueber dessen Knoepfen die Maus steht */

static float btn_cx(int i) { return (float)U(20) + (float)i * U(20); } /* Mitte von Knopf i (0-2) */

int title_button_at(const Win *w, int x, int y)
{
    int ly = y - w->y, lx = x - w->x;
    if (ly < 0 || ly >= TITLE_H)
        return 0;
    for (int i = 0; i < 3; i++) {
        float dx = lx - btn_cx(i), dy = ly - TITLE_H * 0.5f;
        if (dx * dx + dy * dy <= (float)U(9) * U(9))
            return i + 1;
    }
    return 0;
}

void set_button_hover(Win *w)
{
    if (w == hover_btns)
        return;
    if (hover_btns && hover_btns->used)
        win_dirty(hover_btns, 0, 0, U(80), TITLE_H);
    hover_btns = w;
    if (w)
        win_dirty(w, 0, 0, U(80), TITLE_H);
}

static void draw_buttons(Surface *s, Win *w, int active)
{
    static const u32 fill[3] = {0xFF5F57, 0xFEBC2E, 0x28C840}, edge[3] = {0xE0443E, 0xDEA123, 0x1AAB29};
    float cy = w->y + TITLE_H * 0.5f, r = U(6);
    int show = hover_btns == w;
    for (int i = 0; i < 3; i++) {
        float cx = w->x + btn_cx(i);
        int on = active || show;
        gfx_disc(s, cx, cy, r, on ? edge[i] : 0xC8C8CC, 255);
        gfx_disc(s, cx, cy, r - 0.8f, on ? fill[i] : 0xDCDCE0, 255);
        if (!show)
            continue;
        float k = r * 0.45f, lw = U(1) * 1.3f;
        u32 sym = 0x4D0000 + (u32)i * 0x001000;
        if (i == 0) {
            gfx_capsule(s, cx - k, cy - k, cx + k, cy + k, lw, sym, 200);
            gfx_capsule(s, cx - k, cy + k, cx + k, cy - k, lw, sym, 200);
        } else if (i == 1) {
            gfx_capsule(s, cx - k * 1.1f, cy, cx + k * 1.1f, cy, lw, 0x985700, 220);
        } else {
            gfx_capsule(s, cx - k * 1.1f, cy, cx + k * 1.1f, cy, lw, 0x006500, 220);
            gfx_capsule(s, cx, cy - k * 1.1f, cx, cy + k * 1.1f, lw, 0x006500, 220);
        }
    }
}

static void draw_window(Win *w)
{
    Surface *s = tgt;
    int active = w == focused();
    gfx_no_clip();
    gfx_gradient(s, w->x, w->y, w->w, TITLE_H, active ? C_TITLE_TOP : 0xF8F8F8, active ? C_TITLE_BOT : 0xF2F2F2);
    gfx_fill(s, w->x, w->y + TITLE_H - 1, w->w, 1, C_HAIRLINE);
    draw_buttons(s, w, active);
    int tw = text_width(font_bold, FS, w->title), tx = w->x + (w->w - tw) / 2;
    if (tx < w->x + U(80))
        tx = w->x + U(80);
    gfx_set_clip(w->x + U(80), w->y, w->w - U(90), TITLE_H);
    text_draw(s, font_bold, FS, tx, w->y + (TITLE_H - text_height(font_bold, FS)) / 2, w->title,
              active ? 0x4D4D4D : 0xA8A8AC);
    gfx_no_clip();

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

/* Rechteck des Bildschirms zusammensetzen: Hintergrund, je Fenster Schatten und Bild (runde Ecken, feiner Rand),
 * dann Menueleiste, Dock und offenes Menue; anzeigen */
static void compose(const Clip *r)
{
    int x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
    for (int y = y0; y < y1; y++)
        memcpy(gfx_screen.px + (u64)y * (u64)W + (u64)x0, bg.px + (u64)y * (u64)W + (u64)x0, (u64)(x1 - x0) * 4);
    gfx_set_base_clip(x0, y0, x1 - x0, y1 - y0);
    gfx_no_clip();
    Win *f = focused();
    for (int i = 0; i < nord; i++) {
        Win *w = order[i];
        if (w->minimized || !w->buf.px)
            continue;
        if (w->x - SHADOW >= x1 || w->x + w->w + SHADOW <= x0 || w->y - SHADOW >= y1 || w->y + w->h + 2 * SHADOW <= y0)
            continue;
        gfx_shadow(&gfx_screen, w->x, w->y + shadow_dy(), w->w, w->h, RADIUS, SHADOW, w == f ? 95 : 55);
        gfx_blit_round(&gfx_screen, &w->buf, 0, 0, w->x, w->y, w->w, w->h, RADIUS);
        gfx_round_frame(&gfx_screen, w->x, w->y, w->w, w->h, RADIUS, 0x000000, 40);
    }
    if (y0 < MENUBAR_H)
        draw_menubar();
    if (y1 > dock_top() - U(60))
        draw_dock();
    draw_menu();
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
