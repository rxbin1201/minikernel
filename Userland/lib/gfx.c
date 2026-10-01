/* Grafikbibliothek, siehe gfx.h */

#include "gfx.h"
#include "winproto.h"

Surface        gfx_screen; /* hierhin zeichnet das Programm */
static u32    *gfx_front;       /* gfx_screen plus Mauszeiger: das, was auf dem Bildschirm steht */
Clip           gfx_clip;
static Clip    gfx_base = {0, 0, 1 << 30, 1 << 30}; /* aeussere Grenze: jedes gfx_set_clip wird darauf beschraenkt */
static int     gfx_cur_x, gfx_cur_y, gfx_cur_visible = 1;
static int     gfx_hw_cursor; /* der Kernel zeigt den Zeiger als eigene Ebene (Intel-Grafik): nichts einzeichnen */

/* Fenster unter dem Desktop (winproto.h) */
static int      gfx_win;                    /* 1: gfx_screen ist der Inhalt eines Desktop-Fensters */
static int      gfx_win_dead;               /* Desktop ist weg */
static int      gfx_win_frame;              /* WP_FRAME ist angekommen */
static int      gfx_dx0, gfx_dy0, gfx_dx1, gfx_dy1; /* geaendert, aber noch nicht gemeldet (leer: dx0 >= dx1) */
static int      gfx_scr_w, gfx_scr_h, gfx_ui = 100;
static unsigned gfx_shm_id;
static int      gfx_conn = -1;              /* gfx_desktop(): -1 = noch nicht nachgesehen */
static int      gfx_buf_new;                /* nach WP_RESIZE: neuer Puffer, mit dem naechsten Bild melden */
static int      gfx_fd_in = WP_FD_IN, gfx_fd_out = WP_FD_OUT; /* Pipes zum Desktop */

/* ---------- Speicher ---------- */

void *gfx_alloc(u64 bytes)
{
    s64 a = sys_mmap((bytes + 4095) & ~4095ULL);
    return a < 0 ? 0 : (void *)a;
}

void gfx_free(void *p, u64 bytes)
{
    if (p)
        sys_munmap(p, (bytes + 4095) & ~4095ULL);
}

int surface_new(Surface *s, int w, int h)
{
    s->w = w;
    s->h = h;
    s->px = gfx_alloc((u64)w * (u64)h * 4);
    return s->px ? 0 : -1;
}

void surface_free(Surface *s)
{
    gfx_free(s->px, (u64)s->w * (u64)s->h * 4);
    s->px = 0;
    s->w = s->h = 0;
}

/* ---------- Zeichnen ---------- */

void gfx_set_clip(int x, int y, int w, int h)
{
    gfx_clip.x0 = x < gfx_base.x0 ? gfx_base.x0 : x;
    gfx_clip.y0 = y < gfx_base.y0 ? gfx_base.y0 : y;
    gfx_clip.x1 = x + w > gfx_base.x1 ? gfx_base.x1 : x + w;
    gfx_clip.y1 = y + h > gfx_base.y1 ? gfx_base.y1 : y + h;
}

void gfx_no_clip(void)
{
    gfx_clip = gfx_base;
}

/* Alles Zeichnen (auch nach gfx_no_clip) auf ein Rechteck beschraenken, z.B. um nur einen geaenderten Bereich neu zu
 * zeichnen; gfx_reset_base_clip() hebt das wieder auf */
void gfx_set_base_clip(int x, int y, int w, int h)
{
    gfx_base.x0 = x < 0 ? 0 : x;
    gfx_base.y0 = y < 0 ? 0 : y;
    gfx_base.x1 = x + w;
    gfx_base.y1 = y + h;
    gfx_clip = gfx_base;
}

void gfx_reset_base_clip(void)
{
    gfx_base.x0 = gfx_base.y0 = 0;
    gfx_base.x1 = gfx_base.y1 = 1 << 30;
    gfx_clip = gfx_base;
}

void gfx_pixel(Surface *s, int x, int y, u32 c)
{
    if (x < gfx_clip.x0 || y < gfx_clip.y0 || x >= gfx_clip.x1 || y >= gfx_clip.y1 || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return;
    s->px[(u64)y * (u64)s->w + (u64)x] = c;
}

u32 gfx_get(Surface *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h)
        return 0;
    return s->px[(u64)y * (u64)s->w + (u64)x];
}

void gfx_fill(Surface *s, int x, int y, int w, int h, u32 c)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < gfx_clip.x0) x0 = gfx_clip.x0;
    if (y0 < gfx_clip.y0) y0 = gfx_clip.y0;
    if (x1 > gfx_clip.x1) x1 = gfx_clip.x1;
    if (y1 > gfx_clip.y1) y1 = gfx_clip.y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x0 >= x1)
        return;
    for (int yy = y0; yy < y1; yy++)
        memset32(s->px + (u64)yy * (u64)s->w + (u64)x0, c, (size_t)(x1 - x0));
}

