#include "gfx.h"
#include "sound.h"

/* tetris: Pfeil links/rechts bewegen, Pfeil hoch drehen, Pfeil runter schneller, Leertaste fallen lassen,
 * p Pause, Esc beenden. Punkte fuer volle Reihen; alle 10 Reihen wird es schneller. Mit Soundkarte: Effekte. */

#define COLS 10
#define ROWS 20

static int board[ROWS][COLS];
static int cell, ox, oy;
static int piece, rot, px, py, next_piece, score, lines, level, over, paused;
static u64 rng = 0x9E3779B97F4A7C15ULL;

static const u32 colors[8] = {0, RGB(0, 220, 230), RGB(240, 220, 0), RGB(170, 60, 230), RGB(40, 200, 60),
                              RGB(230, 50, 50), RGB(40, 90, 240), RGB(250, 150, 20)};

/* 7 Steine, je 4 Drehungen als 4x4-Bitmuster (Bit 15 = oben links) */
static const unsigned short shapes[7][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444}, /* I */
    {0x6600, 0x6600, 0x6600, 0x6600}, /* O */
    {0x4E00, 0x4640, 0x0E40, 0x4C40}, /* T */
    {0x6C00, 0x4620, 0x06C0, 0x8C40}, /* S */
    {0xC600, 0x2640, 0x0C60, 0x4C80}, /* Z */
    {0x8E00, 0x6440, 0x0E20, 0x44C0}, /* J */
    {0x2E00, 0x4460, 0x0E80, 0xC440}, /* L */
};

static int rnd7(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (int)(rng % 7);
}

static int filled(int p, int r, int x, int y) { return (shapes[p][r] >> (15 - (y * 4 + x))) & 1; }

static int fits(int p, int r, int nx, int ny)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!filled(p, r, x, y))
                continue;
            int bx = nx + x, by = ny + y;
            if (bx < 0 || bx >= COLS || by >= ROWS)
                return 0;
            if (by >= 0 && board[by][bx])
                return 0;
        }
    return 1;
}

static void spawn(void)
{
    piece = next_piece;
    next_piece = rnd7();
    rot = 0;
    px = 3;
    py = -1;
    if (!fits(piece, rot, px, py)) {
        over = 1;
        static const int down[4] = {392, 330, 262, 196}; /* abwaerts: Game Over */
        for (int i = 0; i < 4; i++)
            snd_tone(down[i], i == 3 ? 300 : 130, 55);
    }
}

static void reset(void)
{
    memset(board, 0, sizeof(board));
    score = lines = level = 0;
    over = paused = 0;
    next_piece = rnd7();
    spawn();
}

static void block(int x, int y, u32 c)
{
    gfx_fill(&gfx_screen, x + 1, y + 1, cell - 2, cell - 2, c);
    gfx_fill(&gfx_screen, x + 1, y + 1, cell - 2, 3, (c | 0x404040) & 0xFFFFFF); /* Glanzkante */
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    gfx_fill(s, 0, 0, s->w, s->h, RGB(18, 18, 30));
    gfx_fill(s, ox - 4, oy - 4, COLS * cell + 8, ROWS * cell + 8, RGB(90, 90, 120));
    gfx_fill(s, ox, oy, COLS * cell, ROWS * cell, RGB(8, 8, 16));
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            if (board[y][x])
                block(ox + x * cell, oy + y * cell, colors[board[y][x]]);
    if (!over) {
        int gy = py; /* Schatten: wo der Stein landen wuerde */
        while (fits(piece, rot, px, gy + 1))
            gy++;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (filled(piece, rot, x, y) && gy + y >= 0)
                    gfx_rect(s, ox + (px + x) * cell + 2, oy + (gy + y) * cell + 2, cell - 4, cell - 4, RGB(90, 90, 110));
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (filled(piece, rot, x, y) && py + y >= 0)
                    block(ox + (px + x) * cell, oy + (py + y) * cell, colors[piece + 1]);
    }
    int ix = ox + COLS * cell + 30;
    char t[64];
    gfx_text_scaled(s, ix, oy, "TETRIS", RGB(255, 220, 80), GFX_TRANSPARENT, 2);
    snprintf(t, sizeof(t), "Punkte: %d", score);
    gfx_text(s, ix, oy + 50, t, RGB(230, 230, 230), GFX_TRANSPARENT);
    snprintf(t, sizeof(t), "Reihen: %d", lines);
    gfx_text(s, ix, oy + 70, t, RGB(230, 230, 230), GFX_TRANSPARENT);
    snprintf(t, sizeof(t), "Level:  %d", level + 1);
    gfx_text(s, ix, oy + 90, t, RGB(230, 230, 230), GFX_TRANSPARENT);
    gfx_text(s, ix, oy + 130, "N\xC3\xA4" "chster:", RGB(180, 180, 200), GFX_TRANSPARENT);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (filled(next_piece, 0, x, y))
                block(ix + x * cell, oy + 150 + y * cell, colors[next_piece + 1]);
    gfx_text(s, ix, oy + 250, "\xE2\x86\x90 \xE2\x86\x92  bewegen", RGB(150, 150, 170), GFX_TRANSPARENT);
    gfx_text(s, ix, oy + 268, "\xE2\x86\x91     drehen", RGB(150, 150, 170), GFX_TRANSPARENT);
    gfx_text(s, ix, oy + 286, "\xE2\x86\x93     schneller", RGB(150, 150, 170), GFX_TRANSPARENT);
    gfx_text(s, ix, oy + 304, "Leer  fallen lassen", RGB(150, 150, 170), GFX_TRANSPARENT);
    gfx_text(s, ix, oy + 322, "p     Pause, Esc Ende", RGB(150, 150, 170), GFX_TRANSPARENT);
    if (over || paused) {
        const char *m = over ? "Game Over - Leertaste" : "Pause";
        int w = gfx_text_width(m) * 2;
        gfx_fill(s, ox + (COLS * cell - w) / 2 - 8, oy + ROWS * cell / 2 - 20, w + 16, 40, RGB(0, 0, 0));
        gfx_text_scaled(s, ox + (COLS * cell - w) / 2, oy + ROWS * cell / 2 - 16, m, RGB(255, 220, 80), GFX_TRANSPARENT, 2);
    }
    gfx_present_all();
}

