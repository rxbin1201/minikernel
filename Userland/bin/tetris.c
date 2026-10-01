#include "gfx.h"
#include "sound.h"
#include "ttf.h"

/* tetris: Pfeil links/rechts bewegen, Pfeil hoch drehen, Pfeil runter schneller, Leertaste fallen lassen,
 * p Pause, Esc beenden. Punkte fuer volle Reihen; alle 10 Reihen wird es schneller. Mit Soundkarte: Effekte.
 * Unter dem Desktop im eigenen Fenster (verliert es den Fokus, pausiert das Spiel), sonst im Vollbild. */

#define COLS 10
#define ROWS 20

static int board[ROWS][COLS];
static int cell, ox, oy, ui, side; /* side: Breite der Spalte rechts mit Punkten und naechstem Stein */
static int piece, rot, px, py, next_piece, score, lines, level, over, paused;
static u64 rng = 0x9E3779B97F4A7C15ULL;

static const u32 colors[8] = {0, 0x22D3EE, 0xFACC15, 0xA855F7, 0x4ADE80, 0xF87171, 0x60A5FA, 0xFB923C};

#define C_BG    0x0F172A
#define C_BOARD 0x0B1222
#define C_GRID  0x131D33
#define C_TEXT  0xE5E7EB
#define C_TEXT2 0x94A3B8

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

/* Stein: abgerundet, oben heller (Glanz), feiner heller Rand */
static void block(int x, int y, int size, u32 c)
{
    int in = size / 14 + 1, r = size / 5;
    gfx_round_rect_grad(&gfx_screen, x + in, y + in, size - 2 * in, size - 2 * in, r, gfx_mix(c, 0xFFFFFF, 70), c, 255);
    gfx_round_frame(&gfx_screen, x + in, y + in, size - 2 * in, size - 2 * in, r, 0xFFFFFF, 45);
}

static int S(int v) { return v * ui / 100; }

static void panel(Surface *s, int x, int y, int w, int h)
{
    gfx_round_rect(s, x, y, w, h, S(12), 0x16213A, 255);
    gfx_round_frame(s, x, y, w, h, S(12), 0xFFFFFF, 18);
}

