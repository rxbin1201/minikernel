#include "gfx.h"

/* snake: Pfeiltasten lenken, Futter macht laenger und schneller. Leertaste = Pause/neues Spiel, Esc beendet. */

#define CELL 20
#define MAXLEN 4096

static int gw, gh, ox, oy;       /* Spielfeld in Zellen, Lage auf dem Bildschirm */
static int sx[MAXLEN], sy[MAXLEN], len, dir, next_dir, fx, fy, score, best, dead, paused;
static u64 rng = 88172645463325252ULL;

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
    place_food();
}

static void cell(int x, int y, u32 c, int inset)
{
    gfx_fill(&gfx_screen, ox + x * CELL + inset, oy + y * CELL + inset, CELL - 2 * inset, CELL - 2 * inset, c);
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    gfx_fill(s, 0, 0, s->w, s->h, RGB(20, 24, 30));
    gfx_fill(s, ox - 4, oy - 4, gw * CELL + 8, gh * CELL + 8, RGB(70, 80, 95));
    gfx_fill(s, ox, oy, gw * CELL, gh * CELL, RGB(12, 40, 20));
    cell(fx, fy, RGB(230, 50, 50), 3);
    for (int i = len - 1; i >= 0; i--)
        cell(sx[i], sy[i], i == 0 ? RGB(180, 255, 120) : RGB(60, 200, 80), 1);
    char t[120];
    snprintf(t, sizeof(t), "Snake   Punkte: %d   Rekord: %d   (Pfeile lenken, Leertaste Pause, Esc Ende)", score, best);
    gfx_text(s, ox, 8, t, RGB(230, 230, 230), GFX_TRANSPARENT);
    if (dead || paused) {
        const char *m = dead ? "Game Over - Leertaste: neues Spiel" : "Pause - Leertaste: weiter";
        int w = gfx_text_width(m) * 2;
        gfx_fill(s, (s->w - w) / 2 - 10, s->h / 2 - 22, w + 20, 44, RGB(0, 0, 0));
        gfx_text_scaled(s, (s->w - w) / 2, s->h / 2 - 16, m, RGB(255, 220, 80), GFX_TRANSPARENT, 2);
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
    }
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (gfx_open() != 0)
        sys_exit(1);
    sys_tty_fg(0);
    gfx_show_cursor(0);
    rng ^= (u64)sys_ticks() * 2654435761ULL + (u64)sys_time();
    gw = (gfx_screen.w - 40) / CELL;
    gh = (gfx_screen.h - 60) / CELL;
    if (gw > 60) gw = 60;
    if (gh > 40) gh = 40;
    ox = (gfx_screen.w - gw * CELL) / 2;
    oy = 40 + (gfx_screen.h - 40 - gh * CELL) / 2;
    reset();
    draw();
    s64 last = sys_ticks();
    for (;;) {
        Event e;
        while (gfx_poll(&e)) {
            if (e.type != EV_KEY)
                continue;
            if (e.key == 0x1B || e.key == 'q') {
                gfx_close();
                sys_exit(0);
            }
            if (e.key == ' ') {
                if (dead)
                    reset();
                else
                    paused = !paused;
                draw();
            }
            int nd = e.key == KEY_RIGHT ? 0 : e.key == KEY_DOWN ? 1 : e.key == KEY_LEFT ? 2 : e.key == KEY_UP ? 3 : -1;
            if (nd >= 0 && (nd + 2) % 4 != dir) /* nicht direkt umkehren */
                next_dir = nd;
        }
        int speed = 12 - score / 50; /* Ticks (10 ms) pro Schritt */
        if (speed < 4)
            speed = 4;
        if (!dead && !paused && sys_ticks() - last >= speed) {
            last = sys_ticks();
            step();
            draw();
        }
        sys_sleep_ms(10);
    }
}
