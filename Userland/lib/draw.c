/* Zeichnen mit Kantenglaettung und Transparenz (siehe gfx.h, Abschnitt "Glatt zeichnen"). Rechnet mit float; alle
 * Funktionen halten sich an gfx_clip und die Surface-Grenzen. Alpha: 0 = unsichtbar, 255 = deckend. */

#include "gfx.h"
#include "malloc.h"

u32 gfx_mix(u32 under, u32 over, int alpha)
{
    int a = alpha <= 0 ? 0 : alpha >= 255 ? 256 : alpha;
    int r = (int)((under >> 16) & 0xFF), g = (int)((under >> 8) & 0xFF), b = (int)(under & 0xFF);
    r += (((int)((over >> 16) & 0xFF) - r) * a) >> 8;
    g += (((int)((over >> 8) & 0xFF) - g) * a) >> 8;
    b += (((int)(over & 0xFF) - b) * a) >> 8;
    return (u32)r << 16 | (u32)g << 8 | (u32)b;
}

/* sichtbarer Bereich (Clip und Surface) fuer ein Rechteck; 0 = leer */
static int clip_box(Surface *s, int *x0, int *y0, int *x1, int *y1)
{
    if (*x0 < gfx_clip.x0) *x0 = gfx_clip.x0;
    if (*y0 < gfx_clip.y0) *y0 = gfx_clip.y0;
    if (*x1 > gfx_clip.x1) *x1 = gfx_clip.x1;
    if (*y1 > gfx_clip.y1) *y1 = gfx_clip.y1;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > s->w) *x1 = s->w;
    if (*y1 > s->h) *y1 = s->h;
    return *x0 < *x1 && *y0 < *y1;
}

static inline void blend_px(Surface *s, int x, int y, u32 c, int a)
{
    u32 *p = s->px + (u64)y * (u64)s->w + (u64)x;
    *p = a >= 255 ? c : gfx_mix(*p, c, a);
}

void gfx_blend_fill(Surface *s, int x, int y, int w, int h, u32 c, int alpha)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (alpha <= 0 || !clip_box(s, &x0, &y0, &x1, &y1))
        return;
    if (alpha >= 255) {
        gfx_fill(s, x, y, w, h, c);
        return;
    }
    for (int yy = y0; yy < y1; yy++) {
        u32 *p = s->px + (u64)yy * (u64)s->w;
        for (int xx = x0; xx < x1; xx++)
            p[xx] = gfx_mix(p[xx], c, alpha);
    }
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Abstand von (px, py) zum abgerundeten Rechteck (negativ = innen) */
static inline float rrect_dist(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = (px < cx ? cx - px : px - cx) - (hw - r), qy = (py < cy ? cy - py : py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return __builtin_sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

void gfx_round_rect(Surface *s, int x, int y, int w, int h, int r, u32 c, int alpha)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    /* Mitte ohne Ecken: schnell fuellen */
    gfx_blend_fill(s, x + r, y, w - 2 * r, h, c, alpha);
    gfx_blend_fill(s, x, y + r, r, h - 2 * r, c, alpha);
    gfx_blend_fill(s, x + w - r, y + r, r, h - 2 * r, c, alpha);
    if (r <= 0)
        return;
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int corners[4][2] = {{x, y}, {x + w - r, y}, {x, y + h - r}, {x + w - r, y + h - r}};
    for (int k = 0; k < 4; k++) {
        int x0 = corners[k][0], y0 = corners[k][1], x1 = x0 + r, y1 = y0 + r;
        if (!clip_box(s, &x0, &y0, &x1, &y1))
            continue;
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) {
                float cov = clampf(0.5f - rrect_dist(xx + 0.5f, yy + 0.5f, cx, cy, hw, hh, (float)r), 0, 1);
                if (cov > 0)
                    blend_px(s, xx, yy, c, (int)(cov * alpha + 0.5f));
            }
    }
}

void gfx_round_frame(Surface *s, int x, int y, int w, int h, int r, u32 c, int alpha)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_box(s, &x0, &y0, &x1, &y1))
        return;
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    for (int yy = y0; yy < y1; yy++) {
        int inner = yy >= y + r && yy < y + h - r;
        for (int xx = x0; xx < x1; xx++) {
            if (inner && xx > x + 1 && xx < x + w - 2) { /* innen liegt keine Kante: springen */
                xx = x + w - 3;
                continue;
            }
            float d = rrect_dist(xx + 0.5f, yy + 0.5f, cx, cy, hw, hh, (float)r) + 0.5f; /* Strich 1 px innen */
            float cov = clampf(1.0f - (d < 0 ? -d : d), 0, 1);
            if (cov > 0)
                blend_px(s, xx, yy, c, (int)(cov * alpha + 0.5f));
        }
    }
}

