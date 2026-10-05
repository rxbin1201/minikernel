#include "gl.h"
#include "gfx.h"
#include "malloc.h"
#include "thread.h"
#include "user.h"

/* Kleines OpenGL (siehe gl.h). Die Bibliothek sammelt Dreiecke mit gleichem Zustand (Matrix, Licht, Textur, Tiefentest)
 * und gibt sie gebuendelt ab: der GPU per SYS_GPUCOMP 8 (Eckpunkte unveraendert, Matrix und Licht rechnet der
 * Vertex-Shader), sonst dem Rasterer hier unten, der dasselbe rechnet wie die GPU (Ecke mal Matrix, Teilen durch W,
 * Farbe und Textur perspektivisch richtig interpoliert, Tiefe "kleiner").
 *
 * Matrizen: Zeile fuer Zeile (m[zeile * 4 + spalte]), wirken auf Spaltenvektoren - wie OpenGL, das sie spaltenweise
 * speichert. Die fertige Matrix ist Bildbereich * Projektion * Modell: sie bringt eine Ecke direkt auf Pixel.
 *
 * Die CPU zeichnet mit mehreren Threads (einer je CPU, hoechstens 4): jeder rastert alle Dreiecke eines Stapels, aber
 * nur jede n-te Bildzeile (Zeile y gehoert Thread y mod n - so bekommt jeder gleich viel von jedem Dreieck). Keiner
 * schreibt dieselben Pixel wie ein anderer, Sperren braucht es nicht. Die Ecken rechnet vorher der aufrufende Thread.
 *
 * Abschneiden (clip_tri): jede Ebene des Sichtraums ist in Objektkoordinaten eine Linearkombination der Zeilen der
 * fertigen Matrix - nahe Ebene Z >= 0, ferne Z <= W, Bildraender X >= 0 ... (X, Y, Z, W = Zeilen mal Ecke). Liegt ein
 * Dreieck ganz ausserhalb einer Ebene, faellt es weg; ragt es ueber die nahe oder ferne Ebene oder weit (GUARD Pixel)
 * ueber den Rand, wird es dort abgeschnitten: die neuen Ecken liegen auf der Kante, alle 12 Werte linear dazwischen
 * (genau, weil X, Y, Z, W linear in der Ecke sind). Das Vieleck geht als Faecher weiter - an die GPU wie an die CPU.
 * Innerhalb des Schutzstreifens zerschneidet die GPU selbst (Rasterizer), ihr Clipper bleibt aus.
 *
 * Rueckseiten (glCullFace): die GPU laesst sie im Rasterizer weg (GPU3D_CULL_*), die CPU nach dem Umrechnen. Vorn ist
 * ein Dreieck, das auf dem Bildschirm gegen den Uhrzeigersinn laeuft (glFrontFace(GL_CW) tauscht vorn und hinten).
 *
 * Mischen (glBlendFunc): Ergebnis = Quelle * Faktor + Ziel * Faktor, die GPU im Blend-State (GPU3D_BLEND mit den
 * Faktor-Codes der Hardware), die CPU in blend_px. Das Fenster hat keinen Alpha-Kanal: Byte 3 bleibt 255 (der Desktop
 * liest es als Deckung), GL_DST_ALPHA ist also immer 1 - wie bei einem Bildschirmformat ohne Alpha. */

#define VF    12                /* float je Eckpunkt: x, y, z, u, v, nx, ny, nz, r, g, b, a */
#define BATCH GPU3D_MAX_VERT
#define PRIM  4096              /* Eckpunkte zwischen glBegin und glEnd */
#define NTEX  64
#define STACK 16
#define PI    3.14159265358979f
#define GUARD 4096.0f           /* Schutzstreifen um das Bild (Pixel), bis dahin rastert die GPU ohne Abschneiden */
#define CLIPV (3 + 6)           /* Ecken eines Dreiecks nach dem Abschneiden an 6 Ebenen */

typedef struct {
    int      used, w, h, pw;    /* Groesse, Zeilenlaenge in Pixeln (Vielfaches von 16: Flaeche fuer die GPU) */
    u32     *px;                /* B, G, R, A je Pixel (0xAARRGGBB) */
    unsigned shm;
    int      surf;              /* angemeldete Flaeche, 0 = keine */
    int      linear;
} Tex;

static Tex tex[NTEX];
static int cpu_only;

static struct {
    int    open, gpu, w, h, top;
    int    dst, depth;          /* Flaechen: Fensterbild, Tiefenpuffer */
    u32   *px;                  /* CPU: Fensterinhalt (Zeilenlaenge w) */
    float *zb;                  /* CPU: Tiefenpuffer */
    GLuint white;
    u32    clear_color;
    float  clear_depth;
    int    depth_test, texturing, lighting;
    int    culling, cull_face, front_cw; /* GL_CULL_FACE, GL_BACK/FRONT/FRONT_AND_BACK, glFrontFace(GL_CW) */
    int    blending, bsrc, bdst;  /* GL_BLEND, Faktoren als GPU3D_BF_* */
    int    depth_mask;            /* glDepthMask: Tiefe schreiben */
    GLuint bound;
    int    mode;
    float  st[2][STACK][16];
    int    sp[2];
    float  light[3], amb, dif;  /* Licht: Richtung im Auge, Umgebung, Streuung */
    float  cur[VF];
    GLenum prim;
    int    pn, bn;
    float  pv[PRIM][VF];
    float  bv[BATCH][VF];
    struct {                    /* Zustand der gesammelten Dreiecke */
        float  m[16], l[3], amb, dif;
        GLuint tex;
        int    depth;
        int    cull;            /* GPU3D_CULL_* (vorn = auf dem Bildschirm gegen den Uhrzeigersinn) */
        int    blend;           /* 0 oder Faktor Quelle | Faktor Ziel << 8 (GPU3D_BF_*) */
        int    zwrite;          /* Tiefe schreiben (mit Tiefentest) */
    } bs;
} G;

/* ---------- Rechnen ---------- */

float gl_sqrtf(float x)
{
    return x > 0 ? __builtin_sqrtf(x) : 0;
}

float gl_sinf(float x)
{
    float k = x * (1.0f / (2 * PI));
    k = (float)(int)(k + (k >= 0 ? 0.5f : -0.5f));
    x -= k * 2 * PI;
    if (x > PI / 2)
        x = PI - x;
    else if (x < -PI / 2)
        x = -PI - x;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}

float gl_cosf(float x)
{
    return gl_sinf(x + PI / 2);
}

static int floori(float f)
{
    int i = (int)f;
    return f < (float)i ? i - 1 : i;
}