void gfx_rect(Surface *s, int x, int y, int w, int h, u32 c)
{
    gfx_fill(s, x, y, w, 1, c);
    gfx_fill(s, x, y + h - 1, w, 1, c);
    gfx_fill(s, x, y, 1, h, c);
    gfx_fill(s, x + w - 1, y, 1, h, c);
}

/* Rahmen mit Lichtkante (3D): raised = 1 erhaben, 0 eingedrueckt */
void gfx_bevel(Surface *s, int x, int y, int w, int h, int raised)
{
    u32 light = RGB(250, 250, 255), dark = RGB(110, 110, 125);
    gfx_fill(s, x, y, w, 1, raised ? light : dark);
    gfx_fill(s, x, y, 1, h, raised ? light : dark);
    gfx_fill(s, x, y + h - 1, w, 1, raised ? dark : light);
    gfx_fill(s, x + w - 1, y, 1, h, raised ? dark : light);
}

void gfx_line(Surface *s, int x0, int y0, int x1, int y1, u32 c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_fill_circle(Surface *s, int cx, int cy, int r, u32 c)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r)
            w++;
        gfx_fill(s, cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

/* Dicke Linie (Pinsel mit Durchmesser d): Kreise entlang der Linie */
void gfx_thick_line(Surface *s, int x0, int y0, int x1, int y1, int d, u32 c)
{
    if (d <= 1) {
        gfx_line(s, x0, y0, x1, y1, c);
        return;
    }
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int steps = dx > dy ? dx : dy;
    if (!steps)
        steps = 1;
    for (int i = 0; i <= steps; i++)
        gfx_fill_circle(s, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, d / 2, c);
}

void gfx_circle(Surface *s, int cx, int cy, int r, u32 c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(s, cx + x, cy + y, c); gfx_pixel(s, cx + y, cy + x, c);
        gfx_pixel(s, cx - y, cy + x, c); gfx_pixel(s, cx - x, cy + y, c);
        gfx_pixel(s, cx - x, cy - y, c); gfx_pixel(s, cx - y, cy - x, c);
        gfx_pixel(s, cx + y, cy - x, c); gfx_pixel(s, cx + x, cy - y, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

/* Ellipse im Rechteck (x0, y0) - (x1, y1) */
void gfx_ellipse(Surface *s, int x0, int y0, int x1, int y1, u32 c, int filled)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int a = (x1 - x0) / 2, b = (y1 - y0) / 2, cx = x0 + a, cy = y0 + b;
    if (a < 1 || b < 1) {
        gfx_line(s, x0, y0, x1, y1, c);
        return;
    }
    int prev = 0;
    for (int y = b; y >= 0; y--) { /* von oben zum Aequator: x waechst */
        s64 t = (s64)a * a * ((s64)b * b - (s64)y * y) / ((s64)b * b);
        int x = 0;
        while ((s64)(x + 1) * (x + 1) <= t)
            x++;
        if (filled) {
            gfx_fill(s, cx - x, cy + y, 2 * x + 1, 1, c);
            gfx_fill(s, cx - x, cy - y, 2 * x + 1, 1, c);
        } else {
            int from = y == b ? 0 : prev, to = x; /* Luecken an flachen Stellen schliessen */
            if (from > to)
                from = to;
            gfx_fill(s, cx - to, cy + y, to - from + 1, 1, c);
            gfx_fill(s, cx + from, cy + y, to - from + 1, 1, c);
            gfx_fill(s, cx - to, cy - y, to - from + 1, 1, c);
            gfx_fill(s, cx + from, cy - y, to - from + 1, 1, c);
        }
        prev = x;
    }
}

/* ---------- Text ---------- */

static unsigned char gfx_glyphs[256][16];
static unsigned char gfx_glyph_ok[256];
static unsigned char gfx_xglyphs[128][16]; /* weitere Zeichen (z.B. Rahmenlinien): kleiner Cache nach Codepunkt */
static unsigned      gfx_xglyph_cp[128];

const unsigned char *gfx_glyph(unsigned cp)
{
    if (cp < 256) {
        if (!gfx_glyph_ok[cp]) {
            sys_font(cp, gfx_glyphs[cp]);
            gfx_glyph_ok[cp] = 1;
        }
        return gfx_glyphs[cp];
    }
    unsigned i = (cp * 2654435761u) >> 25;
    if (gfx_xglyph_cp[i] != cp) {
        sys_font(cp, gfx_xglyphs[i]);
        gfx_xglyph_cp[i] = cp;
    }
    return gfx_xglyphs[i];
}

unsigned gfx_utf8_next(const char **sp)
{
    const unsigned char *s = (const unsigned char *)*sp;
    unsigned c = s[0];
    int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0) {
        (*sp)++;
        return 0xFFFD;
    }
    unsigned cp = n == 0 ? c : n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            (*sp)++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *sp += n + 1;
    return cp;
}

/* Ein Zeichen zeichnen */
void gfx_char(Surface *s, int x, int y, unsigned cp, u32 fg, u32 bg, int scale)
{
    int x0 = gfx_clip.x0 > 0 ? gfx_clip.x0 : 0, y0 = gfx_clip.y0 > 0 ? gfx_clip.y0 : 0;
    int x1 = gfx_clip.x1 < s->w ? gfx_clip.x1 : s->w, y1 = gfx_clip.y1 < s->h ? gfx_clip.y1 : s->h;
    if (x >= x1 || y >= y1 || x + 8 * scale <= x0 || y + 16 * scale <= y0)
        return; /* ganz ausserhalb */
    const unsigned char *g = gfx_glyph(cp);
    if (scale == 1) {
        int c0 = x0 > x ? x0 - x : 0, c1 = x1 - x < 8 ? x1 - x : 8;
        for (int r = 0; r < 16; r++) {
            int py = y + r;
            if (py < y0 || py >= y1)
                continue;
            u32 *row = s->px + (u64)py * (u64)s->w + (u64)x;
            unsigned bits = g[r];
            for (int c = c0; c < c1; c++) {
                if (bits & (0x80u >> c))
                    row[c] = fg;
                else if (bg != GFX_TRANSPARENT)
                    row[c] = bg;
            }
        }
        return;
    }
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 8; c++) {
            int on = g[r] & (0x80 >> c);
            if (!on && bg == GFX_TRANSPARENT)
                continue;
            if (scale == 1)
                gfx_pixel(s, x + c, y + r, on ? fg : bg);
            else
                gfx_fill(s, x + c * scale, y + r * scale, scale, scale, on ? fg : bg);
        }
}