static void lock_piece(void)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (filled(piece, rot, x, y) && py + y >= 0)
                board[py + y][px + x] = piece + 1;
    int cleared = 0;
    for (int y = ROWS - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < COLS; x++)
            if (!board[y][x])
                full = 0;
        if (full) {
            for (int yy = y; yy > 0; yy--)
                memcpy(board[yy], board[yy - 1], sizeof(board[0]));
            memset(board[0], 0, sizeof(board[0]));
            cleared++;
            y++; /* dieselbe Zeile noch einmal pruefen */
        }
    }
    if (cleared) { /* aufsteigend; vier Reihen auf einmal mit Schlusston */
        static const int up[4] = {523, 659, 784, 1047};
        for (int i = 0; i < (cleared == 4 ? 4 : 3); i++)
            snd_tone(up[i], cleared == 4 ? 90 : 55, 55);
    } else {
        snd_tone(220, 30, 40); /* Stein liegt */
    }
    static const int pts[5] = {0, 100, 300, 500, 800};
    score += pts[cleared] * (level + 1);
    lines += cleared;
    level = lines / 10;
    spawn();
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (gfx_open() != 0)
        sys_exit(1);
    sys_tty_fg(0);
    gfx_show_cursor(0);
    snd_open();
    rng ^= (u64)sys_ticks() * 2654435761ULL + (u64)sys_time();
    cell = (gfx_screen.h - 60) / ROWS;
    if (cell > 36)
        cell = 36;
    ox = (gfx_screen.w - COLS * cell) / 2 - 90;
    if (ox < 10)
        ox = 10;
    oy = (gfx_screen.h - ROWS * cell) / 2;
    reset();
    draw();
    s64 last = sys_ticks();
    for (;;) {
        Event e;
        int changed = 0;
        while (gfx_poll(&e)) {
            if (e.type != EV_KEY)
                continue;
            int k = e.key;
            if (k == 0x1B || k == 'q') {
                gfx_close();
                sys_exit(0);
            }
            if (over) {
                if (k == ' ') {
                    reset();
                    changed = 1;
                }
                continue;
            }
            if (k == 'p') {
                paused = !paused;
                changed = 1;
                continue;
            }
            if (paused)
                continue;
            if (k == KEY_LEFT && fits(piece, rot, px - 1, py)) px--;
            else if (k == KEY_RIGHT && fits(piece, rot, px + 1, py)) px++;
            else if (k == KEY_UP) {
                int nr = (rot + 1) % 4;
                for (int kick = 0; kick < 3; kick++) { /* an der Wand etwas verschieben, falls noetig */
                    static const int kicks[3] = {0, -1, 1};
                    if (fits(piece, nr, px + kicks[kick], py)) {
                        rot = nr;
                        px += kicks[kick];
                        snd_tone(1400, 12, 20);
                        break;
                    }
                }
            } else if (k == KEY_DOWN) {
                if (fits(piece, rot, px, py + 1)) {
                    py++;
                    score += 1;
                }
            } else if (k == ' ') {
                while (fits(piece, rot, px, py + 1)) {
                    py++;
                    score += 2;
                }
                lock_piece();
                last = sys_ticks();
            }
            changed = 1;
        }
        int delay = 50 - level * 4; /* Ticks (10 ms) pro Fallschritt */
        if (delay < 6)
            delay = 6;
        if (!over && !paused && sys_ticks() - last >= delay) {
            last = sys_ticks();
            if (fits(piece, rot, px, py + 1))
                py++;
            else
                lock_piece();
            changed = 1;
        }
        if (changed)
            draw();
        sys_sleep_ms(10);
    }
}