static float sat(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static void m_ident(float *m)
{
    for (int i = 0; i < 16; i++)
        m[i] = i % 5 == 0;
}

static void m_mul(float *o, const float *a, const float *b)
{
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    memcpy(o, t, sizeof(t));
}

static float *top(int mode)
{
    return G.st[mode][G.sp[mode]];
}

static void mult(const float *b)
{
    float *m = top(G.mode);
    m_mul(m, m, b);
}

/* ---------- Flaechen und Texturen ---------- */

static void tex_free(Tex *t)
{
    if (t->surf)
        sys_gpucomp(2, (u64)t->surf, 0);
    if (t->px)
        sys_shm_unmap(t->px);
    t->surf = 0;
    t->px = 0;
}

static void gpu_off(const char *why);

/* Bild in die Textur: geteilter Speicher (Zeilen auf 16 Pixel aufgerundet), fuer die GPU angemeldet */
static void tex_image(Tex *t, int w, int h, GLenum format, const void *pixels)
{
    tex_free(t);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return;
    int pw = (w + 15) & ~15;
    unsigned id;
    s64 a = sys_shm_create((u64)pw * (u64)h * 4, &id);
    if (a < 0)
        return;
    t->px = (u32 *)a;
    t->shm = id;
    t->w = w;
    t->h = h;
    t->pw = pw;
    const unsigned char *s = pixels;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < pw; x++) {
            const unsigned char *p = s ? s + ((u64)y * (u64)w + (u64)(x < w ? x : w - 1)) * 4 : 0;
            u32 v = 0xFFFFFFFFu;
            if (p && format == GL_RGBA)
                v = (u32)p[3] << 24 | (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
            else if (p)
                v = (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
            t->px[(u64)y * (u64)pw + (u64)x] = v;
        }
    if (G.gpu) {
        s64 r = sys_gpucomp(1, id, (u64)pw | (u64)h << 16);
        if (r > 0)
            t->surf = (int)r;
        else
            gpu_off("Textur laesst sich nicht anmelden");
    }
}

static GLuint tex_new(void)
{
    for (GLuint i = 1; i < NTEX; i++)
        if (!tex[i].used) {
            memset(&tex[i], 0, sizeof(tex[i]));
            tex[i].used = 1;
            return i;
        }
    return 0;
}

/* GPU geht nicht (mehr): ab jetzt zeichnet die CPU */
static void gpu_off(const char *why)
{
    if (!G.gpu)
        return;
    fprintf(2, "gl: %s - ab jetzt zeichnet die CPU\n", why);
    G.gpu = 0;
    if (!G.zb)
        G.zb = u_malloc((u64)G.w * (u64)G.h * 4);
    for (int i = 0; G.zb && i < G.w * G.h; i++)
        G.zb[i] = 1.0f;
}

/* ---------- Rasterer der CPU ---------- */

static void texel(const Tex *t, int x, int y, float *c)
{
    x %= t->w;
    y %= t->h;
    if (x < 0)
        x += t->w;
    if (y < 0)
        y += t->h;
    u32 p = t->px[(u64)y * (u64)t->pw + (u64)x];
    c[0] = (float)(p >> 16 & 0xFF) * (1.0f / 255);
    c[1] = (float)(p >> 8 & 0xFF) * (1.0f / 255);
    c[2] = (float)(p & 0xFF) * (1.0f / 255);
    c[3] = (float)(p >> 24) * (1.0f / 255);
}

static void sample(const Tex *t, float u, float v, float *c)
{
    if (!t->linear) {
        texel(t, floori(u * (float)t->w), floori(v * (float)t->h), c);
        return;
    }
    float fx = u * (float)t->w - 0.5f, fy = v * (float)t->h - 0.5f;
    int x0 = floori(fx), y0 = floori(fy);
    float ax = fx - (float)x0, ay = fy - (float)y0, q[4][4];
    texel(t, x0, y0, q[0]);
    texel(t, x0 + 1, y0, q[1]);
    texel(t, x0, y0 + 1, q[2]);
    texel(t, x0 + 1, y0 + 1, q[3]);
    for (int i = 0; i < 4; i++)
        c[i] = (q[0][i] * (1 - ax) + q[1][i] * ax) * (1 - ay) + (q[2][i] * (1 - ax) + q[3][i] * ax) * ay;
}

static float bfactor(int f, const float *s, const float *d, int c)
{
    switch (f) {
    case GPU3D_BF_ONE: return 1;
    case GPU3D_BF_SRC_COLOR: return s[c];
    case GPU3D_BF_SRC_ALPHA: return s[3];
    case GPU3D_BF_DST_ALPHA: return d[3];
    case GPU3D_BF_DST_COLOR: return d[c];
    case GPU3D_BF_SRC_ALPHA_SAT: return c == 3 ? 1 : s[3] < 1 - d[3] ? s[3] : 1 - d[3];
    case GPU3D_BF_INV_SRC_COLOR: return 1 - s[c];
    case GPU3D_BF_INV_SRC_ALPHA: return 1 - s[3];
    case GPU3D_BF_INV_DST_ALPHA: return 1 - d[3];
    case GPU3D_BF_INV_DST_COLOR: return 1 - d[c];
    default: return 0;
    }
}

/* Quelle s (r, g, b, a, schon 0-1) mit dem Pixel *p mischen; Alpha im Ziel bleibt 255 */
static void blend_px(u32 *p, const float *s)
{
    u32 o = *p;
    float d[4] = {(float)(o >> 16 & 0xFF) * (1.0f / 255), (float)(o >> 8 & 0xFF) * (1.0f / 255),
                  (float)(o & 0xFF) * (1.0f / 255), 1};
    int fs = G.bs.blend & 0xFF, fd = G.bs.blend >> 8;
    u32 r = 0xFF000000u;
    for (int c = 0; c < 3; c++)
        r |= (u32)(sat(s[c] * bfactor(fs, s, d, c) + d[c] * bfactor(fd, s, d, c)) * 255 + 0.5f) << (16 - 8 * c);
    *p = r;
}

/* s[k]: x, y, z (Bildschirm), 1/W, dann u, v, r, g, b, a jeweils mal 1/W. Nur die Zeilen y mit y % step == first. */
static void raster(float s[3][10], const Tex *t, int first, int step)
{
    float x0 = s[0][0], y0 = s[0][1], x1 = s[1][0], y1 = s[1][1], x2 = s[2][0], y2 = s[2][1];
    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == 0)
        return;
    float inv = 1.0f / area;
    float lo_x = x0 < x1 ? x0 : x1, hi_x = x0 > x1 ? x0 : x1, lo_y = y0 < y1 ? y0 : y1, hi_y = y0 > y1 ? y0 : y1;
    lo_x = x2 < lo_x ? x2 : lo_x;
    hi_x = x2 > hi_x ? x2 : hi_x;
    lo_y = y2 < lo_y ? y2 : lo_y;
    hi_y = y2 > hi_y ? y2 : hi_y;
    if (hi_x < 0 || hi_y < 0 || lo_x > (float)G.w || lo_y > (float)G.h)
        return;
    int ax = lo_x < 0 ? 0 : floori(lo_x), bx = hi_x > (float)(G.w - 1) ? G.w - 1 : floori(hi_x);
    int ay = lo_y < 0 ? 0 : floori(lo_y), by = hi_y > (float)(G.h - 1) ? G.h - 1 : floori(hi_y);
    ay += ((first - ay) % step + step) % step; /* erste eigene Zeile */
    for (int y = ay; y <= by; y += step) {
        float py = (float)y + 0.5f;
        for (int x = ax; x <= bx; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)) * inv;
            float w1 = ((x0 - x2) * (py - y2) - (y0 - y2) * (px - x2)) * inv;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            u64 i = (u64)y * (u64)G.w + (u64)x;
            float z = w0 * s[0][2] + w1 * s[1][2] + w2 * s[2][2];
            if (G.bs.depth) {
                if (!(z < G.zb[i]))
                    continue;
                if (G.bs.zwrite)
                    G.zb[i] = z;
            }
            float iw = 1.0f / (w0 * s[0][3] + w1 * s[1][3] + w2 * s[2][3]), a[6], c[4];
            for (int k = 0; k < 6; k++)
                a[k] = (w0 * s[0][4 + k] + w1 * s[1][4 + k] + w2 * s[2][4 + k]) * iw;
            sample(t, a[0], a[1], c);
            float f[4] = {sat(a[2] * c[0]), sat(a[3] * c[1]), sat(a[4] * c[2]), sat(a[5] * c[3])};
            if (G.bs.blend) {
                blend_px(&G.px[i], f);
                continue;
            }
            G.px[i] = 0xFF000000u | (u32)(f[0] * 255 + 0.5f) << 16 | (u32)(f[1] * 255 + 0.5f) << 8 |
                      (u32)(f[2] * 255 + 0.5f);
        }
    }
}

/* ---------- Mehrere Threads ---------- */

#define MAX_WORKERS 4

static float      tris[BATCH / 3][3][10]; /* fertig umgerechnete Dreiecke des Stapels */
static int        ntris;
static const Tex *job_tex;
static int        job_clear;              /* 0 = Dreiecke rastern, sonst Loeschen: Bit 0 Farbe, Bit 1 Tiefe */
static int        nbands = 1;             /* Streifen = Threads (der aufrufende eingeschlossen) */
static volatile u32 job_gen, job_left;    /* Futex: neuer Auftrag, Streifen noch in Arbeit */
static int        workers_tried;