/* Zeichnet UTF-8-Text (scale = Vergroesserung); bg = GFX_TRANSPARENT: ohne Hintergrund. Liefert die Breite in Pixeln. */
int gfx_text_scaled(Surface *s, int x, int y, const char *t, u32 fg, u32 bg, int scale)
{
    int x0 = x;
    while (*t) {
        unsigned cp = gfx_utf8_next(&t);
        if (cp == '\n') {
            y += 16 * scale;
            x = x0;
            continue;
        }
        gfx_char(s, x, y, cp, fg, bg, scale);
        x += 8 * scale;
    }
    return x - x0;
}

int gfx_text(Surface *s, int x, int y, const char *t, u32 fg, u32 bg)
{
    return gfx_text_scaled(s, x, y, t, fg, bg, 1);
}

int gfx_text_width(const char *t)
{
    return utf8_width(t) * 8;
}

/* Bild (Surface) in ein Rechteck skalieren (naechster Nachbar) */
void gfx_draw_scaled(Surface *dst, const Surface *src, int dx, int dy, int dw, int dh)
{
    if (dw <= 0 || dh <= 0 || !src->w || !src->h)
        return;
    for (int y = 0; y < dh; y++) {
        int py = dy + y;
        if (py < gfx_clip.y0 || py >= gfx_clip.y1 || py < 0 || py >= dst->h)
            continue;
        const u32 *srow = src->px + (u64)((s64)y * src->h / dh) * (u64)src->w;
        u32 *drow = dst->px + (u64)py * (u64)dst->w;
        for (int x = 0; x < dw; x++) {
            int px = dx + x;
            if (px < gfx_clip.x0 || px >= gfx_clip.x1 || px < 0 || px >= dst->w)
                continue;
            drow[px] = srow[(s64)x * src->w / dw];
        }
    }
}

/* ---------- Bildschirm, Mauszeiger ---------- */

#define GFX_CUR_W 12
#define GFX_CUR_H 19
static const char *const gfx_cursor_img[GFX_CUR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ",
    "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ",
    "X    X..X   ", "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};

static void gfx_blit_from(const u32 *src, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gfx_screen.w) w = gfx_screen.w - x;
    if (y + h > gfx_screen.h) h = gfx_screen.h - y;
    if (w <= 0 || h <= 0)
        return;
    GfxBlit b = {(u64)(src + (u64)y * (u64)gfx_screen.w + (u64)x), (unsigned)gfx_screen.w, x, y, w, h};
    sys_gfx(1, &b);
}

/* SYS_GFX 3: Hardware-Mauszeiger setzen; 0 = ok, sonst gibt es keinen */
static s64 gfx_hw_cursor_set(void)
{
    u64 arg = ((u64)(gfx_cur_x & 0xFFFF)) | ((u64)(gfx_cur_y & 0xFFFF) << 16) | ((u64)(gfx_cur_visible != 0) << 32);
    return sys_gfx(3, (const void *)arg);
}