void gfx_shadow(Surface *s, int x, int y, int w, int h, int r, int blur, int alpha)
{
    int x0 = x - blur, y0 = y - blur, x1 = x + w + blur, y1 = y + h + blur;
    if (!clip_box(s, &x0, &y0, &x1, &y1))
        return;
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f, inv = 1.0f / (float)(blur > 0 ? blur : 1);
    for (int yy = y0; yy < y1; yy++) {
        int inner = yy >= y + r && yy < y + h - r;
        for (int xx = x0; xx < x1; xx++) {
            if (inner && xx >= x + r && xx < x + w - r) { /* unter dem Fenster: verdeckt, springen */
                xx = x + w - r - 1;
                continue;
            }
            float d = rrect_dist(xx + 0.5f, yy + 0.5f, cx, cy, hw, hh, (float)r);
            if (d >= blur)
                continue;
            float t = clampf(d * inv, 0, 1), f = 1 - t;
            float a = f * f * f * (float)alpha; /* weich auslaufend */
            if (a >= 1)
                blend_px(s, xx, yy, 0, (int)a);
        }
    }
}

void gfx_gradient(Surface *s, int x, int y, int w, int h, u32 top, u32 bottom)
{
    for (int i = 0; i < h; i++)
        gfx_fill(s, x, y + i, w, 1, gfx_mix(top, bottom, h > 1 ? i * 255 / (h - 1) : 0));
}

void gfx_disc(Surface *s, float cx, float cy, float r, u32 c, int alpha)
{
    int x0 = (int)(cx - r - 1), y0 = (int)(cy - r - 1), x1 = (int)(cx + r + 2), y1 = (int)(cy + r + 2);
    if (!clip_box(s, &x0, &y0, &x1, &y1))
        return;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            float dx = xx + 0.5f - cx, dy = yy + 0.5f - cy;
            float cov = clampf(r + 0.5f - __builtin_sqrtf(dx * dx + dy * dy), 0, 1);
            if (cov > 0)
                blend_px(s, xx, yy, c, (int)(cov * alpha + 0.5f));
        }
}

void gfx_ring(Surface *s, float cx, float cy, float r, float width, u32 c, int alpha)
{
    int x0 = (int)(cx - r - 1), y0 = (int)(cy - r - 1), x1 = (int)(cx + r + 2), y1 = (int)(cy + r + 2);
    if (!clip_box(s, &x0, &y0, &x1, &y1))
        return;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            float dx = xx + 0.5f - cx, dy = yy + 0.5f - cy, d = __builtin_sqrtf(dx * dx + dy * dy);
            float cov = clampf(r + 0.5f - d, 0, 1) * clampf(d - (r - width) + 0.5f, 0, 1);
            if (cov > 0)
                blend_px(s, xx, yy, c, (int)(cov * alpha + 0.5f));
        }
}

/* Linie mit runden Enden (Kapsel), Breite width */
void gfx_capsule(Surface *s, float ax, float ay, float bx, float by, float width, u32 c, int alpha)
{
    float r = width * 0.5f;
    int x0 = (int)((ax < bx ? ax : bx) - r - 1), y0 = (int)((ay < by ? ay : by) - r - 1);
    int x1 = (int)((ax > bx ? ax : bx) + r + 2), y1 = (int)((ay > by ? ay : by) + r + 2);
    if (!clip_box(s, &x0, &y0, &x1, &y1))
        return;
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            float px = xx + 0.5f - ax, py = yy + 0.5f - ay;
            float t = len2 > 0 ? clampf((px * dx + py * dy) / len2, 0, 1) : 0;
            float qx = px - t * dx, qy = py - t * dy;
            float cov = clampf(r + 0.5f - __builtin_sqrtf(qx * qx + qy * qy), 0, 1);
            if (cov > 0)
                blend_px(s, xx, yy, c, (int)(cov * alpha + 0.5f));
        }
}

