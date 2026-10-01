/* Desktop: Zusammensetzen auf der GPU (SYS_GPUCOMP, Kernel/drivers/gpu/igd_comp.c).
 * Flaechen, die die GPU lesen oder beschreiben soll (Bildschirmbild, Hintergrund, Fensterbilder, Schatten), liegen in
 * geteiltem Speicher und sind beim Kernel angemeldet (gsurf_new). Je geaendertem Bereich sammelt wm.c Auftraege
 * (kopieren, mischen) und schickt sie mit gq_submit ab. Ohne passende GPU (QEMU) sind es normale Flaechen und die CPU
 * setzt zusammen wie bisher. */

#include "desktop.h"

int gpu_mode;

#define GMAX 96
static struct {
    u32 *px;
    int  handle; /* 0 = nicht angemeldet (dann nur CPU) */
} gs[GMAX];
static u32 *screen_orig; /* Bildschirmbild der Grafikbibliothek (zurueck vor gfx_close) */

int gsurf_new(Surface *s, int w, int h)
{
    s->px = 0;
    if (!gpu_mode)
        return surface_new(s, w, h);
    int slot = -1;
    for (int i = 0; i < GMAX && slot < 0; i++)
        if (!gs[i].px)
            slot = i;
    unsigned id;
    s64 a = slot < 0 ? -1 : sys_shm_create((u64)w * (u64)h * 4, &id);
    if (a < 0)
        return surface_new(s, w, h);
    s64 hnd = sys_gpucomp(1, id, (u64)w | (u64)h << 16);
    gs[slot].px = (u32 *)a;
    gs[slot].handle = hnd > 0 ? (int)hnd : 0;
    s->px = (u32 *)a;
    s->w = w;
    s->h = h;
    return 0;
}

void gsurf_free(Surface *s)
{
    if (!s->px)
        return;
    for (int i = 0; i < GMAX; i++)
        if (gs[i].px == s->px) {
            if (gs[i].handle)
                sys_gpucomp(2, (u64)gs[i].handle, 0);
            sys_shm_unmap(gs[i].px);
            gs[i].px = 0;
            gs[i].handle = 0;
            s->px = 0;
            s->w = s->h = 0;
            return;
        }
    surface_free(s);
}

int gsurf_width(int w)
{
    return gpu_mode ? (w + 15) & ~15 : w;
}

int gsurf_handle(const Surface *s)
{
    for (int i = 0; s->px && i < GMAX; i++)
        if (gs[i].px == s->px)
            return gs[i].handle;
    return 0;
}

int gpu_init(void)
{
    s64 m = sys_gpucomp(0, 0, 0);
    gpu_mode = m > 0 ? (int)m : 0;
    if (!gpu_mode)
        return 0;
    Surface s;
    if (gsurf_new(&s, gfx_screen.w, gfx_screen.h) != 0 || !gsurf_handle(&s)) {
        gsurf_free(&s);
        gpu_mode = 0;
        return 0;
    }
    memcpy(s.px, gfx_screen.px, (u64)s.w * (u64)s.h * 4);
    screen_orig = gfx_screen.px;
    gfx_screen.px = s.px;
    return gpu_mode;
}

void gpu_quit(void)
{
    if (!screen_orig)
        return;
    Surface s = gfx_screen;
    gfx_screen.px = screen_orig;
    screen_orig = 0;
    gsurf_free(&s);
}

/* ---------- Auftraege ---------- */

#define QMAX 1024 /* alle geaenderten Rechtecke eines Bildes */
static GpuOp q[QMAX];
static int   nq, q_bad; /* q_bad: eine Flaeche ist nicht angemeldet oder die Liste ist voll */

