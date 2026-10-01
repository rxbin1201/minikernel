#include "gfx.h"
#include "sound.h"
#include "ttf.h"

/* snake: Pfeiltasten lenken, Futter macht laenger und schneller. Leertaste = Pause/neues Spiel, Esc beendet.
 * Unter dem Desktop im eigenen Fenster (verliert es den Fokus, pausiert das Spiel), sonst im Vollbild.
 * Mit Soundkarte: Toene beim Fressen und beim Aufprall. */

#define MAXLEN 4096

static int cell, pad, hdr, foot;  /* Masse in Pixeln (mit dem Massstab der Oberflaeche) */
static int gw, gh, ox, oy;        /* Spielfeld in Zellen, Lage im Bild */
static int sx[MAXLEN], sy[MAXLEN], len, dir, next_dir, fx, fy, score, best, dead, paused;
static int ready; /* neues Spiel: wartet auf die erste Taste */
static u64 rng = 88172645463325252ULL;

#define C_BG     0x0F172A
#define C_BOARD  0x111C2E
#define C_BOARD2 0x14213A
#define C_TEXT   0xE5E7EB
#define C_TEXT2  0x94A3B8

static int rnd(int n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (int)(rng % (u64)n);
}

static int on_snake(int x, int y)
{
    for (int i = 0; i < len; i++)
        if (sx[i] == x && sy[i] == y)
            return 1;
    return 0;
}

static void place_food(void)
{
    do {
        fx = rnd(gw);
        fy = rnd(gh);
    } while (on_snake(fx, fy));
}

static void reset(void)
{
    len = 4;
    for (int i = 0; i < len; i++) {
        sx[i] = gw / 2 - i;
        sy[i] = gh / 2;
    }
    dir = next_dir = 0; /* 0 rechts, 1 unten, 2 links, 3 oben */
    score = 0;
    dead = 0;
    paused = 0;
    ready = 1;
    place_food();
}

static void draw_head(Surface *s, int x, int y)
{
    static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    float cx = x + cell * 0.5f, cy = y + cell * 0.5f, f = cell * 0.22f, side = cell * 0.2f;
    for (int k = -1; k <= 1; k += 2) { /* zwei Augen, nach vorn versetzt */
        float ex = cx + dx[dir] * f - dy[dir] * side * k, ey = cy + dy[dir] * f + dx[dir] * side * k;
        gfx_disc(s, ex, ey, cell * 0.14f, 0xFFFFFF, 255);
        gfx_disc(s, ex + dx[dir] * cell * 0.04f, ey + dy[dir] * cell * 0.04f, cell * 0.07f, 0x0F172A, 255);
    }
}

static void centered(Surface *s, Font *f, int size, int cx, int y, const char *t, u32 c)
{
    text_draw(s, f, size, cx - text_width(f, size, t) / 2, y, t, c);
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    int ui = gfx_ui_scale(), fs = 15 * ui / 100, fb = 17 * ui / 100;
    gfx_fill(s, 0, 0, s->w, s->h, C_BG);

    /* Kopfzeile: Name links, Punkte rechts */
    int ty = (hdr - text_height(font_bold, fb)) / 2;
    text_draw(s, font_bold, fb, ox, ty, "Snake", C_TEXT);
    char t[96];
    snprintf(t, sizeof(t), "Punkte %d   \xC2\xB7   Rekord %d", score, best);
    text_draw(s, font_ui, fs, ox + gw * cell - text_width(font_ui, fs, t), ty + (fb - fs) / 2, t, C_TEXT2);

    /* Spielfeld: abgerundet, leichtes Schachbrett */
    int bw = gw * cell, bh = gh * cell, r = cell / 2;
    gfx_round_rect(s, ox - 2, oy - 2, bw + 4, bh + 4, r + 2, 0x1E293B, 255);
    gfx_round_rect(s, ox, oy, bw, bh, r, C_BOARD, 255);
    gfx_set_clip(ox + r / 2, oy + r / 2, bw - r, bh - r);
    for (int y = 0; y < gh; y++)
        for (int x = (y & 1); x < gw; x += 2)
            gfx_fill(s, ox + x * cell, oy + y * cell, cell, cell, C_BOARD2);
    gfx_no_clip();

    /* Futter: Apfel mit Glanzpunkt und Blatt */
    float ax = ox + fx * cell + cell * 0.5f, ay = oy + fy * cell + cell * 0.55f;
    gfx_disc(s, ax, ay, cell * 0.36f, 0xEF4444, 255);
    gfx_disc(s, ax - cell * 0.12f, ay - cell * 0.12f, cell * 0.09f, 0xFCA5A5, 200);
    gfx_capsule(s, ax + cell * 0.02f, ay - cell * 0.34f, ax + cell * 0.2f, ay - cell * 0.46f, cell * 0.1f, 0x22C55E, 255);

    /* Schlange: vom Schwanz zum Kopf, Farbe wird zum Kopf hin heller; Glieder ueberlappen sich etwas */
    int in = cell / 10;
    for (int i = len - 1; i >= 0; i--) {
        int k = len > 1 ? i * 255 / (len - 1) : 0;
        u32 c = gfx_mix(0x4ADE80, 0x15803D, k);
        int x = ox + sx[i] * cell, y = oy + sy[i] * cell;
        gfx_round_rect_grad(s, x + in, y + in, cell - 2 * in, cell - 2 * in, cell / 3, gfx_mix(c, 0xFFFFFF, 40), c, 255);
        if (i > 0) { /* Verbindung zum vorderen Glied */
            int px = ox + sx[i - 1] * cell, py = oy + sy[i - 1] * cell;
            int mx = (x + px) / 2, my = (y + py) / 2;
            gfx_round_rect(s, mx + in, my + in, cell - 2 * in, cell - 2 * in, cell / 4, c, 255);
        }
    }
    draw_head(s, ox + sx[0] * cell, oy + sy[0] * cell);

    /* Fusszeile mit der Bedienung */
    centered(s, font_ui, 13 * ui / 100, ox + bw / 2, oy + bh + (foot - text_height(font_ui, 13 * ui / 100)) / 2,
             "Pfeile lenken  \xC2\xB7  Leertaste Pause  \xC2\xB7  Esc beendet", 0x64748B);

    if (dead || paused || ready) { /* Hinweis in der Mitte, halb durchsichtig */
        const char *m = dead ? "Game Over" : ready ? "Bereit?" : "Pause";
        const char *m2 = dead ? "Leertaste: neues Spiel" : ready ? "Pfeiltaste oder Leertaste: los" : "Leertaste: weiter";
        int big = 30 * ui / 100, small = 15 * ui / 100;
        int pw = text_width(font_ui, small, m2) + 60 * ui / 100, ph = text_height(font_bold, big) + text_height(font_ui, small) + 36 * ui / 100;
        int px = ox + (bw - pw) / 2, py = oy + (bh - ph) / 2;
        gfx_shadow(s, px, py + 4, pw, ph, 14 * ui / 100, 18 * ui / 100, 90);
        gfx_round_rect(s, px, py, pw, ph, 14 * ui / 100, 0x020617, 215);
        centered(s, font_bold, big, ox + bw / 2, py + 14 * ui / 100, m, dead ? 0xFCA5A5 : C_TEXT);
        centered(s, font_ui, small, ox + bw / 2, py + 18 * ui / 100 + text_height(font_bold, big), m2, C_TEXT2);
    }
    gfx_present_all();
}

