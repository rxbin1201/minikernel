#include "gfx.h"
#include "gl.h"

/* Prueft das kleine OpenGL (gl.c) mit der CPU: Abschneiden an der nahen und fernen Ebene und am Schutzstreifen,
 * Rueckseiten weglassen (glCullFace, glFrontFace), Mischen (glBlendFunc, glDepthMask), Puffer und Vertex-Arrays
 * (glDrawArrays, glDrawElements: dasselbe Bild wie mit glBegin/glEnd). Zeichnet bildschirmfuellend (ausserhalb des Desktops) und liest
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

/* gleiche Farbe bis auf 2 je Kanal (Rundung) */
static int near(u32 a, u32 b)
{
    for (int c = 0; c < 24; c += 8) {
        int d = (int)(a >> c & 0xFF) - (int)(b >> c & 0xFF);
        if (d < -2 || d > 2)
            return 0;
    }
    return 1;
}

static u32 checksum(void)
{
    u32 h = 2166136261u;
    for (int i = 0; i < W * H; i++)
        h = (h ^ (gfx_screen.px[i] & 0xFFFFFF)) * 16777619u;
    return h;
}

/* Bild loeschen, Kamera im Ursprung (Blick nach -z), nahe Ebene 1, ferne 10 */
static void arrays_off(void)
{
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void frame(void)
{
    arrays_off();
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDepthMask(GL_TRUE);
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

/* derselbe Wuerfel als Arrays: 24 Ecken (Position, Farbe), 36 Indizes wie glBegin(GL_QUADS) sie zerlegt */
static float kv[24][3], kc[24][4];
static unsigned short ci16[36];
static unsigned ci32[36];
static unsigned char ci8[36];

static void cube_arrays(void)
{
    static const float v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                  {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    static const int f[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}};
    static const float col[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1}, {0, 1, 1}};
    for (int i = 0; i < 6; i++) {
        for (int k = 0; k < 4; k++) {
            memcpy(kv[i * 4 + k], v[f[i][k]], sizeof(kv[0]));
            memcpy(kc[i * 4 + k], col[i], sizeof(col[0]));
            kc[i * 4 + k][3] = 1;
        }
        static const int q[6] = {0, 1, 2, 0, 2, 3};
        for (int t = 0; t < 6; t++)
            ci16[i * 6 + t] = (unsigned short)(i * 4 + q[t]);
    }
    for (int i = 0; i < 36; i++) {
        ci32[i] = ci16[i];
        ci8[i] = (unsigned char)ci16[i];
    }
}

/* gedrehter Wuerfel mit Tiefentest; how: 0 glBegin, 1 Puffer + Indizes (16 Bit), 2 Arrays im Programmspeicher,
 * 3 Puffer + Indizes (8 Bit, im Programmspeicher), 4 Puffer + Indizes (32 Bit, Puffer) */
static GLuint vbo[3];

static u32 draw_cube(int how, float dist)
{
    frame();
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glTranslatef(0, 0, -dist);
    glRotatef(30, 1, 1, 0);
    glRotatef(20, 0, 1, 0);
    if (how == 0) {
        cube();
        return checksum();
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    if (how == 2) {
        glVertexPointer(3, GL_FLOAT, 0, kv);
        glColorPointer(4, GL_FLOAT, 0, kc);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, ci16);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
        glVertexPointer(3, GL_FLOAT, 0, 0);
        glColorPointer(4, GL_FLOAT, 0, (const void *)sizeof(kv));
        if (how == 3) {
            glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, ci8);
        } else {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo[how == 1 ? 1 : 2]);
            glDrawElements(GL_TRIANGLES, 36, how == 1 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, 0);
        }
    }
    glFinish();
    return checksum();
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

    /* 9. Mischen: Rechteck ueber die Mitte auf einem geloeschten Hintergrund. near(): Farbe in der Mitte bis auf 2 */
    static const float qa[3] = {-1, -1, -3}, qb[3] = {1, -1, -3}, qc[3] = {0, 1, -3};
    glClearColor(0, 0, 1, 1);
    frame();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 0, 0, 0.5f);
    tri(qa, qb, qc);
    u32 mid = gfx_screen.px[(H / 2) * W + W / 2];
    check(16, near(mid, 0x800080) && mid >> 24 == 0xFF && px(0, 0) == 0x0000FF); /* halb rot, halb blau, deckend */
    glClearColor(0, 0, 0, 1);
    frame();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);                    /* addieren: rot + gruen = gelb */
    glColor3f(1, 0, 0);
    tri(qa, qb, qc);
    glColor3f(0, 1, 0);
    tri(qa, qb, qc);
    check(17, px(W / 2, H / 2) == 0xFFFF00);
    glClearColor(0.5f, 0.5f, 0.5f, 1);
    frame();
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);             /* multiplizieren */
    glColor3f(1, 0.5f, 0);
    tri(qa, qb, qc);
    check(18, near(px(W / 2, H / 2), 0x804000));
    frame();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);                   /* wie ohne Mischen; Alpha bleibt 255 */
    glColor4f(0, 1, 0, 0.25f);
    tri(qa, qb, qc);
    mid = gfx_screen.px[(H / 2) * W + W / 2];
    check(19, mid == 0xFF00FF00u);
    frame();
    glColor4f(0, 1, 0, 0.25f);                      /* ohne Mischen genauso */
    tri(qa, qb, qc);
    mid = gfx_screen.px[(H / 2) * W + W / 2];
    check(20, mid == 0xFF00FF00u);
    glBlendFunc(GL_SRC_ALPHA, 0x1234);   /* ungueltig: Zustand bleibt (hier GL_ONE, GL_ZERO) */
    tri(qa, qb, qc);
    glClearColor(0, 0, 0, 1);

    /* 10. glDepthMask: ein vorderes Dreieck ohne Tiefe schreiben verdeckt ein spaeter gezeichnetes hinteres nicht */
    static const float fr[3][3] = {{-1, -1, -2}, {1, -1, -2}, {0, 1, -2}}, bk[3][3] = {{-2, -2, -4}, {2, -2, -4}, {0, 2, -4}};
    for (int mask = 0; mask < 2; mask++) {
        frame();
        glEnable(GL_DEPTH_TEST);
        glDepthMask(mask ? GL_TRUE : GL_FALSE);
        glColor3f(1, 0, 0);
        tri(fr[0], fr[1], fr[2]);
        glDepthMask(GL_TRUE);
        glColor3f(0, 1, 0);
        tri(bk[0], bk[1], bk[2]);
        check(21 + mask, px(W / 2, H / 2) == (mask ? 0xFF0000u : 0x00FF00u));
    }

    /* 11. Puffer und Vertex-Arrays: jedes Mal genau dasselbe Bild wie mit glBegin/glEnd */
    cube_arrays();
    glGenBuffers(3, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kv) + sizeof(kc), 0, GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(kv), kv);
    glBufferSubData(GL_ARRAY_BUFFER, sizeof(kv), sizeof(kc), kc);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(ci16), ci16, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo[2]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(ci32), ci32, GL_STATIC_DRAW);
    u32 ref = draw_cube(0, 5);
    int all = lit() > 1000;
    for (int how = 1; how <= 4; how++)
        all &= draw_cube(how, 5) == ref;
    check(23, all);
    /* zweimal hintereinander (Zwischenspeicher fuer Indizes und Kasten) und nah dran (Kasten ragt ueber die nahe
     * Ebene: Dreiecke werden zusammengesetzt und abgeschnitten) */
    check(24, draw_cube(1, 5) == ref && draw_cube(1, 1.5f) == draw_cube(0, 1.5f));
    /* hinter der Kamera: nichts */
    draw_cube(1, -5);
    check(25, lit() == 0);
    /* Puffer geaendert: alle Ecken nach (0, 0, 0) - Kasten wird neu berechnet, nichts mehr zu sehen */
    static float zero[24][3];
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(zero), zero);
    draw_cube(1, 5);
    int gone = lit() == 0;
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(kv), kv);
    check(26, gone && draw_cube(1, 5) == ref);

    /* 12. Streifen und Faecher mit glDrawArrays wie mit glBegin; Farben als 4 Byte */
    static const float sv[6][3] = {{-2, -1, -4}, {-2, 1, -4}, {0, -1, -4}, {0, 1, -4}, {2, -1, -4}, {2, 1, -4}};
    static const unsigned char sc[6][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
                                           {255, 255, 0, 255}, {0, 255, 255, 255}, {255, 0, 255, 255}};
    static const GLenum modes[2] = {GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
    all = 1;
    for (int m = 0; m < 2; m++) {
        frame();
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT_AND_BACK);
        glDisable(GL_CULL_FACE);
        glBegin(modes[m]);
        for (int i = 0; i < 6; i++) {
            glColor4ub(sc[i][0], sc[i][1], sc[i][2], sc[i][3]);
            glVertex3fv(sv[i]);
        }
        glEnd();
        glFinish();
        u32 a = checksum();
        int n = lit();
        frame();
        glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(sv) + sizeof(sc), 0, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(sv), sv);
        glBufferSubData(GL_ARRAY_BUFFER, sizeof(sv), sizeof(sc), sc);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, 0);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, (const void *)sizeof(sv));
        glDrawArrays(modes[m], 0, 6);
        glFinish();
        all &= n > 1000 && checksum() == a;
    }
    check(27, all);

    /* 13. Index hinter dem Pufferende: kein Absturz, die Ecke gilt als (0, 0, 0) */
    static const unsigned short bad[3] = {0, 1, 60000};
    frame();
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(sv), sv, GL_STATIC_DRAW);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, 0);
    glTranslatef(0, 0, -1);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, bad);
    glFinish();
    check(28, 1);
    glDeleteBuffers(3, vbo);
    arrays_off();

    gl_close();
    printf("gltest: %d Pruefungen, %s\n", checks, failed ? "FEHLER" : "alle in Ordnung");
    sys_exit(failed);
}
