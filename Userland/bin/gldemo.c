#include "gfx.h"
#include "gl.h"

/* gldemo: drehender Wuerfel und drei kreisende Ringe mit Textur, Licht und Tiefentest ueber das kleine OpenGL (gl.h)
 * - mit der Intel-GPU gezeichnet, sonst mit der CPU. Im Titel: Bilder pro Sekunde und wer zeichnet. Die Ringe
 * (je 2304 Dreiecke) liegen in Puffern (glBufferData, glDrawElements) und werden nur einmal hochgeladen.
 *   gldemo        normal
 *   gldemo -cpu   immer mit der CPU (zum Vergleich)
 * Pfeil hoch/runter oder w/s: Kamera vor und zurueck (auch durch den Wuerfel - zeigt das Abschneiden an der nahen
 * Ebene), c: Rueckseiten weglassen an/aus, b: grosser Wuerfel aus Glas (Mischen) an/aus, v: Ringe aus Puffern oder
 * mit glBegin/glEnd (zum Vergleich), Esc oder q: Ende */

static u32 texture[64 * 64];

static void make_texture(void)
{
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int edge = x < 3 || y < 3 || x >= 61 || y >= 61;
            int check = ((x >> 3) ^ (y >> 3)) & 1;
            int diag = (x - y < 4 && y - x < 4) || (x + y > 59 && x + y < 68);
            u32 c = edge ? 0xFFFFFFFFu : diag ? 0xFFFFD040u : check ? 0xFFE8E8E8u : 0xFF9098A8u;
            texture[y * 64 + x] = c;
        }
}