/* Weichzeichnen (dreimal Kastenfilter, ergibt fast einen Gauss) eines ganzen Bildes */
void gfx_blur(Surface *s, int radius)
{
    int n = s->w > s->h ? s->w : s->h;
    u32 *line = u_malloc((u64)n * 4);
    if (!line || radius < 1)
        return;
    for (int pass = 0; pass < 3; pass++)
        for (int dir = 0; dir < 2; dir++) {
            int count = dir == 0 ? s->h : s->w, len = dir == 0 ? s->w : s->h;
            for (int k = 0; k < count; k++) {
                u64 step = dir == 0 ? 1 : (u64)s->w;
                u32 *base = s->px + (dir == 0 ? (u64)k * (u64)s->w : (u64)k);
                for (int i = 0; i < len; i++)
                    line[i] = base[(u64)i * step];
                int sr = 0, sg = 0, sb = 0, win = 2 * radius + 1;
                for (int i = -radius; i <= radius; i++) {
                    u32 p = line[i < 0 ? 0 : i >= len ? len - 1 : i];
                    sr += (int)(p >> 16 & 0xFF);
                    sg += (int)(p >> 8 & 0xFF);
                    sb += (int)(p & 0xFF);
                }
                for (int i = 0; i < len; i++) {
                    base[(u64)i * step] = (u32)(sr / win) << 16 | (u32)(sg / win) << 8 | (u32)(sb / win);
                    u32 out = line[i - radius < 0 ? 0 : i - radius], in = line[i + radius + 1 >= len ? len - 1 : i + radius + 1];
                    sr += (int)(in >> 16 & 0xFF) - (int)(out >> 16 & 0xFF);
                    sg += (int)(in >> 8 & 0xFF) - (int)(out >> 8 & 0xFF);
                    sb += (int)(in & 0xFF) - (int)(out & 0xFF);
                }
            }
        }
    u_free(line);
}

/* Deckung eines Pixels (xx, yy) im abgerundeten Rechteck (x, y, w, h, r): 0-256; innen schnell 256 */
static inline int round_cov(int xx, int yy, int x, int y, int w, int h, int r);

int gfx_round_cov(int xx, int yy, int x, int y, int w, int h, int r)
{
    return round_cov(xx, yy, x, y, w, h, r);
}

void gfx_shadow_image(Surface *s, int x0, int y0, int rw, int rh, int x, int y, int w, int h, int r, int blur, int alpha)
{
    int x1 = x0 + rw, y1 = y0 + rh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f, inv = 1.0f / (float)(blur > 0 ? blur : 1);
    for (int yy = y0; yy < y1; yy++) {
        u32 *p = s->px + (u64)yy * (u64)s->w, flat = 0;
        int have = 0;
        for (int xx = x0; xx < x1; xx++) {
            int straight = xx >= x + r && xx < x + w - r; /* gerades Stueck: in der ganzen Zeile derselbe Wert */
            if (straight && have) {
                p[xx] = flat;
                continue;
            }
            float d = rrect_dist(xx + 0.5f, yy + 0.5f, cx, cy, hw, hh, (float)r);
            u32 v = 0;
            if (d < blur) {
                float t = clampf(d * inv, 0, 1), f = 1 - t, a = f * f * f * (float)alpha;
                if (a >= 1)
                    v = (u32)(int)a << 24;
            }
            p[xx] = v;
            if (straight) {
                flat = v;
                have = 1;
            }
        }
    }
}

static inline int round_cov(int xx, int yy, int x, int y, int w, int h, int r)
{
    int ix = xx < x + r ? x + r - xx : xx >= x + w - r ? xx - (x + w - r - 1) : 0;
    int iy = yy < y + r ? y + r - yy : yy >= y + h - r ? yy - (y + h - r - 1) : 0;
    if (!ix || !iy)
        return 256;
    float cx = ix - 0.5f, cy = iy - 0.5f; /* Abstand vom Eckmittelpunkt (in Pixeln) */
    float d = __builtin_sqrtf(cx * cx + cy * cy);
    float cov = clampf((float)r - d + 0.5f, 0, 1);
    return (int)(cov * 256 + 0.5f);
}

void gfx_blit_round(Surface *dst, const Surface *src, int sx, int sy, int dx, int dy, int w, int h, int r)
{
    int x0 = dx, y0 = dy, x1 = dx + w, y1 = dy + h;
    if (!clip_box(dst, &x0, &y0, &x1, &y1))
        return;
    for (int yy = y0; yy < y1; yy++) {
        const u32 *sp = src->px + (u64)(yy - dy + sy) * (u64)src->w + (u64)(sx - dx);
        u32 *dp = dst->px + (u64)yy * (u64)dst->w;
        if (yy >= dy + r && yy < dy + h - r) { /* keine Ecke in dieser Zeile */
            memcpy(dp + x0, sp + x0, (u64)(x1 - x0) * 4);
            continue;
        }
        for (int xx = x0; xx < x1; xx++) {
            int c = round_cov(xx, yy, dx, dy, w, h, r);
            if (c >= 256)
                dp[xx] = sp[xx];
            else if (c > 0)
                dp[xx] = gfx_mix(dp[xx], sp[xx], c > 255 ? 255 : c);
        }
    }
}