static void band_rows(int b, int *y0, int *y1)
{
    *y0 = G.h * b / nbands;
    *y1 = G.h * (b + 1) / nbands;
}

static void do_band(int b)
{
    int y0, y1;
    band_rows(b, &y0, &y1);
    if (job_clear) {
        for (u64 i = (u64)y0 * (u64)G.w; i < (u64)y1 * (u64)G.w; i++) {
            if (job_clear & 1)
                G.px[i] = G.clear_color;
            if (job_clear & 2)
                G.zb[i] = G.clear_depth;
        }
        return;
    }
    for (int i = 0; i < ntris; i++)
        raster(tris[i], job_tex, b, nbands);
}

static void *worker(void *arg)
{
    int b = (int)(u64)arg;
    u32 seen = 0;
    for (;;) {
        u32 g;
        while ((g = __atomic_load_n(&job_gen, __ATOMIC_ACQUIRE)) == seen)
            sys_futex_wait(&job_gen, seen, 0);
        seen = g;
        do_band(b);
        if (__atomic_sub_fetch(&job_left, 1, __ATOMIC_ACQ_REL) == 0)
            sys_futex_wake(&job_left, 1);
    }
    return 0;
}

/* Beim ersten Zeichnen auf der CPU: je weitere CPU ein Thread (bis 4 Streifen) */
static void start_workers(void)
{
    workers_tried = 1;
    CpuInfo ci;
    int cpus = 0;
    while (cpus < 16 && sys_cpuinfo((u64)cpus, &ci) == 0)
        cpus++;
    int want = cpus < MAX_WORKERS ? cpus : MAX_WORKERS;
    int n = 1;
    while (n < want && thread_create(worker, (void *)(u64)n) > 0)
        n++;
    nbands = n;
}

/* Auftrag auf alle Streifen verteilen; der aufrufende Thread nimmt Streifen 0 und wartet auf die anderen */
static void run_job(void)
{
    if (!workers_tried)
        start_workers();
    if (nbands > 1) {
        __atomic_store_n(&job_left, (u32)(nbands - 1), __ATOMIC_RELEASE);
        __atomic_add_fetch(&job_gen, 1, __ATOMIC_ACQ_REL);
        sys_futex_wake(&job_gen, (u32)(nbands - 1));
    }
    do_band(0);
    u32 left;
    while ((left = __atomic_load_n(&job_left, __ATOMIC_ACQUIRE)) != 0 && nbands > 1)
        sys_futex_wait(&job_left, left, 0);
}

/* Ecke umrechnen: P = X, Y, Z, W (Zeilen der Matrix mal Ecke); s = x, y, z auf dem Bildschirm, 1/W, dann u, v,
 * r, g, b, a jeweils mal 1/W (Farbe mal Helligkeit). 0 = hinter der Kamera (W zu klein, s ungueltig) */
static int project(const float *p, float *s, float *P)
{
    const float *m = G.bs.m;
    for (int r = 0; r < 4; r++)
        P[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
    if (P[3] <= 1e-6f)
        return 0;
    float iw = 1.0f / P[3];
    float br = G.bs.amb + G.bs.dif * sat(p[5] * G.bs.l[0] + p[6] * G.bs.l[1] + p[7] * G.bs.l[2]);
    s[0] = P[0] * iw;
    s[1] = P[1] * iw;
    s[2] = P[2] * iw;
    s[3] = iw;
    s[4] = p[3] * iw;
    s[5] = p[4] * iw;
    for (int c = 0; c < 3; c++)
        s[6 + c] = p[8 + c] * br * iw;
    s[9] = p[11] * iw;
    return 1;
}

/* gesammelte Dreiecke der CPU rastern */
static void tris_run(void)
{
    if (!ntris)
        return;
    job_tex = &tex[G.bs.tex];
    job_clear = 0;
    run_job();
    ntris = 0;
}

/* umgerechnetes Dreieck sammeln (Rueckseiten weg); ist der Stapel voll, wird gerastert */
static void tris_add(const float *a, const float *b, const float *c)
{
    if (G.bs.cull) { /* Flaeche > 0: auf dem Bildschirm (y nach unten) im Uhrzeigersinn = Rueckseite */
        float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        if (G.bs.cull & (area > 0 ? GPU3D_CULL_BACK : GPU3D_CULL_FRONT))
            return;
    }
    memcpy(tris[ntris][0], a, sizeof(tris[0][0]));
    memcpy(tris[ntris][1], b, sizeof(tris[0][0]));
    memcpy(tris[ntris][2], c, sizeof(tris[0][0]));
    if (++ntris == BATCH / 3)
        tris_run();
}

/* Dreiecke in Objektkoordinaten (abgeschnitten) umrechnen und rastern */
static void emit_cpu(const float *a, const float *b, const float *c)
{
    float s[3][10], P[4];
    if (project(a, s[0], P) && project(b, s[1], P) && project(c, s[2], P))
        tris_add(s[0], s[1], s[2]);
}

static void cpu_draw(void)
{
    for (int i = 0; i + 2 < G.bn; i += 3)
        emit_cpu(G.bv[i], G.bv[i + 1], G.bv[i + 2]);
    tris_run();
}

/* ---------- Abgeben ---------- */

static void base_draw(Gpu3dDraw *d)
{
    memset(d, 0, sizeof(*d));
    d->dst = (unsigned short)G.dst;
    d->depth = (unsigned short)G.depth;
    d->x = 0;
    d->y = G.top;
    d->w = G.w;
    d->h = G.h;
}

/* Auftrag mit dem Zustand der gesammelten Dreiecke (Ziel, Textur, Tests, Matrix, Licht) */
static void state_draw(Gpu3dDraw *d)
{
    base_draw(d);
    const Tex *t = &tex[G.bs.tex];
    d->tex = (unsigned short)t->surf;
    d->tex_w = (unsigned short)t->w;
    d->tex_h = (unsigned short)t->h;
    d->flags = (unsigned short)((G.bs.depth ? GPU3D_DEPTH : 0) | (t->linear ? GPU3D_LINEAR : 0) | G.bs.cull |
                                (G.bs.blend ? GPU3D_BLEND : 0) | (G.bs.depth && !G.bs.zwrite ? GPU3D_NO_DEPTH_WRITE : 0) |
                                GPU3D_KEEP_ALPHA);
    d->blend = (unsigned short)G.bs.blend;
    memcpy(d->m, G.bs.m, sizeof(d->m));
    memcpy(d->light, G.bs.l, sizeof(d->light));
    d->ambient = G.bs.amb;
    d->diffuse = G.bs.dif;
}

static void flush(void)
{
    if (!G.bn)
        return;
    if (G.gpu) {
        Gpu3dDraw d;
        state_draw(&d);
        d.nvert = (unsigned)G.bn;
        d.verts = &G.bv[0][0];
        s64 r = sys_gpu3d(&d);
        if (r >= 0) {
            G.bn = 0;
            return;
        }
        gpu_off(r == -38 ? "keine 3D-Pipeline" : "die GPU lehnt den Auftrag ab");
    }
    cpu_draw();
    G.bn = 0;
}

/* Zustand fuer die naechsten Dreiecke: fertige Matrix, Licht im Modell, Textur, Tiefentest */
static void cur_state(void)
{
    float v[16] = {(float)G.w * 0.5f, 0, 0, (float)G.w * 0.5f, 0, -(float)G.h * 0.5f, 0, (float)G.h * 0.5f,
                   0, 0, 0.5f, 0.5f, 0, 0, 0, 1};
    float t[16], m[16], l[3] = {0, 0, 0}, amb = 1, dif = 0;
    m_mul(t, v, top(1));
    m_mul(m, t, top(0));
    if (G.lighting) { /* Richtung ins Modell: transponierte Modellmatrix (Drehung, gleichmaessige Skalierung) */
        const float *mv = top(0);
        for (int i = 0; i < 3; i++)
            l[i] = mv[i] * G.light[0] + mv[4 + i] * G.light[1] + mv[8 + i] * G.light[2];
        float n = gl_sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        for (int i = 0; n > 0 && i < 3; i++)
            l[i] /= n;
        amb = G.amb;
        dif = G.dif;
    }
    GLuint tid = G.texturing && G.bound && tex[G.bound].px ? G.bound : G.white;
    int depth = G.depth_test != 0, cull = 0;
    if (G.culling) {
        int back = G.cull_face != GL_FRONT, front = G.cull_face != GL_BACK;
        if (G.front_cw) { /* vorn im Uhrzeigersinn: was auf dem Bildschirm hinten waere, ist vorn */
            int t = back;
            back = front;
            front = t;
        }
        cull = (back ? GPU3D_CULL_BACK : 0) | (front ? GPU3D_CULL_FRONT : 0);
    }
    int blend = G.blending ? G.bsrc | G.bdst << 8 : 0, zwrite = G.depth_mask;
    if (blend == (GPU3D_BF_ONE | GPU3D_BF_ZERO << 8))
        blend = 0; /* Quelle * 1 + Ziel * 0: wie ohne Mischen */
    if (G.bn && (memcmp(m, G.bs.m, sizeof(m)) || memcmp(l, G.bs.l, sizeof(l)) || amb != G.bs.amb ||
                 dif != G.bs.dif || tid != G.bs.tex || depth != G.bs.depth || cull != G.bs.cull ||
                 blend != G.bs.blend || zwrite != G.bs.zwrite))
        flush();
    memcpy(G.bs.m, m, sizeof(m));
    memcpy(G.bs.l, l, sizeof(l));
    G.bs.amb = amb;
    G.bs.dif = dif;
    G.bs.tex = tid;
    G.bs.depth = depth;
    G.bs.cull = cull;
    G.bs.blend = blend;
    G.bs.zwrite = zwrite;
}

static void emit(const float *a, const float *b, const float *c)
{
    if (G.bn + 3 > BATCH)
        flush();
    memcpy(G.bv[G.bn++], a, sizeof(G.pv[0]));
    memcpy(G.bv[G.bn++], b, sizeof(G.pv[0]));
    memcpy(G.bv[G.bn++], c, sizeof(G.pv[0]));
}

/* wohin abgeschnittene Dreiecke gehen: in den Stapel (emit) oder bei glDrawArrays/-Elements auf der CPU gleich
 * umgerechnet zum Rastern (emit_cpu) */
static void (*sink)(const float *, const float *, const float *) = emit;

/* Abstaende einer Ecke zu den Ebenen: 0 nah, 1 fern, 2-5 Bildraender (links, rechts, oben, unten), 6-9 dieselben mit
 * Schutzstreifen. >= 0 = innen */
#define NPLANE 10
static void dist_from(const float *P, float *d)
{
    float w = (float)G.w, h = (float)G.h;
    d[0] = P[2];
    d[1] = P[3] - P[2];
    d[2] = P[0];
    d[3] = w * P[3] - P[0];
    d[4] = P[1];
    d[5] = h * P[3] - P[1];
    for (int i = 0; i < 4; i++)
        d[6 + i] = d[2 + i] + GUARD * P[3];
}

static void plane_dist(const float *p, float *d)
{
    const float *m = G.bs.m;
    float P[4];
    for (int r = 0; r < 4; r++)
        P[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
    dist_from(P, d);
}

/* Bit e: ausserhalb von Ebene e. Bits 0-5 (nah, fern, Bildraender): alle drei Ecken draussen = Dreieck weg;
 * Bits 0, 1, 6-9 (nah, fern, Schutzstreifen): abschneiden */
#define OUT_REJECT 0x03F
#define OUT_CLIP   0x3C3
static unsigned outcode(const float *P)
{
    float d[NPLANE];
    dist_from(P, d);
    unsigned o = 0;
    for (int e = 0; e < NPLANE; e++)
        o |= (unsigned)(d[e] < 0) << e;
    return o;
}

/* Vieleck v[0..n) an Ebene k abschneiden (Sutherland-Hodgman), Ergebnis nach o; Anzahl der Ecken */
static int clip_plane(float (*v)[VF], float (*dv)[NPLANE], int n, int k, float (*o)[VF], float (*dout)[NPLANE])
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        int j = i + 1 < n ? i + 1 : 0;
        float di = dv[i][k], dj = dv[j][k];
        if (di >= 0) {
            memcpy(o[m], v[i], sizeof(o[0]));
            memcpy(dout[m++], dv[i], sizeof(dout[0]));
        }
        if ((di >= 0) != (dj >= 0)) { /* Kante schneidet die Ebene: neue Ecke dort */
            float t = di / (di - dj);
            for (int c = 0; c < VF; c++)
                o[m][c] = v[i][c] + (v[j][c] - v[i][c]) * t;
            for (int c = 0; c < NPLANE; c++)
                dout[m][c] = dv[i][c] + (dv[j][c] - dv[i][c]) * t;
            dout[m][k] = 0;
            m++;
        }
    }
    return m;
}