static void step(void)
{
    static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    dir = next_dir;
    int nx = sx[0] + dx[dir], ny = sy[0] + dy[dir];
    if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || on_snake(nx, ny)) {
        dead = 1;
        if (score > best)
            best = score;
        for (int i = 0; i < 4; i++) /* Aufprall: abwaerts */
            snd_tone(330 - i * 50, i == 3 ? 250 : 90, 55);
        return;
    }
    int grow = nx == fx && ny == fy;
    if (grow && len < MAXLEN)
        len++;
    for (int i = len - 1; i > 0; i--) {
        sx[i] = sx[i - 1];
        sy[i] = sy[i - 1];
    }
    sx[0] = nx;
    sy[0] = ny;
    if (grow) {
        score += 10;
        place_food();
        snd_tone(880, 45, 45);
        snd_tone(1320, 60, 45);
    }
}

static void quit(void)
{
    gfx_close();
    sys_exit(0);
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* Fenster: 28 x 18 Zellen; im Vollbild so viele, wie passen (hoechstens 60 x 40) */
    int ui;
    if (gfx_desktop()) {
        ui = gfx_ui_scale();
        cell = 24 * ui / 100;
        pad = 18 * ui / 100;
        hdr = 46 * ui / 100;
        foot = 34 * ui / 100;
        gw = 28;
        gh = 18;
        if (gfx_open_window(gw * cell + 2 * pad, hdr + gh * cell + foot, "Snake") != 0)
            sys_exit(1);
    } else {
        if (gfx_open() != 0)
            sys_exit(1);
        ui = gfx_ui_scale();
        cell = 24 * ui / 100;
        pad = 20 * ui / 100;
        hdr = 50 * ui / 100;
        foot = 36 * ui / 100;
        gw = (gfx_screen.w - 2 * pad) / cell;
        gh = (gfx_screen.h - hdr - foot) / cell;
        if (gw > 60) gw = 60;
        if (gh > 40) gh = 40;
    }
    fonts_init();
    sys_tty_fg(0);
    gfx_show_cursor(0);
    snd_open();
    rng ^= (u64)sys_ticks() * 2654435761ULL + (u64)sys_time();
    ox = (gfx_screen.w - gw * cell) / 2;
    oy = hdr + (gfx_screen.h - hdr - foot - gh * cell) / 2;
    reset();
    draw();
    s64 last = sys_ticks();
    for (;;) {
        Event e;
        while (gfx_poll(&e)) {
            if (e.type == EV_CLOSE)
                quit();
            if (e.type == EV_FOCUS && !e.key && !dead && !paused && !ready) { /* Fenster nicht mehr vorn: anhalten */
                paused = 1;
                draw();
            }
            if (e.type != EV_KEY)
                continue;
            if (e.key == 0x1B || e.key == 'q')
                quit();
            if (e.key == ' ') {
                if (dead)
                    reset();
                else if (ready)
                    ready = 0;
                else
                    paused = !paused;
                last = sys_ticks();
                draw();
            }
            int nd = e.key == KEY_RIGHT ? 0 : e.key == KEY_DOWN ? 1 : e.key == KEY_LEFT ? 2 : e.key == KEY_UP ? 3 : -1;
            if (nd >= 0 && (nd + 2) % 4 != dir) /* nicht direkt umkehren */
                next_dir = nd;
            if (nd >= 0 && ready && !dead) { /* erste Pfeiltaste startet */
                ready = 0;
                paused = 0;
                last = sys_ticks();
                draw();
            }
        }
        int speed = 12 - score / 50; /* Ticks (10 ms) pro Schritt */
        if (speed < 4)
            speed = 4;
        if (!dead && !paused && !ready && sys_ticks() - last >= speed) {
            last = sys_ticks();
            step();
            draw();
        }
        gfx_vsync(); /* im Takt der Anzeige */
    }
}