static void cube(void)
{
    static const float v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                  {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    static const int f[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}};
    static const float n[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    static const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    glBegin(GL_QUADS);
    for (int i = 0; i < 6; i++) {
        glNormal3fv(n[i]);
        for (int k = 0; k < 4; k++) {
            glTexCoord2f(uv[k][0], uv[k][1]);
            glVertex3fv(v[f[i][k]]);
        }
    }
    glEnd();
}

/* Ring (Torus) um die z-Achse: RS x TS Ecken mit Position, Textur, Normale (8 float), 6 Indizes je Viereck */
#define RS 48
#define TS 24
static float          ring_v[RS * TS][8];
static unsigned short ring_i[RS * TS * 6];
static GLuint         ring_buf[2];

static void make_ring(void)
{
    const float R = 1, r = 0.38f, pi2 = 6.2831853f;
    for (int i = 0; i < RS; i++)
        for (int j = 0; j < TS; j++) {
            float a = pi2 * (float)i / RS, b = pi2 * (float)j / TS;
            float ca = gl_cosf(a), sa = gl_sinf(a), cb = gl_cosf(b), sb = gl_sinf(b);
            float *v = ring_v[i * TS + j];
            v[0] = (R + r * cb) * ca;
            v[1] = (R + r * cb) * sa;
            v[2] = r * sb;
            v[3] = (float)i * 4 / RS;
            v[4] = (float)j / TS;
            v[5] = cb * ca;
            v[6] = cb * sa;
            v[7] = sb;
        }
    int n = 0;
    for (int i = 0; i < RS; i++)
        for (int j = 0; j < TS; j++) {
            unsigned short a = (unsigned short)(i * TS + j), b = (unsigned short)((i + 1) % RS * TS + j);
            unsigned short c = (unsigned short)((i + 1) % RS * TS + (j + 1) % TS), d = (unsigned short)(i * TS + (j + 1) % TS);
            unsigned short q[6] = {a, b, c, a, c, d};
            for (int k = 0; k < 6; k++)
                ring_i[n++] = q[k];
        }
    glGenBuffers(2, ring_buf);
    glBindBuffer(GL_ARRAY_BUFFER, ring_buf[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(ring_v), ring_v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ring_buf[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(ring_i), ring_i, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void ring(int vbo)
{
    if (!vbo) { /* dasselbe Ecke fuer Ecke, je Reihe ein glBegin/glEnd */
        for (int i = 0; i < RS; i++) {
            glBegin(GL_TRIANGLES);
            for (int k = i * TS * 6; k < (i + 1) * TS * 6; k++) {
                const float *v = ring_v[ring_i[k]];
                glTexCoord2f(v[3], v[4]);
                glNormal3fv(v + 5);
                glVertex3fv(v);
            }
            glEnd();
        }
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, ring_buf[0]);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ring_buf[1]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(ring_v[0]), (const void *)0);
    glTexCoordPointer(2, GL_FLOAT, sizeof(ring_v[0]), (const void *)12);
    glNormalPointer(GL_FLOAT, sizeof(ring_v[0]), (const void *)20);
    glDrawElements(GL_TRIANGLES, RS * TS * 6, GL_UNSIGNED_SHORT, 0);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void _start(int argc, char **argv)
{
    gl_force_cpu(argc > 1 && strcmp(argv[1], "-cpu") == 0);
    if (gl_open(640, 480, "GL-Demo") != 0)
        sys_exit(1);
    make_texture();
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_BGRA, GL_UNSIGNED_BYTE, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);
    glEnable(GL_CULL_FACE); /* die Wuerfel sind geschlossen: Rueckseiten sieht man nie */
    int culling = 1, glass = 1, vbo = 1;
    make_ring();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float dist = 8;         /* Abstand der Kamera zur Mitte */
    glClearColor(0.08f, 0.10f, 0.16f, 1);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45, (double)gl_width() / gl_height(), 1, 50);

    static const float colors[3][3] = {{1, 0.45f, 0.4f}, {0.45f, 1, 0.5f}, {0.45f, 0.6f, 1}};
    const float light[4] = {0.5f, 0.7f, 1, 0};
    s64 t0 = sys_time_us(), sec = t0;
    int frames = 0;
    s64 draw_us = 0; /* Zeichnen (bis die GPU bzw. CPU fertig ist) je Sekunde */
    for (;;) {
        Event e;
        while (gfx_poll(&e))
            if (e.type == EV_CLOSE || (e.type == EV_KEY && (e.key == 0x1B || e.key == 'q'))) {
                gl_close();
                sys_exit(0);
            } else if (e.type == EV_KEY && (e.key == KEY_UP || e.key == 'w')) {
                dist = dist > 0.5f ? dist - 0.25f : dist;
            } else if (e.type == EV_KEY && (e.key == KEY_DOWN || e.key == 's')) {
                dist = dist < 30 ? dist + 0.25f : dist;
            } else if (e.type == EV_KEY && e.key == 'v') {
                vbo = !vbo;
            } else if (e.type == EV_KEY && e.key == 'b') {
                glass = !glass;
            } else if (e.type == EV_KEY && e.key == 'c') {
                culling = !culling;
                if (culling)
                    glEnable(GL_CULL_FACE);
                else
                    glDisable(GL_CULL_FACE);
            }
        float a = (float)(sys_time_us() - t0) * 60e-6f; /* 60 Grad pro Sekunde */

        s64 d0 = sys_time_us();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glLightfv(GL_LIGHT0, GL_POSITION, light);
        glTranslatef(0, 0, -dist);
        if (!glass) {
            glPushMatrix();
            glRotatef(a, 1, 1, 0);
            glRotatef(a * 0.7f, 0, 1, 0);
            glColor3f(1, 1, 1);
            cube();
            glPopMatrix();
        }
        for (int i = 0; i < 3; i++) { /* drei Ringe kreisen herum */
            glPushMatrix();
            glRotatef(a * 1.3f + (float)i * 120, 0, 1, 0.25f);
            glTranslatef(3.2f, 0, 0);
            glRotatef(a * 3, 1, 0, 0);
            glScalef(0.6f, 0.6f, 0.6f);
            glColor3f(colors[i][0], colors[i][1], colors[i][2]);
            ring(vbo);
            glPopMatrix();
        }
        if (glass) { /* durchsichtig: nach allem Deckenden, Tiefe nur pruefen; erst die hinteren, dann die vorderen Seiten */
            glPushMatrix();
            glRotatef(a, 1, 1, 0);
            glRotatef(a * 0.7f, 0, 1, 0);
            glColor4f(0.7f, 0.9f, 1, 0.4f);
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            cube();
            glCullFace(GL_BACK);
            cube();
            if (!culling)
                glDisable(GL_CULL_FACE);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glPopMatrix();
        }
        glFinish();
        draw_us += sys_time_us() - d0;
        gl_swap();

        frames++;
        s64 now = sys_time_us();
        if (now - sec >= 1000000) {
            char title[96];
            int us = frames ? (int)(draw_us / frames) : 0;
            snprintf(title, sizeof(title), "GL-Demo - %d Bilder/s (%s, %d,%d ms je Bild, %s%s%s)",
                     (int)((s64)frames * 1000000 / (now - sec)), gl_gpu() ? "GPU" : "CPU", us / 1000, us % 1000 / 100, vbo ? "Puffer" : "glBegin",
                     culling ? "" : ", ohne Culling", glass ? ", Glas" : "");
            gfx_set_title(title);
            frames = 0;
            draw_us = 0;
            sec = now;
        }
    }
}