static void tri_v(const float *p0, const float *p1, const float *p2)
{
    static float v[2][CLIPV][VF], dv[2][CLIPV][NPLANE];
    const float *p[3] = {p0, p1, p2};
    int clip = 0;
    for (int k = 0; k < 3; k++)
        plane_dist(p[k], dv[0][k]);
    for (int e = 0; e < NPLANE; e++) {
        int out = (dv[0][0][e] < 0) + (dv[0][1][e] < 0) + (dv[0][2][e] < 0);
        if (out == 3 && e < 6)
            return;                     /* ganz ausserhalb: weg */
        if (out && (e < 2 || e >= 6))
            clip |= 1 << e;             /* ragt ueber nah, fern oder den Schutzstreifen */
    }
    if (!clip) {
        sink(p[0], p[1], p[2]);
        return;
    }
    for (int k = 0; k < 3; k++)
        memcpy(v[0][k], p[k], sizeof(v[0][0]));
    int n = 3, cur = 0;
    for (int e = 0; e < NPLANE && n >= 3; e++)
        if (clip & (1 << e)) {
            n = clip_plane(v[cur], dv[cur], n, e, v[cur ^ 1], dv[cur ^ 1]);
            cur ^= 1;
        }
    for (int i = 2; i < n; i++)
        sink(v[cur][0], v[cur][i - 1], v[cur][i]);
}

static void tri(int a, int b, int c)
{
    tri_v(G.pv[a], G.pv[b], G.pv[c]);
}

/* Dreiecke einer Grundform aus n Ecken: t(a, b, c) mit den Nummern der Ecken (Streifen abwechselnd gedreht, damit
 * alle denselben Umlaufsinn haben) */
static void prims(GLenum mode, int n, void (*t)(int, int, int))
{
    switch (mode) {
    case GL_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3)
            t(i, i + 1, i + 2);
        break;
    case GL_TRIANGLE_STRIP:
        for (int i = 2; i < n; i++)
            if (i & 1)
                t(i - 1, i - 2, i);
            else
                t(i - 2, i - 1, i);
        break;
    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        for (int i = 2; i < n; i++)
            t(0, i - 1, i);
        break;
    case GL_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            t(i, i + 1, i + 2);
            t(i, i + 2, i + 3);
        }
        break;
    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < n; i += 2) {
            t(i, i + 1, i + 3);
            t(i, i + 3, i + 2);
        }
        break;
    }
}

/* ---------- Fenster ---------- */

void gl_force_cpu(int on)
{
    cpu_only = on;
}

int gl_gpu(void)
{
    return G.gpu;
}

int gl_width(void)
{
    return G.w;
}

int gl_height(void)
{
    return G.h;
}

