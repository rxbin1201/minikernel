/* TrueType-Schriften (siehe ttf.h). Jede Glyphe wird fuer eine Groesse einmal gerastert (stb_truetype) und in einem
 * Zwischenspeicher gehalten; gezeichnet wird ihre Deckung (0-255) als Transparenz der Textfarbe. */

#include "ttf.h"
#include "malloc.h"

/* stb_truetype ohne C-Bibliothek: Mathematik und Speicher von hier */
static double ttf_floor(double x) { long long i = (long long)x; return (double)(i - (x < (double)i)); }
static double ttf_ceil(double x) { long long i = (long long)x; return (double)(i + (x > (double)i)); }
static double ttf_fmod(double x, double y) { return y ? x - (double)(long long)(x / y) * y : 0; }
static double ttf_cos(double x) /* nur fuer SDF-Funktionen, die hier nicht benutzt werden */
{
    x = ttf_fmod(x, 6.283185307179586);
    double x2 = x * x;
    return 1 - x2 / 2 + x2 * x2 / 24 - x2 * x2 * x2 / 720;
}
static double ttf_acos(double x) { return 1.5707963267948966 - x - x * x * x / 6; } /* ebenso */
static double ttf_pow(double x, double y) { (void)y; return x; }                     /* ebenso */

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_ifloor(x)   ((int)ttf_floor(x))
#define STBTT_iceil(x)    ((int)ttf_ceil(x))
#define STBTT_sqrt(x)     __builtin_sqrt(x)
#define STBTT_pow(x, y)   ttf_pow(x, y)
#define STBTT_fmod(x, y)  ttf_fmod(x, y)
#define STBTT_cos(x)      ttf_cos(x)
#define STBTT_acos(x)     ttf_acos(x)
#define STBTT_fabs(x)     __builtin_fabs(x)
#define STBTT_malloc(x, u) ((void)(u), u_malloc(x))
#define STBTT_free(x, u)   ((void)(u), u_free(x))
#define STBTT_assert(x)   ((void)0)
#define STBTT_strlen(x)   strlen(x)
#define STBTT_memcpy      memcpy
#define STBTT_memset      memset
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

struct Font {
    stbtt_fontinfo info;
    unsigned char *data;
    int            ascent, descent, gap; /* in Schrifteinheiten */
};

Font *font_ui, *font_bold, *font_mono;

Font *font_load(const char *path)
{
    Stat st;
    if (sys_stat(path, &st) != 0 || st.size < 64 || st.size > (8u << 20))
        return 0;
    s64 fd = sys_open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    Font *f = u_malloc(sizeof(Font));
    unsigned char *d = u_malloc(st.size);
    u64 got = 0;
    s64 r;
    while (f && d && got < st.size && (r = sys_read((int)fd, d + got, st.size - got)) > 0)
        got += (u64)r;
    sys_close((int)fd);
    if (!f || !d || got != st.size || !stbtt_InitFont(&f->info, d, stbtt_GetFontOffsetForIndex(d, 0))) {
        u_free(d);
        u_free(f);
        return 0;
    }
    f->data = d;
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->gap);
    return f;
}

int fonts_init(void)
{
    if (!font_ui)
        font_ui = font_load("/share/fonts/Inter-Regular.ttf");
    if (!font_bold)
        font_bold = font_load("/share/fonts/Inter-SemiBold.ttf");
    if (!font_mono)
        font_mono = font_load("/share/fonts/JetBrainsMono-Regular.ttf");
    if (!font_bold)
        font_bold = font_ui;
    return font_ui && font_mono ? 0 : -1;
}

/* ---------- Glyphen-Zwischenspeicher ---------- */

#define GCACHE 4096

typedef struct {
    Font          *f;
    unsigned       cp;
    int            size;
    short          w, h, xoff, yoff;
    float          adv;
    unsigned char *bm;
} Glyph;

static Glyph         cache[GCACHE];
static unsigned char gamma_tab[256];

static float scale_of(Font *f, int size)
{
    return stbtt_ScaleForMappingEmToPixels(&f->info, (float)size);
}