static void gq_add(int kind, const Surface *d, int dx, int dy, const Surface *s, int sx, int sy, int w, int h,
                   const Clip *c)
{
    if (dx < c->x0) { sx += c->x0 - dx; w -= c->x0 - dx; dx = c->x0; }
    if (dy < c->y0) { sy += c->y0 - dy; h -= c->y0 - dy; dy = c->y0; }
    if (dx + w > c->x1) w = c->x1 - dx;
    if (dy + h > c->y1) h = c->y1 - dy;
    if (w <= 0 || h <= 0)
        return;
    int hd = gsurf_handle(d), hs = gsurf_handle(s);
    if (!hd || !hs || nq == QMAX) {
        q_bad = 1;
        return;
    }
    q[nq++] = (GpuOp){(unsigned short)kind, (unsigned short)hd, (unsigned short)hs, 0, dx, dy, sx, sy, w, h};
}

void gq_copy(const Surface *d, int dx, int dy, const Surface *s, int sx, int sy, int w, int h, const Clip *c)
{
    gq_add(1, d, dx, dy, s, sx, sy, w, h, c);
}

void gq_blend(const Surface *d, int dx, int dy, const Surface *s, int sx, int sy, int w, int h, const Clip *c)
{
    gq_add(2, d, dx, dy, s, sx, sy, w, h, c);
}

void gq_cancel(void)
{
    nq = q_bad = 0;
}

int gq_submit(void)
{
    int bad = q_bad;
    s64 r = !bad && nq ? sys_gpucomp(3, (u64)q, (u64)nq) : 0;
    nq = q_bad = 0;
    return bad || r < 0 ? -1 : 0;
}

void gpu_stat(int gpu, s64 us, s64 px)
{
    if (gpu_mode && us >= 0)
        sys_gpucomp(4, (u64)(gpu != 0), (u64)us | (u64)px << 32);
}

/* ---------- Schatten als Bild ----------
 * Der Schatten eines Fensters (wie gfx_shadow, Rechteck um shadow_dy nach unten versetzt) liegt in zwei Flaechen:
 * shd_tb = oberer und unterer Streifen (Breite w + 2S, je S + R hoch), shd_lr = linker und rechter Streifen (je S + R
 * breit, h - 2R hoch). Was dazwischen liegt, verdeckt das Fenster. Neu berechnet bei neuer Groesse oder Fokus. */

int shadow_ready(Win *w, int alpha)
{
    if (w->shd_tb.px && w->shd_w == w->w && w->shd_h == w->h && w->shd_a == alpha)
        return gsurf_handle(&w->shd_tb) && gsurf_handle(&w->shd_lr);
    shadow_free(w);
    int S = SHADOW, R = RADIUS, band = S + R, lrh = w->h - 2 * R;
    if (lrh < 1 || w->w < 2 * R)
        return 0;
    if (gsurf_new(&w->shd_tb, gsurf_width(w->w + 2 * S), 2 * band) != 0 ||
        gsurf_new(&w->shd_lr, gsurf_width(2 * band), lrh) != 0) {
        shadow_free(w);
        return 0;
    }
    gfx_shadow_image(&w->shd_tb, 0, 0, w->w + 2 * S, band, S, S, w->w, w->h, R, S, alpha);
    gfx_shadow_image(&w->shd_tb, 0, band, w->w + 2 * S, band, S, S - w->h + 2 * R, w->w, w->h, R, S, alpha);
    gfx_shadow_image(&w->shd_lr, 0, 0, band, lrh, S, -R, w->w, w->h, R, S, alpha);
    gfx_shadow_image(&w->shd_lr, band, 0, band, lrh, S - w->w + 2 * R, -R, w->w, w->h, R, S, alpha);
    w->shd_w = w->w;
    w->shd_h = w->h;
    w->shd_a = alpha;
    return gsurf_handle(&w->shd_tb) && gsurf_handle(&w->shd_lr);
}

void shadow_free(Win *w)
{
    gsurf_free(&w->shd_tb);
    gsurf_free(&w->shd_lr);
    w->shd_w = w->shd_h = w->shd_a = 0;
}