int gl_open(int w, int h, const char *title)
{
    if (G.open)
        return 0;
    if (gfx_open_window((w + 15) & ~15, h, title) != 0)
        return -1;
    G.w = gfx_screen.w;
    G.h = gfx_screen.h;
    G.px = gfx_screen.px;
    for (int i = 0; i < 2; i++)
        m_ident(top(i));
    G.light[2] = 1;
    G.cull_face = GL_BACK;
    G.bsrc = GPU3D_BF_ONE;
    G.bdst = GPU3D_BF_ZERO;
    G.depth_mask = 1;
    G.amb = 0.25f;
    G.dif = 0.75f;
    G.clear_depth = 1;
    G.clear_color = 0xFF000000u;
    G.cur[7] = 1;
    for (int i = 8; i < 12; i++)
        G.cur[i] = 1;
    unsigned shm;
    int bw, bh, top_row;
    if (!cpu_only && gfx_window_buffer(&shm, &bw, &bh, &top_row) == 0 && !(bw & 15) && sys_gpucomp(0, 0, 0) == 1) {
        s64 d = sys_gpucomp(1, shm, (u64)bw | (u64)bh << 16);
        int zw = (G.w + 31) & ~31, zh = (G.h + 31) & ~31;
        unsigned zid;
        s64 za = sys_shm_create((u64)zw * (u64)zh * 4, &zid);
        s64 z = za >= 0 ? sys_gpucomp(1, zid, (u64)zw | (u64)zh << 16) : -1;
        if (d > 0 && z > 0) {
            G.gpu = 1;
            G.dst = (int)d;
            G.depth = (int)z;
            G.top = top_row;
        } else if (d > 0) {
            sys_gpucomp(2, (u64)d, 0);
        }
    }
    if (!G.gpu) {
        G.zb = u_malloc((u64)G.w * (u64)G.h * 4);
        if (!G.zb) {
            gfx_close();
            return -1;
        }
        for (int i = 0; i < G.w * G.h; i++)
            G.zb[i] = 1.0f;
    }
    G.white = tex_new();
    tex_image(&tex[G.white], 16, 1, GL_BGRA, 0);
    G.open = 1;
    return 0;
}

static void bufs_free(void);

void gl_close(void)
{
    if (!G.open)
        return;
    bufs_free();
    for (int i = 1; i < NTEX; i++)
        if (tex[i].used) {
            tex_free(&tex[i]);
            tex[i].used = 0;
        }
    if (G.dst)
        sys_gpucomp(2, (u64)G.dst, 0);
    if (G.depth)
        sys_gpucomp(2, (u64)G.depth, 0);
    G.open = 0;
    gfx_close();
}

void gl_swap(void)
{
    flush();
    gfx_present_all();
    gfx_vsync();
}

/* ---------- OpenGL ---------- */

void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    (void)a; /* kein Alpha-Kanal im Fenster: bleibt deckend */
    G.clear_color = 0xFF000000u | (u32)(sat(r) * 255 + 0.5f) << 16 | (u32)(sat(g) * 255 + 0.5f) << 8 |
                    (u32)(sat(b) * 255 + 0.5f);
}

void glClearDepth(GLdouble d)
{
    G.clear_depth = sat((float)d);
}

void glClear(GLbitfield mask)
{
    flush();
    int c = (mask & GL_COLOR_BUFFER_BIT) != 0, z = (mask & GL_DEPTH_BUFFER_BIT) != 0;
    if (!c && !z)
        return;
    if (G.gpu) {
        Gpu3dDraw d;
        base_draw(&d);
        d.tex = (unsigned short)tex[G.white].surf;
        d.flags = (unsigned short)((c ? GPU3D_CLEAR_COLOR : 0) | (z ? GPU3D_CLEAR_DEPTH : 0));
        d.clear_color = G.clear_color;
        d.clear_depth = G.clear_depth;
        s64 r = sys_gpu3d(&d);
        if (r >= 0)
            return;
        gpu_off(r == -38 ? "keine 3D-Pipeline" : "die GPU lehnt das Loeschen ab");
    }
    job_clear = (c ? 1 : 0) | (z ? 2 : 0);
    run_job();
}

void glEnable(GLenum cap)
{
    if (cap == GL_DEPTH_TEST)
        G.depth_test = 1;
    else if (cap == GL_TEXTURE_2D)
        G.texturing = 1;
    else if (cap == GL_LIGHTING)
        G.lighting = 1;
    else if (cap == GL_CULL_FACE)
        G.culling = 1;
    else if (cap == GL_BLEND)
        G.blending = 1;
}

void glDisable(GLenum cap)
{
    if (cap == GL_DEPTH_TEST)
        G.depth_test = 0;
    else if (cap == GL_TEXTURE_2D)
        G.texturing = 0;
    else if (cap == GL_LIGHTING)
        G.lighting = 0;
    else if (cap == GL_CULL_FACE)
        G.culling = 0;
    else if (cap == GL_BLEND)
        G.blending = 0;
}

static int blend_code(GLenum f)
{
    switch (f) {
    case GL_ZERO: return GPU3D_BF_ZERO;
    case GL_ONE: return GPU3D_BF_ONE;
    case GL_SRC_COLOR: return GPU3D_BF_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return GPU3D_BF_INV_SRC_COLOR;
    case GL_SRC_ALPHA: return GPU3D_BF_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return GPU3D_BF_INV_SRC_ALPHA;
    case GL_DST_ALPHA: return GPU3D_BF_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return GPU3D_BF_INV_DST_ALPHA;
    case GL_DST_COLOR: return GPU3D_BF_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return GPU3D_BF_INV_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE: return GPU3D_BF_SRC_ALPHA_SAT;
    default: return 0;
    }
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    int s = blend_code(sfactor), d = blend_code(dfactor);
    if (!s || !d || d == GPU3D_BF_SRC_ALPHA_SAT)
        return; /* GL_INVALID_ENUM: Zustand bleibt */
    G.bsrc = s;
    G.bdst = d;
}

void glDepthMask(GLboolean flag)
{
    G.depth_mask = flag != 0;
}

void glCullFace(GLenum mode)
{
    if (mode == GL_FRONT || mode == GL_BACK || mode == GL_FRONT_AND_BACK)
        G.cull_face = (int)mode;
}

void glFrontFace(GLenum mode)
{
    if (mode == GL_CW || mode == GL_CCW)
        G.front_cw = mode == GL_CW;
}

void glFlush(void)
{
    flush();
}

void glFinish(void)
{
    flush();
}

void glMatrixMode(GLenum mode)
{
    G.mode = mode == GL_PROJECTION ? 1 : 0;
}

void glLoadIdentity(void)
{
    m_ident(top(G.mode));
}

void glLoadMatrixf(const GLfloat *m)
{
    float *t = top(G.mode);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t[r * 4 + c] = m[c * 4 + r];
}

void glMultMatrixf(const GLfloat *m)
{
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t[r * 4 + c] = m[c * 4 + r];
    mult(t);
}

void glPushMatrix(void)
{
    int *sp = &G.sp[G.mode];
    if (*sp + 1 < STACK) {
        memcpy(G.st[G.mode][*sp + 1], G.st[G.mode][*sp], sizeof(G.st[0][0]));
        (*sp)++;
    }
}

void glPopMatrix(void)
{
    if (G.sp[G.mode] > 0)
        G.sp[G.mode]--;
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    float t[16] = {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
    mult(t);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    float t[16] = {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1};
    mult(t);
}

void glRotatef(GLfloat deg, GLfloat x, GLfloat y, GLfloat z)
{
    float n = gl_sqrtf(x * x + y * y + z * z);
    if (n <= 0)
        return;
    x /= n;
    y /= n;
    z /= n;
    float a = deg * (PI / 180), c = gl_cosf(a), s = gl_sinf(a), k = 1 - c;
    float t[16] = {x * x * k + c,     x * y * k - z * s, x * z * k + y * s, 0,
                   y * x * k + z * s, y * y * k + c,     y * z * k - x * s, 0,
                   x * z * k - y * s, y * z * k + x * s, z * z * k + c,     0,
                   0,                 0,                 0,                 1};
    mult(t);
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16] = {(float)(2 * n / (r - l)), 0, (float)((r + l) / (r - l)), 0,
                   0, (float)(2 * n / (t - b)), (float)((t + b) / (t - b)), 0,
                   0, 0, (float)(-(f + n) / (f - n)), (float)(-2 * f * n / (f - n)),
                   0, 0, -1, 0};
    mult(m);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16] = {(float)(2 / (r - l)), 0, 0, (float)(-(r + l) / (r - l)),
                   0, (float)(2 / (t - b)), 0, (float)(-(t + b) / (t - b)),
                   0, 0, (float)(-2 / (f - n)), (float)(-(f + n) / (f - n)),
                   0, 0, 0, 1};
    mult(m);
}

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble n, GLdouble f)
{
    float h = (float)fovy * (PI / 360), c = gl_cosf(h) / gl_sinf(h);
    float m[16] = {c / (float)aspect, 0, 0, 0,
                   0, c, 0, 0,
                   0, 0, (float)((f + n) / (n - f)), (float)(2 * f * n / (n - f)),
                   0, 0, -1, 0};
    mult(m);
}