/* Rechteck aus gfx_screen in die Anzeige uebernehmen, Zeiger darueber zeichnen */
void gfx_compose(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gfx_screen.w) w = gfx_screen.w - x;
    if (y + h > gfx_screen.h) h = gfx_screen.h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int yy = y; yy < y + h; yy++)
        memcpy(gfx_front + (u64)yy * (u64)gfx_screen.w + (u64)x, gfx_screen.px + (u64)yy * (u64)gfx_screen.w + (u64)x,
               (u64)w * 4);
    if (!gfx_cur_visible)
        return;
    for (int r = 0; r < GFX_CUR_H; r++) {
        int py = gfx_cur_y + r;
        if (py < y || py >= y + h)
            continue;
        for (int c = 0; c < GFX_CUR_W; c++) {
            int px = gfx_cur_x + c;
            char ch = gfx_cursor_img[r][c];
            if (ch == ' ' || px < x || px >= x + w)
                continue;
            gfx_front[(u64)py * (u64)gfx_screen.w + (u64)px] = ch == 'X' ? 0 : 0xFFFFFF;
        }
    }
}

static void gfx_win_damage(int x, int y, int w, int h);

void gfx_present(int x, int y, int w, int h)
{
    if (gfx_win) {
        gfx_win_damage(x, y, w, h);
        return;
    }
    if (gfx_hw_cursor) { /* Zeiger ist eine eigene Ebene: direkt aus gfx_screen, ohne Zwischenkopie */
        gfx_blit_from(gfx_screen.px, x, y, w, h);
        return;
    }
    gfx_compose(x, y, w, h);
    gfx_blit_from(gfx_front, x, y, w, h);
}

void gfx_present_all(void)
{
    gfx_present(0, 0, gfx_screen.w, gfx_screen.h);
}

void gfx_move_cursor(int x, int y)
{
    if (gfx_win || (x == gfx_cur_x && y == gfx_cur_y)) /* im Fenster gehoert der Zeiger dem Desktop */
        return;
    int ox = gfx_cur_x, oy = gfx_cur_y;
    gfx_cur_x = x;
    gfx_cur_y = y;
    if (gfx_hw_cursor) {
        gfx_hw_cursor_set();
        return;
    }
    gfx_present(ox, oy, GFX_CUR_W, GFX_CUR_H);
    gfx_present(x, y, GFX_CUR_W, GFX_CUR_H);
}

void gfx_show_cursor(int visible)
{
    if (gfx_win)
        return;
    gfx_cur_visible = visible;
    if (gfx_hw_cursor) {
        gfx_hw_cursor_set();
        return;
    }
    gfx_present(gfx_cur_x, gfx_cur_y, GFX_CUR_W, GFX_CUR_H);
}

/* ---------- Ereignisse ---------- */

static Event     gfx_q[64];
static int       gfx_qh, gfx_qt;
static MouseInfo gfx_mprev;
static int       gfx_left_down, gfx_right_down;

static void gfx_push(Event e)
{
    int n = (gfx_qh + 1) % 64;
    if (n == gfx_qt)
        return;
    gfx_q[gfx_qh] = e;
    gfx_qh = n;
}

static void gfx_collect(void)
{
    s64 c;
    for (int i = 0; i < 32 && (c = sys_getchar()) >= 0; i++) {
        int k = (int)c;
        if (k == KEY_ALT || k == KEY_ALT_SHIFT) { /* Alt-Kombination: die Taste kommt gleich dahinter */
            s64 n = sys_getchar();
            if (n < 0)
                continue;
            k = (int)n | KEY_MOD_ALT | (c == KEY_ALT_SHIFT ? KEY_MOD_SHIFT : 0);
        }
        Event e = {EV_KEY, k, gfx_cur_x, gfx_cur_y, 0, 0};
        gfx_push(e);
    }
    MouseInfo m;
    if (sys_mouse(&m) != 0 || m.events == gfx_mprev.events)
        return;
    if (m.x != gfx_cur_x || m.y != gfx_cur_y) {
        gfx_move_cursor(m.x, m.y);
        Event e = {EV_MOVE, 0, m.x, m.y, 0, 0};
        gfx_push(e);
    }
    for (unsigned k = gfx_mprev.left_presses; k != m.left_presses; k++) { /* jeder Druck, auch sehr kurze Klicks */
        if (gfx_left_down) {
            Event u = {EV_UP, 0, m.press_x, m.press_y, 1, 0};
            gfx_push(u);
        }
        Event e = {EV_DOWN, 0, m.press_x, m.press_y, 1, 0};
        gfx_push(e);
        gfx_left_down = 1;
    }
    if (gfx_left_down && !(m.buttons & 1)) {
        Event e = {EV_UP, 0, m.x, m.y, 1, 0};
        gfx_push(e);
        gfx_left_down = 0;
    }
    for (unsigned k = gfx_mprev.right_presses; k != m.right_presses; k++) {
        Event e = {EV_DOWN, 0, m.x, m.y, 2, 0};
        gfx_push(e);
        gfx_right_down = 1;
    }
    if (gfx_right_down && !(m.buttons & 2)) {
        Event e = {EV_UP, 0, m.x, m.y, 2, 0};
        gfx_push(e);
        gfx_right_down = 0;
    }
    if (m.wheel) {
        Event e = {EV_WHEEL, 0, m.x, m.y, 0, m.wheel};
        gfx_push(e);
    }
    gfx_mprev = m;
}