static Glyph *glyph(Font *f, int size, unsigned cp)
{
    if (!gamma_tab[255]) /* etwas kraeftiger: duenne Striche verschwinden auf hellem Grund sonst */
        for (int i = 0; i < 256; i++)
            gamma_tab[i] = (unsigned char)((i + __builtin_sqrtf((float)i * 255.0f)) * 0.5f + 0.5f);
    unsigned h = (cp * 2654435761u) ^ ((unsigned)size * 40503u) ^ (unsigned)(u64)f;
    Glyph *victim = 0;
    for (int k = 0; k < 8; k++) {
        Glyph *g = &cache[(h + (unsigned)k) & (GCACHE - 1)];
        if (g->f == f && g->cp == cp && g->size == size)
            return g;
        if (!g->f || !victim)
            victim = g;
        if (!g->f)
            break;
    }
    Glyph *g = victim;
    u_free(g->bm);
    float sc = scale_of(f, size);
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetCodepointHMetrics(&f->info, (int)cp, &adv, &lsb);
    stbtt_GetCodepointBitmapBox(&f->info, (int)cp, sc, sc, &x0, &y0, &x1, &y1);
    g->f = f;
    g->cp = cp;
    g->size = size;
    g->adv = (float)adv * sc;
    g->w = (short)(x1 - x0);
    g->h = (short)(y1 - y0);
    g->xoff = (short)x0;
    g->yoff = (short)y0;
    g->bm = 0;
    if (g->w > 0 && g->h > 0 && (g->bm = u_malloc((u64)g->w * (u64)g->h))) {
        stbtt_MakeCodepointBitmap(&f->info, g->bm, g->w, g->h, g->w, sc, sc, (int)cp);
        for (int i = 0; i < g->w * g->h; i++)
            g->bm[i] = gamma_tab[g->bm[i]];
    }
    return g;
}

static void blit_glyph(Surface *s, Glyph *g, int x, int y, u32 color)
{
    for (int j = 0; j < g->h; j++) {
        int yy = y + j;
        if (yy < gfx_clip.y0 || yy >= gfx_clip.y1 || yy < 0 || yy >= s->h)
            continue;
        u32 *row = s->px + (u64)yy * (u64)s->w;
        const unsigned char *src = g->bm + j * g->w;
        for (int i = 0; i < g->w; i++) {
            int xx = x + i;
            if (!src[i] || xx < gfx_clip.x0 || xx >= gfx_clip.x1 || xx < 0 || xx >= s->w)
                continue;
            row[xx] = src[i] == 255 ? color : gfx_mix(row[xx], color, src[i]);
        }
    }
}

int text_ascent(Font *f, int size)
{
    return f ? (int)(f->ascent * scale_of(f, size) + 0.5f) : 12;
}

int text_height(Font *f, int size)
{
    return f ? (int)((f->ascent - f->descent + f->gap) * scale_of(f, size) + 0.5f) : 16;
}

int text_advance(Font *f, int size, unsigned cp)
{
    return f ? (int)(glyph(f, size, cp)->adv + 0.5f) : 8;
}

int text_glyph(Surface *s, Font *f, int size, int x, int y, unsigned cp, u32 color)
{
    if (!f) {
        gfx_char(s, x, y + (size - 16) / 2, cp, color, GFX_TRANSPARENT, 1);
        return 8;
    }
    Glyph *g = glyph(f, size, cp);
    if (g->bm)
        blit_glyph(s, g, x + g->xoff, y + text_ascent(f, size) + g->yoff, color);
    return (int)(g->adv + 0.5f);
}

static int text_run(Surface *s, Font *f, int size, int x, int y, const char *t, u32 color)
{
    if (!f) {
        if (s)
            gfx_text(s, x, y + (text_height(f, size) - 16) / 2, t, color, GFX_TRANSPARENT);
        return gfx_text_width(t);
    }
    float pen = (float)x, sc = scale_of(f, size);
    int base = y + text_ascent(f, size);
    unsigned prev = 0;
    const char *p = t;
    while (*p) {
        unsigned cp = gfx_utf8_next(&p);
        if (prev)
            pen += (float)stbtt_GetCodepointKernAdvance(&f->info, (int)prev, (int)cp) * sc;
        Glyph *g = glyph(f, size, cp);
        if (s && g->bm)
            blit_glyph(s, g, (int)(pen + 0.5f) + g->xoff, base + g->yoff, color);
        pen += g->adv;
        prev = cp;
    }
    return (int)(pen - (float)x + 0.5f);
}

int text_draw(Surface *s, Font *f, int size, int x, int y, const char *t, u32 color)
{
    return text_run(s, f, size, x, y, t, color);
}

int text_width(Font *f, int size, const char *t)
{
    return text_run(0, f, size, 0, 0, t, 0);
}
