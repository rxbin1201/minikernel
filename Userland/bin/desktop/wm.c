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
 * Animationen: Fensterbild von Rechteck from nach to, weich beschleunigt/abgebremst
 * ==================================================================================================================== */

static void set4(int *r, int x, int y, int w, int h) { r[0] = x; r[1] = y; r[2] = w; r[3] = h; }

static void anim_start(Win *w, int type, const int *from, const int *to, int ms)
{
    w->anim = type;
    w->anim_t0 = now_us;
    w->anim_ms = ms;
    memcpy(w->from, from, sizeof(w->from));
    memcpy(w->to, to, sizeof(w->to));
    memcpy(w->last, from, sizeof(w->last));
}

/* Rechteck um r schrumpfen/wachsen (Faktor pct/100) um die Mitte */
static void scaled_rect(const Win *w, int pct, int *r)
{
    int nw = w->w * pct / 100, nh = w->h * pct / 100;
    set4(r, w->x + (w->w - nw) / 2, w->y + (w->h - nh) / 2, nw, nh);
}

/* Stand der Animation: Rechteck und Deckkraft (0-255) */
static void anim_state(const Win *w, int *r, int *alpha)
{
    float t = (float)(now_us - w->anim_t0) / (w->anim_ms * 1000.0f);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float e;
    if (w->anim == ANIM_OPEN) {
        float u = 1 - t;
        e = 1 - u * u * u; /* schnell los, sanft ankommen */
    } else if (w->anim == ANIM_CLOSE) {
        e = t * t;
    } else {
        e = t < 0.5f ? 4 * t * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * (-2 * t + 2) / 2; /* weich an beiden Enden */
    }
    for (int i = 0; i < 4; i++)
        r[i] = w->from[i] + (int)((w->to[i] - w->from[i]) * e);
    switch (w->anim) {
    case ANIM_OPEN: *alpha = (int)(255 * (t * 2 < 1 ? t * 2 : 1)); break;
    case ANIM_CLOSE: *alpha = (int)(255 * (1 - e)); break;
    case ANIM_MIN: *alpha = (int)(255 - 120 * e); break;
    case ANIM_RESTORE: *alpha = (int)(135 + 120 * e); break;
    default: *alpha = 255;
    }
}

static void damage_rect_shadow(const int *r)
{
    damage(r[0] - SHADOW, r[1] - SHADOW + shadow_dy(), r[2] + 2 * SHADOW, r[3] + 2 * SHADOW);
}

void anim_tick(void)
{
    for (int i = 0; i < MAXW; i++) {
        Win *w = &wins[i];
        if (!w->used || !w->anim)
            continue;
        int r[4], a;
        anim_state(w, r, &a);
        damage_rect_shadow(w->last);
        damage_rect_shadow(r);
        memcpy(w->last, r, sizeof(r));
        if (now_us - w->anim_t0 < (s64)w->anim_ms * 1000)
            continue;
        int type = w->anim;
        w->anim = 0;
        if (type == ANIM_CLOSE)
            close_win_now(w);
        else if (type == ANIM_MIN)
            damage_dock(); /* jetzt erscheint das Fenster in der Taskleiste */
        else
            damage_win(w);
    }
}

/* ======================================================================================================================
 * Fensterverwaltung
 * ==================================================================================================================== */

void content_rect(const Win *w, int *x, int *y, int *cw, int *ch)
{
    *x = w->x;
    *y = w->y + win_th(w);
    *cw = w->w;
    *ch = w->h - win_th(w);
}