static void stat(Surface *s, int x, int y, const char *label, int value)
{
    char t[32];
    snprintf(t, sizeof(t), "%d", value);
    text_draw(s, font_ui, S(13), x, y, label, C_TEXT2);
    text_draw(s, font_bold, S(22), x, y + S(17), t, C_TEXT);
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    gfx_fill(s, 0, 0, s->w, s->h, C_BG);
    int bw = COLS * cell, bh = ROWS * cell;
    gfx_round_rect(s, ox - S(6), oy - S(6), bw + S(12), bh + S(12), S(14), 0x1E293B, 255);
    gfx_fill(s, ox, oy, bw, bh, C_BOARD);
    for (int x = 1; x < COLS; x++) /* feines Raster */
        gfx_fill(s, ox + x * cell, oy, 1, bh, C_GRID);
    for (int y = 1; y < ROWS; y++)
        gfx_fill(s, ox, oy + y * cell, bw, 1, C_GRID);
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            if (board[y][x])
                block(ox + x * cell, oy + y * cell, cell, colors[board[y][x]]);
    if (!over) {
        int gy = py; /* Schatten: wo der Stein landen wuerde */
        while (fits(piece, rot, px, gy + 1))
            gy++;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (filled(piece, rot, x, y) && gy + y >= 0) {
                    int bx = ox + (px + x) * cell, by = oy + (gy + y) * cell, in = cell / 14 + 1;
                    gfx_round_rect(s, bx + in, by + in, cell - 2 * in, cell - 2 * in, cell / 5, colors[piece + 1], 40);
                    gfx_round_frame(s, bx + in, by + in, cell - 2 * in, cell - 2 * in, cell / 5, colors[piece + 1], 110);
                }
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (filled(piece, rot, x, y) && py + y >= 0)
                    block(ox + (px + x) * cell, oy + (py + y) * cell, cell, colors[piece + 1]);
    }

    /* rechte Spalte */
    int ix = ox + bw + S(26), iw = side;
    text_draw(s, font_bold, S(26), ix, oy - S(4), "Tetris", C_TEXT);
    int y = oy + S(44);
    panel(s, ix, y, iw, S(186));
    stat(s, ix + S(16), y + S(14), "Punkte", score);
    stat(s, ix + S(16), y + S(70), "Reihen", lines);
    stat(s, ix + S(16), y + S(126), "Level", level + 1);
    y += S(202);
    int nc = cell * 3 / 4; /* naechster Stein etwas kleiner, mittig im Kasten */
    panel(s, ix, y, iw, S(30) + nc * 3);
    text_draw(s, font_ui, S(13), ix + S(16), y + S(10), "N\xC3\xA4" "chster", C_TEXT2);
    int minx = 4, maxx = -1, miny = 4, maxy = -1;
    for (int yy = 0; yy < 4; yy++)
        for (int xx = 0; xx < 4; xx++)
            if (filled(next_piece, 0, xx, yy)) {
                if (xx < minx) minx = xx;
                if (xx > maxx) maxx = xx;
                if (yy < miny) miny = yy;
                if (yy > maxy) maxy = yy;
            }
    int pw = (maxx - minx + 1) * nc, ph = (maxy - miny + 1) * nc;
    int nx0 = ix + (iw - pw) / 2, ny0 = y + S(28) + (nc * 3 - ph) / 2;
    for (int yy = 0; yy < 4; yy++)
        for (int xx = 0; xx < 4; xx++)
            if (filled(next_piece, 0, xx, yy))
                block(nx0 + (xx - minx) * nc, ny0 + (yy - miny) * nc, nc, colors[next_piece + 1]);
    y += S(46) + nc * 3;
    static const char *const help[] = {"\xE2\x86\x90 \xE2\x86\x92   bewegen", "\xE2\x86\x91        drehen",
                                       "\xE2\x86\x93        schneller", "Leertaste  fallen",
                                       "P   Pause  \xC2\xB7  Esc   Ende"};
    for (int i = 0; i < 5 && y + S(20) <= oy + bh; i++, y += S(20))
        text_draw(s, font_ui, S(13), ix + S(4), y, help[i], 0x64748B);

    if (over || paused) {
        const char *m = over ? "Game Over" : "Pause";
        const char *m2 = over ? "Leertaste: neues Spiel" : "P: weiter";
        int big = S(28), small = S(14);
        int pw2 = text_width(font_ui, small, m2) + S(48);
        if (pw2 < text_width(font_bold, big, m) + S(48))
            pw2 = text_width(font_bold, big, m) + S(48);
        int ph2 = text_height(font_bold, big) + text_height(font_ui, small) + S(34);
        int px2 = ox + (bw - pw2) / 2, py2 = oy + (bh - ph2) / 2;
        gfx_shadow(s, px2, py2 + 4, pw2, ph2, S(14), S(18), 90);
        gfx_round_rect(s, px2, py2, pw2, ph2, S(14), 0x020617, 215);
        text_draw(s, font_bold, big, px2 + (pw2 - text_width(font_bold, big, m)) / 2, py2 + S(12), m,
                  over ? 0xFCA5A5 : C_TEXT);
        text_draw(s, font_ui, small, px2 + (pw2 - text_width(font_ui, small, m2)) / 2,
                  py2 + S(16) + text_height(font_bold, big), m2, C_TEXT2);
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
    /* Fenster: Feld mit 30 Pixel grossen Steinen, rechts die Spalte; Vollbild: so gross, wie die Hoehe zulaesst */
    if (gfx_desktop()) {
        ui = gfx_ui_scale();
        cell = S(30);
        side = S(170);
        if (gfx_open_window(S(28) + COLS * cell + S(26) + side + S(28), ROWS * cell + S(56), "Tetris") != 0)
            sys_exit(1);
    } else {
        if (gfx_open() != 0)
            sys_exit(1);
        ui = gfx_ui_scale();
        cell = (gfx_screen.h - S(80)) / ROWS;
        if (cell > S(40))
            cell = S(40);
        side = S(170);
    }
    fonts_init();
    sys_tty_fg(0);
    gfx_show_cursor(0);
    snd_open();
    rng ^= (u64)sys_ticks() * 2654435761ULL + (u64)sys_time();
    ox = (gfx_screen.w - (COLS * cell + S(26) + side)) / 2;
    if (ox < S(20))
        ox = S(20);
    oy = (gfx_screen.h - ROWS * cell) / 2;
    reset();
    draw();
    s64 last = sys_ticks();
    for (;;) {
        Event e;
        int changed = 0;
        while (gfx_poll(&e)) {
            if (e.type == EV_CLOSE) {
                gfx_close();
                sys_exit(0);
            }
            if (e.type == EV_FOCUS && !e.key && !over && !paused) { /* Fenster nicht mehr vorn: anhalten */
                paused = 1;
                changed = 1;
            }
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
        gfx_vsync(); /* im Takt der Anzeige */
    }
}