void gfx_round_rect_grad(Surface *s, int x, int y, int w, int h, int r, u32 top, u32 bottom, int alpha)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_box(s, &x0, &y0, &x1, &y1))
        return;
    for (int yy = y0; yy < y1; yy++) {
        u32 c = gfx_mix(top, bottom, h > 1 ? (yy - y) * 255 / (h - 1) : 0);
        u32 *p = s->px + (u64)yy * (u64)s->w;
        for (int xx = x0; xx < x1; xx++) {
            int cov = round_cov(xx, yy, x, y, w, h, r);
            int a = cov * alpha / 256;
            if (a >= 255)
                p[xx] = c;
            else if (a > 0)
                p[xx] = gfx_mix(p[xx], c, a);
        }
    }
}

/* Ganzes Bild src auf das Rechteck (dx, dy, dw, dh) skalieren, mit runden Ecken (Radius r) und Transparenz alpha:
 * fuer Animationen (Fenster auf/zu, minimieren, maximieren). Kleine Flaechen bilinear; grosse (ab 1 Mio. Pixel, z.B.
 * beim Maximieren auf 3440x1440) mit dem naechsten Pixel - in der Bewegung sieht man keinen Unterschied, es ist aber
 * ein Vielfaches schneller (Zeilen mit derselben Quellzeile werden nur kopiert). Die Quellspalten werden je Aufruf
 * einmal berechnet, die Eckenrundung nur in den Ecken. */
#define SCALED_MAXW 8192
void gfx_blit_scaled(Surface *dst, const Surface *src, int dx, int dy, int dw, int dh, int alpha, int r)
{
    static u32 colx[SCALED_MAXW]; /* je Zielspalte: Quellspalte << 8 | Gewicht */
    int x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
    if (dw < 2 || dh < 2 || alpha <= 0 || !clip_box(dst, &x0, &y0, &x1, &y1))
        return;
    if (x1 - x0 > SCALED_MAXW)
        x1 = x0 + SCALED_MAXW;
    if (r * 2 > dw) r = dw / 2;
    if (r * 2 > dh) r = dh / 2;
    if (alpha > 255) alpha = 255;
    u64 stepx = ((u64)(src->w - 1) << 16) / (u64)(dw - 1), stepy = ((u64)(src->h - 1) << 16) / (u64)(dh - 1);
    int n = x1 - x0, nearest = (u64)dw * (u64)dh >= 1000000;
    for (int i = 0; i < n; i++)
        colx[i] = (u32)(((u64)(x0 + i - dx) * stepx) >> 8);
    int cl = dx + r, cr = dx + dw - r; /* Spalten [cl, cr) liegen in keiner Ecke */
    int il = cl - x0, ir = cr - x0;
    if (il < 0) il = 0;
    if (ir > n) ir = n;
    if (il > ir) il = ir;
    int prev_sy = -1, prev_plain = 0;
    u32 *prev = 0;
    for (int yy = y0; yy < y1; yy++) {
        u64 fy = (u64)(yy - dy) * stepy;
        int sy = (int)(fy >> 16), wy = (int)(fy >> 8 & 0xFF), sy1 = sy + 1 < src->h ? sy + 1 : sy;
        const u32 *r0 = src->px + (u64)sy * (u64)src->w, *r1 = src->px + (u64)sy1 * (u64)src->w;
        u32 *dp = dst->px + (u64)yy * (u64)dst->w + x0;
        int corner = yy < dy + r || yy >= dy + dh - r, plain = !corner && alpha == 255;
        if (nearest && plain && prev_plain && sy == prev_sy) { /* dieselbe Quellzeile wie eben: nur kopieren */
            memcpy(dp, prev, (u64)n * 4);
            prev = dp;
            continue;
        }
        for (int i = 0; i < n; i++) {
            u32 c, cx = colx[i];
            if (nearest) {
                c = r0[cx >> 8];
            } else {
                int sx = (int)(cx >> 8), wx = (int)(cx & 0xFF), sx1 = sx + 1 < src->w ? sx + 1 : sx;
                c = gfx_mix(gfx_mix(r0[sx], r0[sx1], wx), gfx_mix(r1[sx], r1[sx1], wx), wy);
            }
            int a = alpha;
            if (corner && (i < il || i >= ir))
                a = round_cov(x0 + i, yy, dx, dy, dw, dh, r) * alpha / 256;
            if (a >= 255)
                dp[i] = c;
            else if (a > 0)
                dp[i] = gfx_mix(dp[i], c, a);
        }
        prev_sy = sy;
        prev_plain = plain;
        prev = dp;
    }
}