/* ---------- Fenster unter dem Desktop ---------- */

static void wp_send(WpMsg *m)
{
    if (!gfx_win_dead && write_all(gfx_fd_out, m, sizeof(*m)) != 0)
        gfx_win_dead = 1;
}

/* Eine ganze Nachricht lesen, wenn eine da ist */
static int wp_recv(WpMsg *m)
{
    s64 n = sys_fdavail(gfx_fd_in);
    if (n < 0) {
        gfx_win_dead = 1;
        return 0;
    }
    if (n < (s64)sizeof(*m))
        return 0;
    u64 got = 0;
    while (got < sizeof(*m)) {
        s64 r = sys_read(gfx_fd_in, (char *)m + got, sizeof(*m) - got);
        if (r <= 0) {
            gfx_win_dead = 1;
            return 0;
        }
        got += (u64)r;
    }
    return 1;
}

static void gfx_win_damage(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gfx_screen.w) w = gfx_screen.w - x;
    if (y + h > gfx_screen.h) h = gfx_screen.h - y;
    if (w <= 0 || h <= 0)
        return;
    if (gfx_dx0 >= gfx_dx1) {
        gfx_dx0 = x; gfx_dy0 = y; gfx_dx1 = x + w; gfx_dy1 = y + h;
        return;
    }
    if (x < gfx_dx0) gfx_dx0 = x;
    if (y < gfx_dy0) gfx_dy0 = y;
    if (x + w > gfx_dx1) gfx_dx1 = x + w;
    if (y + h > gfx_dy1) gfx_dy1 = y + h;
}

/* Geaenderten Bereich melden (gesammelt bis zum naechsten gfx_poll/gfx_vsync: eine Nachricht statt vieler) */
static void gfx_win_flush(void)
{
    if (gfx_buf_new) { /* neuer Puffer ist gezeichnet: erst melden, dann ganz anzeigen lassen */
        if (gfx_dx0 >= gfx_dx1)
            return; /* noch nichts gezeichnet: der Desktop zeigt so lange den alten */
        WpMsg b = {WP_BUFFER, gfx_screen.w, gfx_screen.h, (int)gfx_shm_id, 0, 0, 0, 0, {0}};
        wp_send(&b);
        gfx_buf_new = 0;
        gfx_dx0 = gfx_dy0 = 0;
        gfx_dx1 = gfx_screen.w;
        gfx_dy1 = gfx_screen.h;
    }
    if (gfx_dx0 >= gfx_dx1)
        return;
    WpMsg m = {WP_DAMAGE, gfx_dx0, gfx_dy0, gfx_dx1 - gfx_dx0, gfx_dy1 - gfx_dy0, 0, 0, 0, {0}};
    wp_send(&m);
    gfx_dx0 = gfx_dx1 = 0;
}

/* Neue Groesse: neuen geteilten Puffer anlegen (alter Inhalt oben links uebernommen), das Programm zeichnet neu */
static void gfx_win_resize(int w, int h)
{
    if (w < 16 || h < 16 || w > 16384 || h > 16384 || (w == gfx_screen.w && h == gfx_screen.h))
        return;
    unsigned id;
    s64 a = sys_shm_create((u64)w * (u64)h * 4, &id);
    if (a < 0)
        return;
    u32 *np = (u32 *)a, *op = gfx_screen.px;
    int cw = w < gfx_screen.w ? w : gfx_screen.w, chh = h < gfx_screen.h ? h : gfx_screen.h;
    for (int y = 0; y < chh; y++)
        memcpy(np + (u64)y * (u64)w, op + (u64)y * (u64)gfx_screen.w, (u64)cw * 4);
    sys_shm_unmap(op); /* der Desktop haelt den alten Puffer, bis WP_BUFFER kommt */
    gfx_screen.px = np;
    gfx_screen.w = w;
    gfx_screen.h = h;
    gfx_shm_id = id;
    gfx_buf_new = 1;
    gfx_dx0 = gfx_dx1 = 0;
    gfx_reset_base_clip();
    gfx_no_clip();
    Event e = {EV_RESIZE, 0, w, h, 0, 0};
    gfx_push(e);
}

