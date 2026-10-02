#include "gl.h"
#include "gfx.h"
#include "malloc.h"
#include "user.h"

/* Kleines OpenGL (siehe gl.h). Die Bibliothek sammelt Dreiecke mit gleichem Zustand (Matrix, Licht, Textur, Tiefentest)
 * und gibt sie gebuendelt ab: der GPU per SYS_GPUCOMP 8 (Eckpunkte unveraendert, Matrix und Licht rechnet der
 * Vertex-Shader), sonst dem Rasterer hier unten, der dasselbe rechnet wie die GPU (Ecke mal Matrix, Teilen durch W,
 * Farbe und Textur perspektivisch richtig interpoliert, Tiefe "kleiner").
 *
 * Matrizen: Zeile fuer Zeile (m[zeile * 4 + spalte]), wirken auf Spaltenvektoren - wie OpenGL, das sie spaltenweise
 * speichert. Die fertige Matrix ist Bildbereich * Projektion * Modell: sie bringt eine Ecke direkt auf Pixel. */

#define VF    12                /* float je Eckpunkt: x, y, z, u, v, nx, ny, nz, r, g, b, a */
#define BATCH GPU3D_MAX_VERT
#define PRIM  4096              /* Eckpunkte zwischen glBegin und glEnd */
#define NTEX  64
#define STACK 16
#define PI    3.14159265358979f

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

/* s[k]: x, y, z (Bildschirm), 1/W, dann u, v, r, g, b, a jeweils mal 1/W */
static void raster(float s[3][10], const Tex *t)
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
    for (int y = ay; y <= by; y++) {
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
                G.zb[i] = z;
            }
            float iw = 1.0f / (w0 * s[0][3] + w1 * s[1][3] + w2 * s[2][3]), a[6], c[4];
            for (int k = 0; k < 6; k++)
                a[k] = (w0 * s[0][4 + k] + w1 * s[1][4 + k] + w2 * s[2][4 + k]) * iw;
            sample(t, a[0], a[1], c);
            u32 r = (u32)(sat(a[2] * c[0]) * 255 + 0.5f), g = (u32)(sat(a[3] * c[1]) * 255 + 0.5f),
                b = (u32)(sat(a[4] * c[2]) * 255 + 0.5f), al = (u32)(sat(a[5] * c[3]) * 255 + 0.5f);
            G.px[i] = al << 24 | r << 16 | g << 8 | b;
        }
    }
}

static void cpu_draw(void)
{
    const Tex *t = &tex[G.bs.tex];
    const float *m = G.bs.m;
    for (int i = 0; i + 2 < G.bn; i += 3) {
        float s[3][10];
        int ok = 1;
        for (int k = 0; k < 3; k++) {
            const float *p = G.bv[i + k];
            float P[4];
            for (int r = 0; r < 4; r++)
                P[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
            if (P[3] <= 1e-6f) {
                ok = 0;
                break;
            }
            float iw = 1.0f / P[3];
            float br = G.bs.amb + G.bs.dif * sat(p[5] * G.bs.l[0] + p[6] * G.bs.l[1] + p[7] * G.bs.l[2]);
            s[k][0] = P[0] * iw;
            s[k][1] = P[1] * iw;
            s[k][2] = P[2] * iw;
            s[k][3] = iw;
            s[k][4] = p[3] * iw;
            s[k][5] = p[4] * iw;
            for (int c = 0; c < 3; c++)
                s[k][6 + c] = p[8 + c] * br * iw;
            s[k][9] = p[11] * iw;
        }
        if (ok)
            raster(s, t);
    }
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

static void flush(void)
{
    if (!G.bn)
        return;
    if (G.gpu) {
        Gpu3dDraw d;
        base_draw(&d);
        const Tex *t = &tex[G.bs.tex];
        d.tex = (unsigned short)t->surf;
        d.tex_w = (unsigned short)t->w;
        d.tex_h = (unsigned short)t->h;
        d.flags = (unsigned short)((G.bs.depth ? GPU3D_DEPTH : 0) | (t->linear ? GPU3D_LINEAR : 0));
        memcpy(d.m, G.bs.m, sizeof(d.m));
        memcpy(d.light, G.bs.l, sizeof(d.light));
        d.ambient = G.bs.amb;
        d.diffuse = G.bs.dif;
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
    int depth = G.depth_test != 0;
    if (G.bn && (memcmp(m, G.bs.m, sizeof(m)) || memcmp(l, G.bs.l, sizeof(l)) || amb != G.bs.amb ||
                 dif != G.bs.dif || tid != G.bs.tex || depth != G.bs.depth))
        flush();
    memcpy(G.bs.m, m, sizeof(m));
    memcpy(G.bs.l, l, sizeof(l));
    G.bs.amb = amb;
    G.bs.dif = dif;
    G.bs.tex = tid;
    G.bs.depth = depth;
}

static void tri(int a, int b, int c)
{
    if (G.bn + 3 > BATCH)
        flush();
    memcpy(G.bv[G.bn++], G.pv[a], sizeof(G.pv[0]));
    memcpy(G.bv[G.bn++], G.pv[b], sizeof(G.pv[0]));
    memcpy(G.bv[G.bn++], G.pv[c], sizeof(G.pv[0]));
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

void gl_close(void)
{
    if (!G.open)
        return;
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
    G.clear_color = (u32)(sat(a) * 255 + 0.5f) << 24 | (u32)(sat(r) * 255 + 0.5f) << 16 |
                    (u32)(sat(g) * 255 + 0.5f) << 8 | (u32)(sat(b) * 255 + 0.5f);
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
    for (int i = 0; i < G.w * G.h; i++) {
        if (c)
            G.px[i] = G.clear_color;
        if (z)
            G.zb[i] = G.clear_depth;
    }
}

void glEnable(GLenum cap)
{
    if (cap == GL_DEPTH_TEST)
        G.depth_test = 1;
    else if (cap == GL_TEXTURE_2D)
        G.texturing = 1;
    else if (cap == GL_LIGHTING)
        G.lighting = 1;
}

void glDisable(GLenum cap)
{
    if (cap == GL_DEPTH_TEST)
        G.depth_test = 0;
    else if (cap == GL_TEXTURE_2D)
        G.texturing = 0;
    else if (cap == GL_LIGHTING)
        G.lighting = 0;
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
    int n = G.pn;
    cur_state();
    switch (G.prim) {
    case GL_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3)
            tri(i, i + 1, i + 2);
        break;
    case GL_TRIANGLE_STRIP:
        for (int i = 2; i < n; i++)
            if (i & 1)
                tri(i - 1, i - 2, i);
            else
                tri(i - 2, i - 1, i);
        break;
    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        for (int i = 2; i < n; i++)
            tri(0, i - 1, i);
        break;
    case GL_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            tri(i, i + 1, i + 2);
            tri(i, i + 2, i + 3);
        }
        break;
    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < n; i += 2) {
            tri(i, i + 1, i + 3);
            tri(i, i + 3, i + 2);
        }
        break;
    }
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