void glBegin(GLenum mode)
{
    G.prim = mode;
    G.pn = 0;
}

void glEnd(void)
{
    cur_state();
    prims(G.prim, G.pn, tri);
    G.pn = 0;
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    if (G.pn >= PRIM)
        return;
    float *p = G.pv[G.pn++];
    memcpy(p, G.cur, sizeof(G.cur));
    p[0] = x;
    p[1] = y;
    p[2] = z;
}

void glVertex2f(GLfloat x, GLfloat y)
{
    glVertex3f(x, y, 0);
}

void glVertex3fv(const GLfloat *v)
{
    glVertex3f(v[0], v[1], v[2]);
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    G.cur[8] = r;
    G.cur[9] = g;
    G.cur[10] = b;
    G.cur[11] = a;
}

void glColor3f(GLfloat r, GLfloat g, GLfloat b)
{
    glColor4f(r, g, b, 1);
}

void glColor3ub(GLubyte r, GLubyte g, GLubyte b)
{
    glColor4f((float)r / 255, (float)g / 255, (float)b / 255, 1);
}

void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    glColor4f((float)r / 255, (float)g / 255, (float)b / 255, (float)a / 255);
}

void glTexCoord2f(GLfloat u, GLfloat v)
{
    G.cur[3] = u;
    G.cur[4] = v;
}

void glNormal3f(GLfloat x, GLfloat y, GLfloat z)
{
    G.cur[5] = x;
    G.cur[6] = y;
    G.cur[7] = z;
}

void glNormal3fv(const GLfloat *n)
{
    glNormal3f(n[0], n[1], n[2]);
}

void glGenTextures(GLsizei n, GLuint *ids)
{
    for (int i = 0; i < n; i++)
        ids[i] = tex_new();
}

void glDeleteTextures(GLsizei n, const GLuint *ids)
{
    flush();
    for (int i = 0; i < n; i++)
        if (ids[i] > 0 && ids[i] < NTEX && ids[i] != G.white && tex[ids[i]].used) {
            tex_free(&tex[ids[i]]);
            tex[ids[i]].used = 0;
            if (G.bound == ids[i])
                G.bound = 0;
        }
}

void glBindTexture(GLenum target, GLuint id)
{
    (void)target;
    G.bound = id < NTEX && tex[id].used ? id : 0;
}

void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels)
{
    (void)target;
    (void)internal;
    (void)border;
    if (level != 0 || !G.bound || type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_BGRA))
        return;
    flush(); /* gesammelte Dreiecke koennten die alte Textur benutzen */
    tex_image(&tex[G.bound], w, h, format, pixels);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    (void)target;
    if (G.bound && (pname == GL_TEXTURE_MAG_FILTER || pname == GL_TEXTURE_MIN_FILTER)) {
        flush();
        tex[G.bound].linear = param == GL_LINEAR;
    }
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *v)
{
    if (light != GL_LIGHT0)
        return;
    if (pname == GL_POSITION) { /* Richtung (w = 0) mit der aktuellen Modellmatrix ins Auge */
        const float *mv = top(0);
        for (int i = 0; i < 3; i++)
            G.light[i] = mv[i * 4] * v[0] + mv[i * 4 + 1] * v[1] + mv[i * 4 + 2] * v[2];
        float n = gl_sqrtf(G.light[0] * G.light[0] + G.light[1] * G.light[1] + G.light[2] * G.light[2]);
        for (int i = 0; n > 0 && i < 3; i++)
            G.light[i] /= n;
    } else if (pname == GL_AMBIENT) {
        G.amb = v[0];
    } else if (pname == GL_DIFFUSE) {
        G.dif = v[0];
    }
}

/* ---------- Puffer und Vertex-Arrays ----------
 *
 * Ein Puffer ist geteilter Speicher (ganze Seiten), fuer die GPU als Flaeche angemeldet (1024 Pixel = 4 KiB je Zeile).
 * Er bleibt ueber viele Bilder; glDrawArrays/glDrawElements schicken nur noch den Zustand, Anfang und Abstand je
 * Attribut (SYS_GPUCOMP 9) - die GPU liest Ecken und Indizes selbst. Das Abschneiden an der nahen Ebene macht die
 * GPU dabei nicht: vorher wird der Kasten um die benutzten Ecken geprueft (pos_box, zwischengespeichert, solange
 * sich der Puffer nicht aendert). Ganz ausserhalb: nichts zeichnen. Ganz innerhalb von nah, fern und Schutzstreifen:
 * direkt auf der GPU. Sonst - und fuer alles, was die GPU so nicht kann (Ecken im Programmspeicher, Vierecke, andere
 * Formate) - setzt die Bibliothek die Dreiecke zusammen wie bei glBegin/glEnd. Die CPU rastert immer selbst. */

typedef struct {
    int      used, surf;
    unsigned char      *data;
    u64      size, cap;         /* benutzt (glBufferData), angelegt (Seiten) */
    unsigned shm;
    u32      gen;               /* Zaehler der Aenderungen: Zwischenergebnisse unten gelten nur fuer einen Stand */
    struct { u32 gen, valid; u64 off; int count, isz; u32 lo, hi; } ir;          /* Indizes: kleinster, groesster */
    struct { u32 gen, valid; u64 off; int stride, n; u32 lo, hi; float box[6]; } bb; /* Kasten der Positionen */
} Buf;

static Buf   *bufs;
static GLuint nbufs;            /* Eintraege in bufs (Nummer 0 bleibt frei) */

enum { A_POS, A_TEX, A_NRM, A_COL };
typedef struct {
    int    on, size, stride;
    GLenum type;
    u64    ptr;                 /* Adresse, mit Puffer: Abstand darin */
    GLuint buf;
} Arr;

static Arr    arr[4];
static GLuint array_buf, elem_buf;

static Buf *buf_get(GLuint id)
{
    return id && id < nbufs && bufs[id].used ? &bufs[id] : 0;
}

static void buf_release(Buf *b)
{
    if (b->surf)
        sys_gpucomp(2, (u64)b->surf, 0);
    if (b->data)
        sys_shm_unmap(b->data);
    b->surf = 0;
    b->data = 0;
    b->size = b->cap = 0;
    b->gen++;
}

static void bufs_free(void)
{
    for (GLuint i = 1; i < nbufs; i++)
        if (bufs[i].used) {
            buf_release(&bufs[i]);
            bufs[i].used = 0;
        }
    array_buf = elem_buf = 0;
    memset(arr, 0, sizeof(arr));
}

void glGenBuffers(GLsizei n, GLuint *ids)
{
    for (int k = 0; k < n; k++) {
        GLuint id = 1;
        while (id < nbufs && bufs[id].used)
            id++;
        if (id >= nbufs) { /* Tabelle verdoppeln */
            GLuint nn = nbufs ? nbufs * 2 : 64;
            Buf *nb = u_malloc((u64)nn * sizeof(Buf));
            if (!nb) {
                ids[k] = 0;
                continue;
            }
            memset(nb, 0, (u64)nn * sizeof(Buf));
            if (bufs) {
                memcpy(nb, bufs, (u64)nbufs * sizeof(Buf));
                u_free(bufs);
            }
            bufs = nb;
            nbufs = nn;
        }
        memset(&bufs[id], 0, sizeof(Buf));
        bufs[id].used = 1;
        ids[k] = id;
    }
}