Win *new_window(const char *title, int w, int h)
{
    for (int i = 0; i < MAXW; i++) {
        if (!wins[i].used) {
            Win *n = &wins[i];
            memset(n, 0, sizeof(*n));
            n->used = 1;
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
            order[nord++] = n;
            win_dirty_all(n);
            int from[4], to[4];
            scaled_rect(n, 92, from);
            set4(to, n->x, n->y, n->w, n->h);
            anim_start(n, ANIM_OPEN, from, to, 200);
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
    if (w->minimized) { /* aus der Taskleiste zurueck: vom Symbol auf die alte Groesse */
        int from[4], to[4], sz;
        dock_slot_of(w, &from[0], &from[1], &sz);
        set4(from, from[0], from[1], sz, sz * w->h / (w->w ? w->w : 1));
        w->minimized = 0;
        set4(to, w->x, w->y, w->w, w->h);
        anim_start(w, ANIM_RESTORE, from, to, 280);
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
    w->minimized = 1; /* die Taskleiste macht schon Platz: dorthin schrumpft das Fenster */
    int from[4], to[4], sz;
    set4(from, w->x, w->y, w->w, w->h);
    dock_slot_of(w, &to[0], &to[1], &sz);
    set4(to, to[0], to[1] + sz / 2 - sz * w->h / (2 * (w->w ? w->w : 1)), sz, sz * w->h / (w->w ? w->w : 1));
    anim_start(w, ANIM_MIN, from, to, 280);
    damage_dock();
    damage_menubar();
    Win *f = focused();
    if (f)
        win_dirty(f, 0, 0, f->w, TITLE_H);
}

/* Andocken: ganzer Bildschirm oder eine Haelfte (ueber der Taskleiste, mit kleinem Rand) */
void snap_rect(int where, int *r)
{
    int gap = U(6), top = MENUBAR_H + gap, h = dock_top() - top - U(2), half = (W - 3 * gap) / 2;
    if (where == SNAP_LEFT)
        set4(r, gap, top, half, h);
    else if (where == SNAP_RIGHT)
        set4(r, W - gap - half, top, half, h);
    else
        set4(r, gap, top, W - 2 * gap, h);
}

void snap_win(Win *w, int where)
{
    if (!(w->flags & WPF_RESIZABLE) || where == w->zoomed) /* feste Groesse: das Programm zeichnet genau so viel */
        return;
    damage_win(w);
    int from[4], to[4];
    set4(from, w->x, w->y, w->w, w->h);
    if (!w->zoomed) { /* Lage merken, um spaeter zurueckzukehren */
        w->zx = w->x; w->zy = w->y; w->zw = w->w; w->zh = w->h;
    }
    if (where == SNAP_NONE)
        set4(to, w->zx, w->zy, w->zw, w->zh);
    else
        snap_rect(where, to);
    w->x = to[0]; w->y = to[1]; w->w = to[2]; w->h = to[3];
    w->zoomed = where;
    win_dirty_all(w); /* die neue Groesse erfaehrt das Programm nach dem Bild (apps_frame) */
    anim_start(w, ANIM_ZOOM, from, to, 220);
}

void zoom_win(Win *w)
{
    snap_win(w, w->zoomed ? SNAP_NONE : SNAP_MAX);
}

static int snap_preview;

void set_snap_preview(int where)
{
    if (where == snap_preview)
        return;
    int r[4];
    if (snap_preview) {
        snap_rect(snap_preview, r);
        damage(r[0] - U(4), r[1] - U(4), r[2] + U(8), r[3] + U(8));
        overlay_dirty(r[0] - U(4), r[1] - U(4), r[2] + U(8), r[3] + U(8));
    }
    snap_preview = where;
    if (where) {
        snap_rect(where, r);
        damage(r[0] - U(4), r[1] - U(4), r[2] + U(8), r[3] + U(8));
        overlay_dirty(r[0] - U(4), r[1] - U(4), r[2] + U(8), r[3] + U(8));
    }
}

void lower_win(Win *w)
{
    int i = 0;
    while (i < nord && order[i] != w)
        i++;
    if (i == nord)
        return;
    for (; i > 0; i--)
        order[i] = order[i - 1];
    order[0] = w;
    damage_win(w);
    Win *f = focused();
    if (f) {
        win_dirty(f, 0, 0, f->w, TITLE_H);
        damage_win(f);
    }
    damage_menubar();
}

/* Alt+Tab: das hinterste sichtbare Fenster nach vorn (so kommt nacheinander jedes dran); zurueck: das vorderste nach
 * hinten */
void cycle_windows(int dir)
{
    Win *f = focused();
    if (!f)
        return;
    if (dir < 0) {
        lower_win(f);
        return;
    }
    for (int i = 0; i < nord; i++) {
        Win *w = order[i];
        if (!w->minimized && w->anim != ANIM_CLOSE && w != f) {
            raise_win(w);
            return;
        }
    }
}

Win *focused(void)
{
    for (int i = nord - 1; i >= 0; i--)
        if (!order[i]->minimized && order[i]->anim != ANIM_CLOSE)
            return order[i];
    return 0;
}

/* Schliessen: erst ausblenden (das Bild bleibt so lange), dann wirklich schliessen */
void close_win(Win *w)
{
    if (w->anim == ANIM_CLOSE)
        return;
    if (!w->app_done) { /* erst das Programm fragen; das Fenster geht, wenn es sich beendet */
        app_request_close(w);
        return;
    }
    if (w->minimized || !w->buf.px) {
        close_win_now(w);
        return;
    }
    int from[4], to[4];
    set4(from, w->x, w->y, w->w, w->h);
    scaled_rect(w, 92, to);
    anim_start(w, ANIM_CLOSE, from, to, 160);
    if (drag_win == w)
        drag_mode = 0;
    damage_menubar();
    Win *f = focused();
    if (f)
        win_dirty(f, 0, 0, f->w, TITLE_H);
}

void close_win_now(Win *w)
{
    app_free(w);
    damage_win(w);
    damage_dock();
    damage_menubar();
    if (w->buf.px)
        gsurf_free(&w->buf);
    shadow_free(w);
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
    if (ly < 0 || ly >= win_th(w))
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
    if (win_th(w)) { /* rahmenlose Fenster zeichnen ihren Kopf selbst */
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
    }

    int x, y, cw, ch;
    content_rect(w, &x, &y, &cw, &ch);
    gfx_set_clip(x, y, cw, ch);
    draw_app(w, x, y, cw, ch);
    gfx_no_clip();
}

/* Deckung in Byte 3 (im neu gezeichneten Teil): in den oberen und unteren RADIUS Zeilen die der runden Ecken,
 * sonst 255 - so setzt die GPU das Fensterbild auch skaliert (Animationen) richtig auf */
static void corner_alpha(Win *w)
{
    int x0 = w->rx0 < 0 ? 0 : w->rx0, x1 = w->rx1 > w->w ? w->w : w->rx1;
    int y0 = w->ry0 < 0 ? 0 : w->ry0, y1 = w->ry1 > w->h ? w->h : w->ry1;
    for (int y = y0; y < y1; y++) {
        u32 *p = w->buf.px + (u64)y * (u64)w->buf.w;
        if (y >= RADIUS && y < w->h - RADIUS) {
            for (int x = x0; x < x1; x++)
                p[x] |= 0xFF000000u;
            continue;
        }
        for (int x = x0; x < x1; x++) {
            int c = gfx_round_cov(x, y, 0, 0, w->w, w->h, RADIUS);
            p[x] = (p[x] & 0xFFFFFF) | (u32)(c > 255 ? 255 : c) << 24;
        }
    }
}

/* Geaenderten Teil eines Fensters in sein eigenes Bild zeichnen und auf dem Bildschirm als geaendert melden */
static void render_window(Win *w)
{
    int bw = gsurf_width(w->w); /* fuer die GPU auf 16 Pixel aufgerundet, rechts bleibt ein unbenutzter Rand */
    if (!w->buf.px || w->buf.w != bw || w->buf.h != w->h) {
        if (w->buf.px)
            gsurf_free(&w->buf);
        if (gsurf_new(&w->buf, bw, w->h) != 0) {
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
    gfx_round_frame(&w->buf, 0, 0, w->w, w->h, RADIUS, 0x000000, 40); /* feiner Rand, fest im Bild */
    corner_alpha(w);
    gfx_reset_base_clip();
    tgt = &gfx_screen;
    w->x = sx;
    w->y = sy;
    if (!w->minimized)
        damage(w->x + w->rx0, w->y + w->ry0, w->rx1 - w->rx0, w->ry1 - w->ry0);
    w->rx0 = w->rx1 = 0;
}

/* Kann die GPU dieses Bild zusammensetzen? */
static int gpu_usable(void)
{
    return gpu_mode && gsurf_handle(&gfx_screen) && gsurf_handle(&bg);
}

/* Fenster in der Animation: Bild (und Schatten) auf der GPU skalieren und mit der Deckung der Animation mischen.
 * Wie compose_cpu: Rechteck und Deckung aus anim_state, Schatten um shadow_dy * Massstab versetzt; die Schattenstreifen
 * werden nur entlang der Kante gestreckt (die Weichheit bleibt). 0 = geht nicht (dann die CPU) */
static int queue_anim(Win *w, Win *f, const Clip *c)
{
    int r[4], a;
    anim_state(w, r, &a);
    if (a <= 0 || r[2] < 8 || r[3] < 8) /* unsichtbar bzw. winzig */
        return 1;
    int S = SHADOW, R = RADIUS, band = S + R, b8 = shadow_b8();
    int sc = r[2] < w->w ? r[2] : w->w, ww = w->w ? w->w : 1;
    int X = r[0], Y = r[1], dw = r[2], dh = r[3], dys = shadow_dy() * sc / ww;
    if (X - S >= c->x1 || X + dw + S <= c->x0 || Y + dys - S >= c->y1 || Y + dys + dh + S <= c->y0)
        return 1;
    Surface *t = anim_temps();
    int w8 = (w->w + 7) & ~7, dw8 = (dw + 7) & ~7, dh8 = (dh + 7) & ~7;
    if (!t || w8 > t[0].w || dh8 > t[0].h || dw8 > t[1].w || dh8 > t[1].h)
        return 0;
    int sty = (w->h - 1) * 256 / (dh8 - 1), stx = (w->w - 1) * 256 / (dw8 - 1);
    if (sty < 1 || sty > 0xFFFF || stx < 1 || stx > 0xFFFF)
        return 0;
    int g = a + (a >> 7); /* 0-255 -> 0-256 */
    gq_scale(1, &t[0], 0, 0, &w->buf, 0, 0, w8, dh8, sty);
    gq_scale(0, &t[1], 0, 0, &t[0], 0, 0, dw8, dh8, stx);
    if (shadow_ready(w, w == f ? 95 : 55)) {
        /* oben/unten: die Ecken (je S + R breit) unveraendert, nur das gerade Mittelstueck strecken */
        int tw = dw + 2 * S, mw = dw - 2 * R, mw8 = (mw + 7) & ~7, srcm = w->w - 2 * R;
        int sxs = mw >= 8 && srcm > 1 ? (srcm - 1) * 256 / (mw8 - 1) : 0;
        if (sxs >= 1 && sxs <= 0xFFFF && band + mw8 + band <= t[2].w) {
            Clip all = {0, 0, t[2].w, t[2].h};
            for (int half = 0; half <= b8; half += b8) {
                gq_scale(0, &t[2], band, half, &w->shd_tb, band, half, mw8, b8, sxs);
                gq_copy(&t[2], 0, half, &w->shd_tb, 0, half, band, band, &all);
                gq_copy(&t[2], band + mw, half, &w->shd_tb, w->w + 2 * S - band, half, band, band, &all);
            }
            gq_blend_a(&gfx_screen, X - S, Y + dys - S, &t[2], 0, 0, tw, band, g, c);
            gq_blend_a(&gfx_screen, X - S, Y + dys + dh - R, &t[2], 0, b8, tw, band, g, c);
        }
        int lh = dh - 2 * R, lh8 = (lh + 7) & ~7, lrh = w->h - 2 * R;
        int sys = lh >= 8 && lrh > 1 ? (lrh - 1) * 256 / (lh8 - 1) : 0;
        if (sys >= 1 && sys <= 0xFFFF && lh8 <= t[3].h) {
            gq_scale(1, &t[3], 0, 0, &w->shd_lr, 0, 0, b8, lh8, sys);
            gq_scale(1, &t[3], b8, 0, &w->shd_lr, b8, 0, b8, lh8, sys);
            gq_blend_a(&gfx_screen, X - S, Y + dys + R, &t[3], 0, 0, band, lh, g, c);
            gq_blend_a(&gfx_screen, X + dw - R, Y + dys + R, &t[3], b8, 0, band, lh, g, c);
        }
    }
    gq_blend_a(&gfx_screen, X, Y, &t[1], 0, 0, dw, dh, g, c);
    return 1;
}

/* Auftraege fuer ein Rechteck: Hintergrund kopieren, je Fenster den Schatten (vier Streifen) und das Bild mischen
 * (obere und untere Zeilen mit den runden Ecken) bzw. kopieren (dazwischen). 0 = geht nicht */
static int queue_gpu(const Clip *r)
{
    Win *f = focused();
    int S = SHADOW, R = RADIUS, band = S + R, b8 = shadow_b8(), dy = shadow_dy();
    gq_copy(&gfx_screen, r->x0, r->y0, &bg, r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0, r);
    for (int i = 0; i < nord; i++) {
        Win *w = order[i];
        if (!w->buf.px || (w->minimized && w->anim != ANIM_MIN))
            continue;
        if (w->anim) {
            if (!queue_anim(w, f, r))
                return 0;
            continue;
        }
        if (w->x - S >= r->x1 || w->x + w->w + S <= r->x0 || w->y + dy - S >= r->y1 || w->y + w->h + dy + S <= r->y0)
            continue;
        if (!shadow_ready(w, w == f ? 95 : 55))
            return 0;
        int X = w->x, Y = w->y, lrh = w->h - 2 * R;
        gq_blend(&gfx_screen, X - S, Y + dy - S, &w->shd_tb, 0, 0, w->w + 2 * S, band, r);
        gq_blend(&gfx_screen, X - S, Y + dy + w->h - R, &w->shd_tb, 0, b8, w->w + 2 * S, band, r);
        gq_blend(&gfx_screen, X - S, Y + dy + R, &w->shd_lr, 0, 0, band, lrh, r);
        gq_blend(&gfx_screen, X + w->w - R, Y + dy + R, &w->shd_lr, b8, 0, band, lrh, r);
        gq_blend(&gfx_screen, X, Y, &w->buf, 0, 0, w->w, R, r);
        gq_copy(&gfx_screen, X, Y + R, &w->buf, 0, R, w->w, w->h - 2 * R, r);
        gq_blend(&gfx_screen, X, Y + w->h - R, &w->buf, 0, w->h - R, w->w, R, r);
    }
    return 1;
}

/* Hintergrund und Fenster mit der CPU (auch waehrend der Animationen) */
static void compose_cpu(const Clip *r)
{
    int x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
    for (int y = y0; y < y1; y++)
        memcpy(gfx_screen.px + (u64)y * (u64)W + (u64)x0, bg.px + (u64)y * (u64)W + (u64)x0, (u64)(x1 - x0) * 4);
    gfx_set_base_clip(x0, y0, x1 - x0, y1 - y0);
    gfx_no_clip();
    Win *f = focused();
    for (int i = 0; i < nord; i++) {
        Win *w = order[i];
        if (!w->buf.px || (w->minimized && w->anim != ANIM_MIN))
            continue;
        if (w->anim) { /* waehrend der Animation: skaliert und ein-/ausgeblendet */
            int r[4], a;
            anim_state(w, r, &a);
            /* Radius und Schattenversatz schrumpfen mit, wachsen aber nie ueber das Normalmass (beim Wiederherstellen
             * ist das Bild groesser als das Fenster: sonst laege der Schatten ausserhalb des neu gezeichneten Bereichs) */
            int sc = r[2] < w->w ? r[2] : w->w, ww = w->w ? w->w : 1;
            int rad = RADIUS * sc / ww;
            gfx_shadow(&gfx_screen, r[0], r[1] + shadow_dy() * sc / ww, r[2], r[3], rad, SHADOW,
                       (w == f ? 95 : 55) * a / 255);
            gfx_blit_scaled_part(&gfx_screen, &w->buf, w->w, w->h, r[0], r[1], r[2], r[3], a, rad);
            continue;
        }
        if (w->x - SHADOW >= x1 || w->x + w->w + SHADOW <= x0 || w->y - SHADOW >= y1 || w->y + w->h + 2 * SHADOW <= y0)
            continue;
        gfx_shadow(&gfx_screen, w->x, w->y + shadow_dy(), w->w, w->h, RADIUS, SHADOW, w == f ? 95 : 55);
        gfx_blit_round(&gfx_screen, &w->buf, 0, 0, w->x, w->y, w->w, w->h, RADIUS);
    }
    gfx_reset_base_clip();
}

/* Was ueber den Fenstern liegt (immer mit der CPU): Vorschau beim Andocken, Taskleiste, Menue, Dialog */
static void overlays(const Clip *r)
{
    int x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
    gfx_set_base_clip(x0, y0, x1 - x0, y1 - y0);
    gfx_no_clip();
    if (snap_preview) { /* Vorschau beim Ziehen an den Rand: helles Milchglas */
        int sr[4];
        snap_rect(snap_preview, sr);
        gfx_round_rect(&gfx_screen, sr[0], sr[1], sr[2], sr[3], RADIUS, 0xFFFFFF, 70);
        gfx_round_frame(&gfx_screen, sr[0], sr[1], sr[2], sr[3], RADIUS, 0xFFFFFF, 170);
    }
    if (y1 > dock_top() - U(50)) /* Taskleiste und die Namen darueber */
        draw_dock();
    draw_menu();
    draw_dialog();
    gfx_reset_base_clip();
}

/* ---------- Ebene ueber den Fenstern (GPU) ----------
 * Taskleiste, Menues, Dialog und Andock-Vorschau zeichnet die CPU nur, wenn sie sich aendern (overlay_dirty), und zwar
 * zweimal: auf Schwarz und auf Weiss. Aus dem Unterschied ergibt sich je Pixel die Deckung a = 255 - (weiss - schwarz)
 * und die Farbe schwarz * 255 / a - so passt das fuer Milchglas, Text, Symbole, Schatten und das Abdunkeln gleichermassen.
 * Die GPU mischt diese Ebene bei jedem Bild ueber die Fenster (nur dort, wo gerade etwas liegt). */

#define MAXOV 8

static Surface ov_layer, ov_black, ov_white, ov_dim; /* ov_dim: Schwarz mit Deckung DIALOG_DIM (Dialog) */
static Clip    ov_dirty[MAXOV];
static int     nov, ov_failed, ov_stale = 1; /* ov_stale: die ganze Ebene neu zeichnen */
int            ov_pass;

/* Wo die Ebene gerade etwas zeigt (jeweils x, y, w, h); 0-4 Zonen. Taskleiste immer, dazu offenes Menue, Dialog,
 * Andock-Vorschau. Menue und Vorschau enden ueber der Taskleiste (jede Stelle wird nur einmal gemischt). */
static int ov_zones(int z[4][4])
{
    int n = 0;
    dock_zone(z[n]);
    int top = z[n][1];
    n++;
    if (menu_zone(z[n])) {
        if (z[n][1] + z[n][3] > top)
            z[n][3] = top - z[n][1];
        n++;
    }
    if (dialog_kind)
        dialog_zone(z[n++]);
    if (snap_preview) {
        snap_rect(snap_preview, z[n]);
        z[n][0] -= U(4);
        z[n][1] -= U(4);
        z[n][2] += U(8);
        z[n][3] += U(8);
        if (z[n][1] + z[n][3] > top)
            z[n][3] = top - z[n][1];
        n++;
    }
    return n;
}

void overlay_dirty(int x, int y, int w, int h)
{
    Clip r = {x < 0 ? 0 : x, y < 0 ? 0 : y, x + w > W ? W : x + w, y + h > H ? H : y + h};
    if (r.x0 >= r.x1 || r.y0 >= r.y1)
        return;
    for (int i = 0; i < nov;) { /* ueberlappende zusammenfassen */
        Clip *d = &ov_dirty[i];
        if (d->x0 <= r.x1 && r.x0 <= d->x1 && d->y0 <= r.y1 && r.y0 <= d->y1) {
            if (d->x0 < r.x0) r.x0 = d->x0;
            if (d->y0 < r.y0) r.y0 = d->y0;
            if (d->x1 > r.x1) r.x1 = d->x1;
            if (d->y1 > r.y1) r.y1 = d->y1;
            ov_dirty[i] = ov_dirty[--nov];
            i = 0;
        } else {
            i++;
        }
    }
    if (nov == MAXOV)
        ov_stale = 1;
    else
        ov_dirty[nov++] = r;
}

static int ov_ready(void)
{
    if (ov_layer.px)
        return 1;
    if (ov_failed)
        return 0;
    if (gsurf_new(&ov_layer, W, H) != 0 || !gsurf_handle(&ov_layer) || surface_new(&ov_black, W, H) != 0 ||
        surface_new(&ov_white, W, H) != 0) {
        gsurf_free(&ov_layer);
        if (ov_black.px)
            surface_free(&ov_black);
        if (ov_white.px)
            surface_free(&ov_white);
        ov_failed = 1;
        return 0;
    }
    ov_stale = 1;
    return 1;
}

/* Rechteck der Ebene neu: auf Schwarz und Weiss zeichnen, Deckung und Farbe herausrechnen */
static void ov_render(const Clip *r)
{
    int n = r->x1 - r->x0;
    for (int y = r->y0; y < r->y1; y++) {
        memset(ov_black.px + (u64)y * (u64)W + (u64)r->x0, 0, (u64)n * 4);
        u32 *wt = ov_white.px + (u64)y * (u64)W + (u64)r->x0;
        for (int x = 0; x < n; x++)
            wt[x] = 0xFFFFFF;
    }
    u32 *screen = gfx_screen.px;
    ov_pass = 1;
    gfx_screen.px = ov_black.px;
    overlays(r);
    gfx_screen.px = ov_white.px;
    overlays(r);
    gfx_screen.px = screen;
    ov_pass = 0;
    for (int y = r->y0; y < r->y1; y++) {
        const u32 *b = ov_black.px + (u64)y * (u64)W + (u64)r->x0, *wt = ov_white.px + (u64)y * (u64)W + (u64)r->x0;
        u32 *o = ov_layer.px + (u64)y * (u64)W + (u64)r->x0;
        for (int x = 0; x < n; x++) {
            u32 bp = b[x], wp = wt[x];
            if (!(bp & 0xFFFFFF) && (wp & 0xFFFFFF) == 0xFFFFFF) { /* nichts gezeichnet */
                o[x] = 0;
                continue;
            }
            int diff = (int)((wp >> 16 & 0xFF) - (bp >> 16 & 0xFF)) + (int)((wp >> 8 & 0xFF) - (bp >> 8 & 0xFF)) +
                       (int)((wp & 0xFF) - (bp & 0xFF));
            int a = 255 - (diff + 1) / 3;
            if (a <= 0) {
                o[x] = 0;
                continue;
            }
            if (a >= 255) {
                o[x] = (bp & 0xFFFFFF) | 0xFF000000u;
                continue;
            }
            u32 c = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                int v = ((int)(bp >> sh & 0xFF) * 255 + a / 2) / a;
                c |= (u32)(v > 255 ? 255 : v) << sh;
            }
            o[x] = c | (u32)a << 24;
        }
    }
}

/* Geaenderte Teile neu zeichnen - nur innerhalb der Zonen (anderswo wird die Ebene nicht gemischt; eine Zone, die neu
 * erscheint, meldet sich selbst als geaendert) */
static void ov_update(void)
{
    int z[4][4], nz = ov_zones(z);
    Clip all = {0, 0, W, H};
    const Clip *d = ov_stale ? &all : ov_dirty;
    int nd = ov_stale ? 1 : nov;
    for (int i = 0; i < nd; i++)
        for (int k = 0; k < nz; k++) {
            Clip c = {d[i].x0 > z[k][0] ? d[i].x0 : z[k][0], d[i].y0 > z[k][1] ? d[i].y0 : z[k][1],
                      d[i].x1 < z[k][0] + z[k][2] ? d[i].x1 : z[k][0] + z[k][2],
                      d[i].y1 < z[k][1] + z[k][3] ? d[i].y1 : z[k][1] + z[k][3]};
            if (c.x0 < 0) c.x0 = 0;
            if (c.y0 < 0) c.y0 = 0;
            if (c.x1 > W) c.x1 = W;
            if (c.y1 > H) c.y1 = H;
            if (c.x0 < c.x1 && c.y0 < c.y1)
                ov_render(&c);
        }
    ov_stale = 0;
    nov = 0;
}

/* Flaeche zum Abdunkeln (beim ersten Dialog angelegt); 0 = geht nicht */
static int ov_dim_ready(void)
{
    if (ov_dim.px)
        return 1;
    if (gsurf_new(&ov_dim, W, H) != 0 || !gsurf_handle(&ov_dim)) {
        gsurf_free(&ov_dim);
        return 0;
    }
    for (u64 i = 0; i < (u64)W * (u64)H; i++)
        ov_dim.px[i] = (u32)DIALOG_DIM << 24;
    return 1;
}

/* Die Ebene ueber ein Rechteck mischen - nur wo gerade etwas liegt, und jede Stelle hoechstens einmal: die Taskleiste
 * (unten, ganze Breite), darueber Menue bzw. Andock-Vorschau; beim Dialog den ganzen Bildschirm */
static int queue_overlay(const Clip *c)
{
    int z[4][4], nz = ov_zones(z);
    for (int k = 0; k < nz; k++) {
        if (dialog_kind && k == nz - 1 - (snap_preview != 0)) { /* wie bei der CPU: erst alles abdunkeln, dann der Kasten */
            if (!ov_dim_ready())
                return 0;
            gq_blend(&gfx_screen, 0, 0, &ov_dim, 0, 0, W, H, c);
        }
        gq_blend(&gfx_screen, z[k][0], z[k][1], &ov_layer, z[k][0], z[k][1], z[k][2], z[k][3], c);
    }
    return 1;
}

/* Geaenderte Fenster neu zeichnen, dann die geaenderten Rechtecke zusammensetzen: Hintergrund, je Fenster Schatten und
 * Bild (runde Ecken, feiner Rand), darueber Taskleiste, Menues usw. Mit der GPU: alles in einer Auftragsliste, und die
 * GPU zeigt das Bild auch selbst an (der Desktop wartet nicht; braucht einen Mauszeiger als eigene Ebene). Sonst mit
 * der CPU. */
void draw_all(void)
{
    gpu_wait(); /* das letzte Bild liest vielleicht noch aus Flaechen, die sich gleich aendern */
    for (int i = 0; i < nord; i++)
        render_window(order[i]);
    if (!ndmg)
        return;
    s64 t0 = sys_time_us(), px = 0;
    int gpu = gpu_usable() && ov_ready();
    if (gpu) {
        ov_update();
    } else { /* Bild mit der CPU: die Ebene beim naechsten GPU-Bild ganz neu */
        nov = 0;
        ov_stale = 1;
    }
    for (int i = 0; gpu && i < ndmg; i++) {
        gpu = queue_gpu(&dmg[i]) && queue_overlay(&dmg[i]);
    }
    for (int i = 0; i < ndmg; i++)
        px += (s64)(dmg[i].x1 - dmg[i].x0) * (dmg[i].y1 - dmg[i].y0);
    if (gpu && (gfx_hw_cursor_on() || gpu_mode == 2)) { /* die GPU setzt zusammen und zeigt an - nicht warten */
        for (int i = 0; i < ndmg; i++)
            gq_present(dmg[i].x0, dmg[i].y0, dmg[i].x1 - dmg[i].x0, dmg[i].y1 - dmg[i].y0);
        if (gq_submit_async() == 0) {
            gpu_stat(2, sys_time_us() - t0, px);
            ndmg = 0;
            return;
        }
        gpu = 0; /* abgelehnt: alles mit der CPU */
    } else if (gpu) {
        gpu = gq_submit() == 0;
    } else {
        gq_cancel();
    }
    if (!gpu)
        for (int i = 0; i < ndmg; i++) {
            compose_cpu(&dmg[i]);
            overlays(&dmg[i]);
        }
    gpu_stat(gpu, sys_time_us() - t0, px);
    for (int i = 0; i < ndmg; i++)
        gfx_present(dmg[i].x0, dmg[i].y0, dmg[i].x1 - dmg[i].x0, dmg[i].y1 - dmg[i].y0);
    ndmg = 0;
}