static void gfx_win_collect(void)
{
    WpMsg m;
    while (wp_recv(&m)) {
        if (m.type == WP_INPUT) {
            Event e = {m.a, m.b, m.c, m.d, m.e, m.f};
            gfx_push(e);
        } else if (m.type == WP_FOCUS) {
            Event e = {EV_FOCUS, m.a, 0, 0, 0, 0};
            gfx_push(e);
        } else if (m.type == WP_CLOSE) {
            Event e = {EV_CLOSE, 0, 0, 0, 0, 0};
            gfx_push(e);
        } else if (m.type == WP_FRAME) {
            gfx_win_frame = 1;
        } else if (m.type == WP_RESIZE) {
            gfx_win_resize(m.a, m.b);
        }
    }
    static int reported;
    if (gfx_win_dead && !reported) { /* Desktop beendet: das Programm soll auch gehen */
        reported = 1;
        Event e = {EV_CLOSE, 0, 0, 0, 0, 0};
        gfx_push(e);
    }
}

static int gfx_hello(const WpMsg *m)
{
    if (m->type != WP_HELLO || m->d != WP_MAGIC)
        return 0;
    gfx_scr_w = m->a;
    gfx_scr_h = m->b;
    gfx_ui = m->c > 0 ? m->c : 100;
    return 1;
}

/* Laeuft ein Desktop? Hat er das Programm selbst gestartet, liegt seine Begruessung schon in Deskriptor 3 (andere
 * Deskriptoren - Datei: 1 Byte "verfuegbar", Konsole: 0, nicht offen: Fehler - kommen nicht auf 64 Byte). Sonst (z.B.
 * im Terminal gestartet) beim Dienst "desktop" anklopfen: der nimmt mit seinem naechsten Bild an und gruesst. */
static int gfx_win_connect(void)
{
    WpMsg m;
    if (sys_fdavail(WP_FD_IN) >= (s64)sizeof(WpMsg)) {
        if (wp_recv(&m) && gfx_hello(&m))
            return 1;
        gfx_win_dead = 0;
        return 0;
    }
    int fds[2];
    if (sys_service_connect("desktop", fds) != 0)
        return 0;
    gfx_fd_in = fds[0];
    gfx_fd_out = fds[1];
    s64 t0 = sys_time_us();
    while (sys_time_us() - t0 < 2000000) {
        s64 n = sys_fdavail(gfx_fd_in);
        if (n < 0)
            break;
        if (n >= (s64)sizeof(m)) {
            if (wp_recv(&m) && gfx_hello(&m))
                return 1;
            break;
        }
        sys_sleep_ms(5);
    }
    sys_close(fds[0]); /* Desktop antwortet nicht: dann eben ohne ihn */
    sys_close(fds[1]);
    gfx_fd_in = WP_FD_IN;
    gfx_fd_out = WP_FD_OUT;
    gfx_win_dead = 0;
    return 0;
}

static int gfx_win_open(int w, int h, const char *title, int flags)
{
    if (w <= 0 || h <= 0) {
        w = gfx_scr_w * 3 / 5;
        h = gfx_scr_h * 3 / 5;
    }
    if (w > gfx_scr_w - 40) w = gfx_scr_w - 40;
    if (h > gfx_scr_h - 160) h = gfx_scr_h - 160;
    if (w < 64) w = 64;
    if (h < 32) h = 32;
    s64 a = sys_shm_create((u64)w * (u64)h * 4, &gfx_shm_id);
    if (a < 0) {
        fprintf(2, "Grafik: kein Speicher fuer ein Fenster %dx%d\n", w, h);
        return -1;
    }
    gfx_screen.px = (u32 *)a;
    gfx_screen.w = w;
    gfx_screen.h = h;
    gfx_no_clip();
    gfx_qh = gfx_qt = 0;
    gfx_dx0 = gfx_dx1 = 0;
    gfx_win = 1;
    WpMsg m = {WP_CREATE, w, h, (int)gfx_shm_id, flags & GFX_RESIZABLE ? WPF_RESIZABLE : 0, 0, 0, 0, {0}};
    snprintf(m.text, sizeof(m.text), "%s", title ? title : "");
    wp_send(&m);
    return 0;
}

void gfx_set_title(const char *title)
{
    if (!gfx_win)
        return;
    WpMsg m = {WP_TITLE, 0, 0, 0, 0, 0, 0, 0, {0}};
    snprintf(m.text, sizeof(m.text), "%s", title);
    wp_send(&m);
}

int gfx_desktop_open(const char *path)
{
    if (!gfx_win || gfx_win_dead)
        return -1;
    int n = (int)strlen(path);
    if (n >= WP_PATH_MAX)
        return -1;
    for (int off = 0; off == 0 || off < n; off += 32) {
        int c = n - off < 32 ? n - off : 32;
        WpMsg m = {WP_OPEN, off, off + c >= n, c, 0, 0, 0, 0, {0}};
        memcpy(m.text, path + off, (size_t)c);
        wp_send(&m);
        if (c < 32)
            break;
    }
    return 0;
}

