#include "gfx.h"
#include "gl.h"

/* Prueft das kleine OpenGL (gl.c) mit der CPU: Abschneiden an der nahen und fernen Ebene und am Schutzstreifen,
 * Rueckseiten weglassen (glCullFace, glFrontFace). Zeichnet bildschirmfuellend (ausserhalb des Desktops) und liest
 * die Pixel zurueck. Exit-Code 0 = alles in Ordnung, sonst die Nummer der ersten fehlgeschlagenen Pruefung. */

static int failed, checks, W, H;

static void check(int number, int ok)
{
    checks++;
    if (!ok && !failed) {
        failed = number;
        fprintf(2, "[gltest] Pruefung %d fehlgeschlagen\n", number);
    }
}

static u32 px(int x, int y)
{
    return gfx_screen.px[y * W + x] & 0xFFFFFF;
}

/* Pixel, die nicht schwarz sind */
static int lit(void)
{
    int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            n += px(x, y) != 0;
    return n;
}

static u32 checksum(void)
{
    u32 h = 2166136261u;
    for (int i = 0; i < W * H; i++)
        h = (h ^ (gfx_screen.px[i] & 0xFFFFFF)) * 16777619u;
    return h;
}

/* Bild loeschen, Kamera im Ursprung (Blick nach -z), nahe Ebene 1, ferne 10 */
static void frame(void)
{
    glDisable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60, (double)W / H, 1, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor3f(1, 0, 0);
}

static void tri(const float a[3], const float b[3], const float c[3])
{
    glBegin(GL_TRIANGLES);
    glVertex3fv(a);
    glVertex3fv(b);
    glVertex3fv(c);
    glEnd();
    glFinish();
}

static void cube(void)
{
    static const float v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                  {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    static const int f[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}};
    static const float col[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1}, {0, 1, 1}};
    glBegin(GL_QUADS);
    for (int i = 0; i < 6; i++) {
        glColor3f(col[i][0], col[i][1], col[i][2]);
        for (int k = 0; k < 4; k++)
            glVertex3fv(v[f[i][k]]);
    }
    glEnd();
    glFinish();
}

void _start(void)
{
    gl_force_cpu(1);
    if (gl_open(640, 480, "gltest") != 0)
        sys_exit(100);
    W = gl_width();
    H = gl_height();
    glClearColor(0, 0, 0, 1);

    /* 1. Boden-Dreieck mit einer Ecke hinter der Kamera: der sichtbare Teil erscheint unten (frueher fiel es weg),
     * ueber dem Horizont und oben bleibt alles schwarz (keine Ausreisser durch W <= 0) */
    static const float fa[3] = {-1, -1, -3}, fb[3] = {1, -1, -3}, fc[3] = {0, -1, 2};
    frame();
    tri(fa, fb, fc);
    int top_black = 1;
    for (int x = 0; x < W; x++)
        top_black &= px(x, 0) == 0 && px(x, H / 2) == 0;
    check(1, px(W / 2, H - 2) == 0xFF0000 && px(W / 2, H * 9 / 10) == 0xFF0000);
    check(2, top_black);

    /* 2. ganz hinter der Kamera: nichts */
    static const float ba[3] = {-1, -1, 2}, bb[3] = {1, -1, 2}, bc[3] = {0, 1, 2};
    frame();
    tri(ba, bb, bc);
    check(3, lit() == 0);

    /* 3. hinter der fernen Ebene (ohne Tiefentest): nichts; davor: sichtbar */
    static const float za[3] = {-20, -20, -20}, zb[3] = {20, -20, -20}, zc[3] = {0, 20, -20};
    frame();
    tri(za, zb, zc);
    check(4, lit() == 0);
    static const float na[3] = {-5, -5, -5}, nb[3] = {5, -5, -5}, nc[3] = {0, 5, -5};
    frame();
    tri(na, nb, nc);
    check(5, px(W / 2, H / 2) == 0xFF0000);

    /* 4. riesiges Dreieck weit ueber den Schutzstreifen: wird abgeschnitten, fuellt den ganzen Bildschirm */
    static const float ga[3] = {-1000, -1000, -2}, gb[3] = {1000, -1000, -2}, gc[3] = {0, 1000, -2};
    frame();
    tri(ga, gb, gc);
    check(6, lit() == W * H);

    /* 5. Rueckseiten: gegen den Uhrzeigersinn (von vorn gesehen) ist vorn */
    static const float ca[3] = {-1, -1, -3}, cb[3] = {1, -1, -3}, cc[3] = {0, 1, -3};
    frame();
    glEnable(GL_CULL_FACE);
    tri(ca, cb, cc);
    check(7, px(W / 2, H / 2) == 0xFF0000);        /* vorn, Rueckseiten weg: sichtbar */
    frame();
    glEnable(GL_CULL_FACE);
    tri(ca, cc, cb);
    check(8, lit() == 0);                           /* im Uhrzeigersinn: Rueckseite, weg */
    frame();
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    tri(ca, cb, cc);
    int front_gone = lit() == 0;
    frame();
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    tri(ca, cc, cb);
    check(9, front_gone && px(W / 2, H / 2) == 0xFF0000);
    frame();
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CW);
    tri(ca, cb, cc);
    int cw_gone = lit() == 0;
    frame();
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CW);
    tri(ca, cc, cb);
    check(10, cw_gone && px(W / 2, H / 2) == 0xFF0000);
    frame();
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT_AND_BACK);
    tri(ca, cb, cc);
    tri(ca, cc, cb);
    check(11, lit() == 0);
    frame();
    glEnable(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);
    tri(ca, cc, cb);
    check(12, px(W / 2, H / 2) == 0xFF0000);       /* wieder aus: beide Seiten */

    /* 6. abgeschnittenes Dreieck behaelt seinen Umlaufsinn: der Boden von oben gesehen (vorn) bleibt, verkehrt herum
     * faellt er weg */
    frame();
    glEnable(GL_CULL_FACE);
    tri(fb, fa, fc);
    int floor_front = px(W / 2, H - 2) == 0xFF0000;
    frame();
    glEnable(GL_CULL_FACE);
    tri(fa, fb, fc);
    check(13, floor_front && lit() == 0);

    /* 7. gedrehter Wuerfel mit Tiefentest: mit Rueckseiten weglassen genau dasselbe Bild */
    u32 sum[2];
    int npx = 0;
    for (int c = 0; c < 2; c++) {
        frame();
        glEnable(GL_DEPTH_TEST);
        if (c)
            glEnable(GL_CULL_FACE);
        glTranslatef(0, 0, -5);
        glRotatef(30, 1, 1, 0);
        glRotatef(20, 0, 1, 0);
        cube();
        sum[c] = checksum();
        npx = lit();
    }
    check(14, sum[0] == sum[1] && npx > 1000);

    /* 8. Kamera im Wuerfel: ohne Culling die Innenseiten ueberall (an der nahen Ebene abgeschnitten), mit Culling
     * nichts (alle Seiten zeigen nach aussen) */
    frame();
    glEnable(GL_DEPTH_TEST);
    glScalef(3, 3, 3);
    cube();
    int inside = lit() == W * H;
    frame();
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glScalef(3, 3, 3);
    cube();
    check(15, inside && lit() == 0);

    gl_close();
    printf("gltest: %d Pruefungen, %s\n", checks, failed ? "FEHLER" : "alle in Ordnung");
    sys_exit(failed);
}