void glDeleteBuffers(GLsizei n, const GLuint *ids)
{
    flush();
    for (int k = 0; k < n; k++) {
        Buf *b = buf_get(ids[k]);
        if (!b)
            continue;
        buf_release(b);
        b->used = 0;
        if (array_buf == ids[k])
            array_buf = 0;
        if (elem_buf == ids[k])
            elem_buf = 0;
    }
}

void glBindBuffer(GLenum target, GLuint id)
{
    if (id && !buf_get(id))
        return;
    if (target == GL_ARRAY_BUFFER)
        array_buf = id;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        elem_buf = id;
}

static Buf *bound(GLenum target)
{
    return buf_get(target == GL_ARRAY_BUFFER ? array_buf : target == GL_ELEMENT_ARRAY_BUFFER ? elem_buf : 0);
}

void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    (void)usage;
    Buf *b = bound(target);
    if (!b || size < 0)
        return;
    if ((u64)size > b->cap || !b->data) { /* neu anlegen: ganze Seiten, fuer die GPU 4 KiB je Zeile */
        buf_release(b);
        u64 bytes = ((u64)size + 4095) & ~4095ULL;
        if (!bytes)
            bytes = 4096;
        unsigned id;
        s64 a = sys_shm_create(bytes, &id);
        if (a < 0)
            return;
        b->data = (unsigned char *)a;
        b->shm = id;
        b->cap = bytes;
        if (G.gpu && bytes / 4096 <= 16384) {
            s64 r = sys_gpucomp(1, id, 1024 | (bytes / 4096) << 16);
            b->surf = r > 0 ? (int)r : 0; /* sonst setzt die Bibliothek die Dreiecke zusammen */
        }
    }
    b->size = (u64)size;
    if (data)
        memcpy(b->data, data, (u64)size);
    else
        memset(b->data, 0, (u64)size);
    b->gen++;
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
    Buf *b = bound(target);
    if (!b || offset < 0 || size < 0 || (u64)offset + (u64)size > b->size)
        return;
    memcpy(b->data + offset, data, (u64)size);
    b->gen++;
}

void *glMapBuffer(GLenum target, GLenum access)
{
    (void)access;
    Buf *b = bound(target);
    if (!b)
        return 0;
    b->gen++; /* das Programm schreibt vielleicht hinein */
    return b->data;
}

GLboolean glUnmapBuffer(GLenum target)
{
    Buf *b = bound(target);
    if (b)
        b->gen++;
    return b != 0;
}

static void set_arr(int a, int size, GLenum type, GLsizei stride, const GLvoid *p)
{
    int ts = type == GL_FLOAT ? 4 : type == GL_UNSIGNED_BYTE && a == A_COL ? 1 : 0;
    if (!ts || size < 1 || size > 4 || stride < 0)
        return;
    Arr *r = &arr[a];
    r->size = size;
    r->type = type;
    r->stride = stride ? stride : size * ts;
    r->ptr = (u64)p;
    r->buf = array_buf;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p)
{
    if (size >= 2)
        set_arr(A_POS, size, type, stride, p);
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p)
{
    set_arr(A_TEX, size, type, stride, p);
}

void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p)
{
    set_arr(A_NRM, 3, type, stride, p);
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p)
{
    if (size >= 3)
        set_arr(A_COL, size, type, stride, p);
}

static int client_arr(GLenum cap)
{
    return cap == GL_VERTEX_ARRAY ? A_POS : cap == GL_TEXTURE_COORD_ARRAY ? A_TEX : cap == GL_NORMAL_ARRAY ? A_NRM
         : cap == GL_COLOR_ARRAY ? A_COL : -1;
}

void glEnableClientState(GLenum cap)
{
    int a = client_arr(cap);
    if (a >= 0)
        arr[a].on = 1;
}

void glDisableClientState(GLenum cap)
{
    int a = client_arr(cap);
    if (a >= 0)
        arr[a].on = 0;
}

/* Attribut der Ecke i: Zeiger auf die Daten oder 0 (Puffer geloescht oder Ecke dahinter: dann gilt 0) */
static const unsigned char *arr_at(const Arr *r, u32 i)
{
    u64 n = (u64)r->size * (r->type == GL_FLOAT ? 4 : 1), off = r->ptr + (u64)i * (u64)r->stride;
    if (!r->buf)
        return (const unsigned char *)off;
    const Buf *b = buf_get(r->buf);
    return b && off + n <= b->size ? b->data + off : 0;
}

/* Ecke i aus den Arrays im Format der Dreiecke (x, y, z, u, v, nx, ny, nz, r, g, b, a); ohne Array der aktuelle Wert */
static void fetch(u32 i, float *v)
{
    static const int dst[4] = {0, 3, 5, 8}, cnt[4] = {3, 2, 3, 4};
    memcpy(v, G.cur, sizeof(G.cur));
    for (int a = 0; a < 4; a++) {
        const Arr *r = &arr[a];
        if (!r->on)
            continue;
        const unsigned char *p = arr_at(r, i);
        float t[4] = {0, 0, 0, 1};
        for (int c = 0; p && c < r->size; c++)
            if (r->type == GL_FLOAT)
                memcpy(&t[c], p + 4 * c, 4);
            else
                t[c] = (float)p[c] * (1.0f / 255);
        for (int c = 0; c < cnt[a]; c++)
            v[dst[a] + c] = t[c];
    }
}

static struct {
    const unsigned char *ib;               /* Indizes oder 0 (der Reihe nach ab first) */
    int       isz, first, noclip;
} D;

static u32 vidx(int k)
{
    if (!D.ib)
        return (u32)(D.first + k);
    if (D.isz == 1)
        return D.ib[k];
    if (D.isz == 2) {
        unsigned short v;
        memcpy(&v, D.ib + 2 * k, 2);
        return v;
    }
    u32 v;
    memcpy(&v, D.ib + 4 * (u64)k, 4);
    return v;
}

static void tri_idx(int a, int b, int c)
{
    float va[VF], vb[VF], vc[VF];
    fetch(vidx(a), va);
    fetch(vidx(b), vb);
    fetch(vidx(c), vc);
    if (D.noclip)
        emit(va, vb, vc);
    else
        tri_v(va, vb, vc);
}

/* kleinster und groesster Index (zwischengespeichert, solange der Puffer gleich bleibt) */
static void index_range(Buf *b, u64 off, int count, int isz, u32 *lo, u32 *hi)
{
    if (!(b->ir.valid && b->ir.gen == b->gen && b->ir.off == off && b->ir.count == count && b->ir.isz == isz)) {
        D.ib = b->data + off;
        D.isz = isz;
        u32 l = 0xFFFFFFFFu, h = 0;
        for (int k = 0; k < count; k++) {
            u32 v = vidx(k);
            l = v < l ? v : l;
            h = v > h ? v : h;
        }
        b->ir.valid = 1;
        b->ir.gen = b->gen;
        b->ir.off = off;
        b->ir.count = count;
        b->ir.isz = isz;
        b->ir.lo = l;
        b->ir.hi = h;
    }
    *lo = b->ir.lo;
    *hi = b->ir.hi;
}