int gfx_windowed(void)
{
    return gfx_win;
}

int gfx_desktop(void)
{
    if (gfx_conn < 0)
        gfx_conn = gfx_win_connect();
    return gfx_conn;
}

void gfx_display_size(int *w, int *h)
{
    int d = gfx_desktop();
    *w = d ? gfx_scr_w : gfx_screen.w;
    *h = d ? gfx_scr_h : gfx_screen.h;
}

int gfx_ui_scale(void)
{
    if (gfx_desktop())
        return gfx_ui;
    return gfx_screen.h >= 1300 ? 125 : 100;
}

/* Naechstes Ereignis; 0 = keins (nicht blockierend) */
int gfx_poll(Event *e)
{
    if (gfx_win) {
        gfx_win_flush();
        if (gfx_qt == gfx_qh)
            gfx_win_collect();
    } else if (gfx_qt == gfx_qh) {
        gfx_collect();
    }
    if (gfx_qt == gfx_qh)
        return 0;
    *e = gfx_q[gfx_qt];
    gfx_qt = (gfx_qt + 1) % 64;
    return 1;
}

/* Wartet hoechstens timeout_ms auf ein Ereignis (-1 = ohne Grenze). 0 = keins */
int gfx_wait(Event *e, int timeout_ms)
{
    s64 start = sys_ticks();
    for (;;) {
        if (gfx_poll(e))
            return 1;
        if (timeout_ms >= 0 && (sys_ticks() - start) * 10 >= timeout_ms)
            return 0;
        sys_sleep_ms(10);
    }
}

/* ---------- Oeffnen / Schliessen ---------- */

int gfx_open(void)
{
    return gfx_open_window(0, 0, 0);
}

int gfx_open_window(int ww, int wh, const char *title)
{
    return gfx_open_window_ex(ww, wh, title, 0);
}

int gfx_open_window_ex(int ww, int wh, const char *title, int flags)
{
    if (gfx_desktop()) {
        if (gfx_win_dead)
            return -1;
        return gfx_win_open(ww, wh, title, flags);
    }
    s64 r = sys_gfx(0, 0);
    if (r < 0) {
        fprintf(2, "Grafik: Bildschirm nicht verfuegbar (%s)\n", r == -11 ? "wird schon benutzt" : "keine Konsole");
        return -1;
    }
    int w = (int)(r >> 32), h = (int)(r & 0xFFFFFFFF);
    if (surface_new(&gfx_screen, w, h) != 0 || !(gfx_front = gfx_alloc((u64)w * (u64)h * 4))) {
        sys_gfx(2, 0);
        fprintf(2, "Grafik: kein Speicher fuer %dx%d\n", w, h);
        return -1;
    }
    gfx_no_clip();
    sys_mouse(&gfx_mprev);
    gfx_cur_x = gfx_mprev.x;
    gfx_cur_y = gfx_mprev.y;
    gfx_cur_visible = gfx_mprev.attached != 0;
    gfx_hw_cursor = gfx_hw_cursor_set() == 0;
    gfx_qh = gfx_qt = 0;
    gfx_left_down = gfx_right_down = 0;
    while (sys_getchar() >= 0) /* alte Tasten verwerfen */
        ;
    return 0;
}

void gfx_close(void)
{
    if (gfx_win) { /* Pipes zu: der Desktop schliesst das Fenster */
        gfx_win_flush();
        sys_close(gfx_fd_in);
        sys_close(gfx_fd_out);
        sys_shm_unmap(gfx_screen.px);
        gfx_screen.px = 0;
        gfx_win = 0;
        gfx_win_dead = 1; /* die Pipes sind zu: kein zweites Fenster */
        return;
    }
    sys_gfx(2, 0);
    gfx_free(gfx_front, (u64)gfx_screen.w * (u64)gfx_screen.h * 4);
    surface_free(&gfx_screen);
    gfx_front = 0;
}

/* Bildschirm voruebergehend abgeben (z.B. um ein anderes Grafikprogramm zu starten) und wieder uebernehmen */
void gfx_suspend(void)
{
    if (gfx_win)
        return;
    sys_gfx(2, 0);
}

int gfx_resume(void)
{
    if (gfx_win)
        return 0;
    if (sys_gfx(0, 0) < 0)
        return -1;
    sys_mouse(&gfx_mprev);
    gfx_cur_x = gfx_mprev.x;
    gfx_cur_y = gfx_mprev.y;
    gfx_left_down = gfx_right_down = 0;
    if (gfx_hw_cursor)
        gfx_hw_cursor_set();
    gfx_present_all();
    return 0;
}

/* ---------- BMP-Dateien (24 und 32 Bit, unkomprimiert) ---------- */

static u32 gfx_le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static int gfx_le16(const unsigned char *p) { return p[0] | (p[1] << 8); }

