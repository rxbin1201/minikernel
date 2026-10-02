#include "gfx.h"
#include "gl.h"

/* gldemo: drehende Wuerfel mit Textur, Licht und Tiefentest ueber das kleine OpenGL (gl.h) - mit der Intel-GPU
 * gezeichnet, sonst mit der CPU. Im Titel: Bilder pro Sekunde und wer zeichnet.
 *   gldemo        normal
 *   gldemo -cpu   immer mit der CPU (zum Vergleich)
 * Esc oder q: Ende */

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
            }
        float a = (float)(sys_time_us() - t0) * 60e-6f; /* 60 Grad pro Sekunde */

        s64 d0 = sys_time_us();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glLightfv(GL_LIGHT0, GL_POSITION, light);
        glTranslatef(0, 0, -8);
        glPushMatrix();
        glRotatef(a, 1, 1, 0);
        glRotatef(a * 0.7f, 0, 1, 0);
        glColor3f(1, 1, 1);
        cube();
        glPopMatrix();
        for (int i = 0; i < 3; i++) { /* drei kleine Wuerfel kreisen herum */
            glPushMatrix();
            glRotatef(a * 1.3f + (float)i * 120, 0, 1, 0.25f);
            glTranslatef(3.2f, 0, 0);
            glRotatef(a * 3, 1, 0, 0);
            glScalef(0.45f, 0.45f, 0.45f);
            glColor3f(colors[i][0], colors[i][1], colors[i][2]);
            cube();
            glPopMatrix();
        }
        glFinish();
        draw_us += sys_time_us() - d0;
        gl_swap();

        frames++;
        s64 now = sys_time_us();
        if (now - sec >= 1000000) {
            char title[64];
            int us = frames ? (int)(draw_us / frames) : 0;
            snprintf(title, sizeof(title), "GL-Demo - %d Bilder/s (%s, %d,%d ms je Bild)",
                     (int)((s64)frames * 1000000 / (now - sec)), gl_gpu() ? "GPU" : "CPU", us / 1000, us % 1000 / 100);
            gfx_set_title(title);
            frames = 0;
            draw_us = 0;
            sec = now;
        }
    }
}