/* Kasten (min x, y, z, max x, y, z) um die Positionen der Ecken lo..hi; Ecken hinter dem Pufferende zaehlen als 0 */
static void pos_box(Buf *b, const Arr *r, u32 lo, u32 hi, float *box)
{
    if (!(b->bb.valid && b->bb.gen == b->gen && b->bb.off == r->ptr && b->bb.stride == r->stride &&
          b->bb.n == r->size && b->bb.lo == lo && b->bb.hi == hi)) {
        u64 n = (u64)r->size * 4, last = r->ptr + n <= b->size ? (b->size - r->ptr - n) / (u64)r->stride : 0;
        int zero = r->ptr + n > b->size || hi > last;
        u32 end = hi > last ? (u32)last : hi;
        float m[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (u32 i = lo; i <= end && r->ptr + n <= b->size; i++) {
            float p[3] = {0, 0, 0};
            memcpy(p, b->data + r->ptr + (u64)i * (u64)r->stride, (u64)(r->size < 3 ? r->size : 3) * 4);
            for (int c = 0; c < 3; c++) {
                m[c] = p[c] < m[c] ? p[c] : m[c];
                m[3 + c] = p[c] > m[3 + c] ? p[c] : m[3 + c];
            }
            if (i == 0xFFFFFFFFu)
                break;
        }
        if (zero || m[0] > m[3])
            for (int c = 0; c < 3; c++) {
                m[c] = m[c] < 0 ? m[c] : 0;
                m[3 + c] = m[3 + c] > 0 ? m[3 + c] : 0;
            }
        b->bb.valid = 1;
        b->bb.gen = b->gen;
        b->bb.off = r->ptr;
        b->bb.stride = r->stride;
        b->bb.n = r->size;
        b->bb.lo = lo;
        b->bb.hi = hi;
        memcpy(b->bb.box, m, sizeof(m));
    }
    memcpy(box, b->bb.box, sizeof(b->bb.box));
}

/* 0 = Kasten ganz ausserhalb des Bildes, 1 = ganz innerhalb von nah, fern und Schutzstreifen, 2 = muss abschneiden */
static int box_test(const float *box)
{
    float d[8][NPLANE];
    for (int k = 0; k < 8; k++) {
        float p[3] = {box[k & 1 ? 3 : 0], box[k & 2 ? 4 : 1], box[k & 4 ? 5 : 2]};
        plane_dist(p, d[k]);
    }
    int clip = 0;
    for (int e = 0; e < NPLANE; e++) {
        int out = 0;
        for (int k = 0; k < 8; k++)
            out += d[k][e] < 0;
        if (out == 8 && e < 6)
            return 0;
        if (out && (e < 2 || e >= 6))
            clip = 1;
    }
    return clip ? 2 : 1;
}

/* direkt auf der GPU: alle Arrays in angemeldeten Puffern, Formate, die die Hardware liest; 1 = gezeichnet */
static int gpu_draw(GLenum mode, int first, int count, const Buf *ib, u64 ioff, int isz)
{
    int prim = mode == GL_TRIANGLES ? GPU3D_PRIM_TRIANGLES : mode == GL_TRIANGLE_STRIP ? GPU3D_PRIM_STRIP
             : mode == GL_TRIANGLE_FAN ? GPU3D_PRIM_FAN : 0;
    if (!prim || (isz && !ib->surf))
        return 0;
    Gpu3dDrawVB v;
    memset(&v, 0, sizeof(v));
    const float *c = G.cur;
    float val[4][4] = {{0, 0, 0, 1}, {c[3], c[4], 0, 1}, {c[5], c[6], c[7], 0}, {c[8], c[9], c[10], c[11]}};
    memcpy(v.value, val, sizeof(val));
    for (int a = 0; a < 4; a++) {
        const Arr *r = &arr[a];
        if (!r->on)
            continue;
        const Buf *b = buf_get(r->buf);
        if (!b || !b->surf || (r->ptr & 3) || (r->stride & 3) || r->stride > 2048 || r->ptr >= b->cap)
            return 0;
        int fmt = r->type == GL_FLOAT ? r->size : r->size == 4 ? GPU3D_F_UBYTE4N : 0;
        if (!fmt)
            return 0;
        v.attr[a].surf = (unsigned short)b->surf;
        v.attr[a].format = (unsigned short)fmt;
        v.attr[a].offset = (unsigned)r->ptr;
        v.attr[a].stride = (unsigned)r->stride;
    }
    flush(); /* gesammelte Dreiecke zuerst (Reihenfolge) */
    state_draw(&v.d);
    v.index_surf = (unsigned short)(isz ? ib->surf : 0);
    v.index_size = (unsigned short)isz;
    v.index_offset = (unsigned)ioff;
    v.prim = (unsigned)prim;
    v.first = isz ? 0 : (unsigned)first;
    v.count = (unsigned)count;
    s64 rc = sys_gpu3d_vb(&v);
    if (rc == -38)
        gpu_off("keine 3D-Pipeline");
    return rc >= 0;
}

/* CPU: die benutzten Ecken lo..hi einmal umrechnen (bei Indizes gehoert jede Ecke meist zu mehreren Dreiecken),
 * dann die Dreiecke daraus zusammensetzen; nur die, die abgeschnitten werden muessen, gehen den Weg ueber tri_v */
static float    (*xs)[10];
static unsigned *xoc;
static u32       xcap, xlo;

static void tri_cpu(int a, int b, int c)
{
    u32 i[3] = {vidx(a) - xlo, vidx(b) - xlo, vidx(c) - xlo};
    unsigned oa = xoc[i[0]], ob = xoc[i[1]], oc = xoc[i[2]];
    if (oa & ob & oc & OUT_REJECT)
        return;
    if ((oa | ob | oc) & OUT_CLIP) {
        float v[3][VF];
        for (int k = 0; k < 3; k++)
            fetch(i[k] + xlo, v[k]);
        tri_v(v[0], v[1], v[2]);
        return;
    }
    tris_add(xs[i[0]], xs[i[1]], xs[i[2]]);
}

static int cpu_arrays(GLenum mode, int count, u32 lo, u32 hi)
{
    u64 n = (u64)hi - lo + 1;
    if (n > 4 * (u64)count + 64)
        return 0; /* viele Ecken, die gar nicht benutzt werden */
    if (n > xcap) {
        float (*ns)[10] = u_malloc(n * sizeof(xs[0]));
        unsigned *no = u_malloc(n * sizeof(xoc[0]));
        if (!ns || !no) {
            if (ns)
                u_free(ns);
            if (no)
                u_free(no);
            return 0;
        }
        if (xs) {
            u_free(xs);
            u_free(xoc);
        }
        xs = ns;
        xoc = no;
        xcap = (u32)n;
    }
    flush(); /* gesammelte Dreiecke zuerst (Reihenfolge) */
    for (u64 k = 0; k < n; k++) {
        float v[VF], P[4];
        fetch(lo + (u32)k, v);
        project(v, xs[k], P);
        xoc[k] = outcode(P);
    }
    xlo = lo;
    sink = emit_cpu;
    prims(mode, count, tri_cpu);
    sink = emit;
    tris_run();
    return 1;
}

static void draw(GLenum mode, int first, int count, int isz, const GLvoid *indices)
{
    if (!G.open || count <= 0 || first < 0 || !arr[A_POS].on)
        return;
    const unsigned char *ib = 0;
    Buf *ibuf = 0;
    u64 ioff = 0;
    if (isz) {
        if (elem_buf) {
            ibuf = buf_get(elem_buf);
            ioff = (u64)indices;
            if (!ibuf || ioff % (u64)isz || ioff + (u64)count * (u64)isz > ibuf->size)
                return;
            ib = ibuf->data + ioff;
        } else if (!(ib = indices)) {
            return;
        }
    }
    cur_state();
    u32 lo = (u32)first, hi = (u32)(first + count - 1);
    if (ibuf) {
        index_range(ibuf, ioff, count, isz, &lo, &hi);
    } else if (isz) { /* Indizes im Programmspeicher: jedes Mal durchsehen */
        D.ib = ib;
        D.isz = isz;
        lo = 0xFFFFFFFFu;
        hi = 0;
        for (int k = 0; k < count; k++) {
            u32 v = vidx(k);
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
    }
    int vis = 2;
    Buf *pb = buf_get(arr[A_POS].buf);
    if (pb && arr[A_POS].buf && (!isz || ibuf)) {
        float box[6];
        pos_box(pb, &arr[A_POS], lo, hi, box);
        vis = box_test(box);
    }
    if (vis == 0)
        return;
    if (vis == 1 && G.gpu && gpu_draw(mode, first, count, ibuf, ioff, isz))
        return;
    D.ib = ib;
    D.isz = isz;
    D.first = first;
    D.noclip = vis == 1;
    if (!G.gpu && cpu_arrays(mode, count, lo, hi))
        return;
    prims(mode, count, tri_idx);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    draw(mode, first, count, 0, 0);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    int isz = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 0;
    if (isz)
        draw(mode, 0, count, isz, indices);
}