/* Laedt ein BMP. 0 = ok; Fehlermeldung in err */
int bmp_load(const char *path, Surface *out, char *err, int errmax)
{
    Stat st;
    if (sys_stat(path, &st) != 0 || st.is_dir) {
        snprintf(err, (size_t)errmax, "nicht gefunden");
        return -1;
    }
    unsigned char *data = gfx_alloc(st.size + 1);
    if (!data) {
        snprintf(err, (size_t)errmax, "kein Speicher");
        return -1;
    }
    s64 fd = sys_open(path, O_RDONLY);
    u64 got = 0;
    s64 r;
    while (fd >= 0 && got < st.size && (r = sys_read((int)fd, data + got, st.size - got)) > 0)
        got += (u64)r;
    if (fd >= 0)
        sys_close((int)fd);
    int ok = -1;
    if (got < 54 || data[0] != 'B' || data[1] != 'M') {
        snprintf(err, (size_t)errmax, "kein BMP-Bild");
    } else {
        u32 off = gfx_le32(data + 10);
        int w = (int)gfx_le32(data + 18), hh = (int)gfx_le32(data + 22), bpp = gfx_le16(data + 28);
        u32 comp = gfx_le32(data + 30);
        int top_down = hh < 0, h = hh < 0 ? -hh : hh;
        if ((bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
            snprintf(err, (size_t)errmax, "nicht unterstuetzt (%d Bit, Kompression %u)", bpp, comp);
        } else {
            u64 stride = ((u64)w * (u64)bpp / 8 + 3) & ~3ULL;
            if (off + stride * (u64)h > got) {
                snprintf(err, (size_t)errmax, "Datei unvollstaendig");
            } else if (surface_new(out, w, h) != 0) {
                snprintf(err, (size_t)errmax, "kein Speicher");
            } else {
                for (int y = 0; y < h; y++) {
                    const unsigned char *row = data + off + stride * (u64)(top_down ? y : h - 1 - y);
                    for (int x = 0; x < w; x++) {
                        const unsigned char *p = row + x * (bpp / 8);
                        out->px[(u64)y * (u64)w + (u64)x] = RGB(p[2], p[1], p[0]);
                    }
                }
                ok = 0;
            }
        }
    }
    gfx_free(data, st.size + 1);
    return ok;
}

/* Speichert ein Rechteck als 24-Bit-BMP. 0 = ok */
int bmp_save(const char *path, const Surface *s, int x, int y, int w, int h)
{
    u64 stride = ((u64)w * 3 + 3) & ~3ULL, size = 54 + stride * (u64)h;
    unsigned char *d = gfx_alloc(size);
    if (!d)
        return -1;
    memset(d, 0, size);
    d[0] = 'B';
    d[1] = 'M';
#define PUT32(o, v) do { u32 v_ = (u32)(v); d[o] = (unsigned char)v_; d[o + 1] = (unsigned char)(v_ >> 8); d[o + 2] = (unsigned char)(v_ >> 16); d[o + 3] = (unsigned char)(v_ >> 24); } while (0)
    PUT32(2, size);
    PUT32(10, 54);
    PUT32(14, 40);
    PUT32(18, w);
    PUT32(22, h);
    d[26] = 1;
    d[28] = 24;
    PUT32(34, stride * (u64)h);
    PUT32(38, 2835);
    PUT32(42, 2835);
#undef PUT32
    for (int r = 0; r < h; r++) {
        unsigned char *row = d + 54 + stride * (u64)(h - 1 - r);
        for (int c = 0; c < w; c++) {
            u32 p = s->px[(u64)(y + r) * (u64)s->w + (u64)(x + c)];
            row[c * 3] = (unsigned char)p;
            row[c * 3 + 1] = (unsigned char)(p >> 8);
            row[c * 3 + 2] = (unsigned char)(p >> 16);
        }
    }
    s64 fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    int rc = fd < 0 ? (int)fd : write_all((int)fd, d, size);
    if (fd >= 0)
        sys_close((int)fd);
    gfx_free(d, size);
    return rc;
}

int gfx_vsync(void)
{
    if (gfx_win) { /* im Fenster: im Takt des Desktops (er meldet sich nach seinem naechsten Bild) */
        gfx_win_flush();
        gfx_win_frame = 0;
        WpMsg m = {WP_WANT_FRAME, 0, 0, 0, 0, 0, 0, 0, {0}};
        wp_send(&m);
        s64 t0 = sys_time_us();
        while (!gfx_win_dead && sys_time_us() - t0 < 50000) {
            gfx_win_collect();
            if (gfx_win_frame)
                return 1;
            sys_sleep_ms(1);
        }
        return 0;
    }
    if (sys_gfx(4, 0) >= 0)
        return 1;
    sys_sleep_ms(10);
    return 0;
}
