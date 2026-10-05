/* Startanimation: solange der Kernel startet (ohne "verbose" in der Kommandozeile), steht statt der Meldungen das
 * Logo des Desktops (abgerundetes Quadrat mit Farbverlauf und weissem Punkt) auf dunklem Grund, darunter ein Kreis
 * aus zehn Punkten, durch den ein heller Punkt laeuft. Nur Ganzzahlen (der Kernel rechnet ohne FPU); Kanten werden
 * mit 4 x 4 Abtastpunkten je Pixel geglaettet (Koordinaten in 1/8 Pixel). */

#include "console/splash.h"

#define BG    0x0B0D14u
#define TOP   0x6A7CFFu /* Farbverlauf des Logos (wie draw_logo im Desktop) */
#define BOT   0xC04BD6u
#define NDOTS 10

/* Punkte auf dem Kreis: sin/cos in 1/1000, beginnend oben, im Uhrzeigersinn */
static const int dot_sin[NDOTS] = {0, 588, 951, 951, 588, 0, -588, -951, -951, -588};
static const int dot_cos[NDOTS] = {1000, 809, 309, -309, -809, -1000, -809, -309, 309, 809};

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t) /* t: 0 = a, 256 = b */
{
    uint32_t r = 0;
    for (int s = 0; s < 24; s += 8) {
        uint32_t ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        r |= ((ca * (256 - t) + cb * t) >> 8) << s;
    }
    return r;
}

typedef struct {
    int S, lx, ly;          /* Logo: Groesse, links oben */
    int cx, cy, R, rd;      /* Punktkreis: Mitte, Radius, Radius eines Punkts */
} Layout;

static void layout(uint32_t w, uint32_t h, Layout *L)
{
    int S = (int)(h < w ? h : w) / 6;
    if (S < 48)
        S = 48;
    L->S = S;
    L->lx = ((int)w - S) / 2;
    L->ly = (int)h / 2 - S * 3 / 4;
    L->cx = (int)w / 2;
    L->cy = L->ly + S + S * 3 / 4;
    L->R = S * 22 / 100;
    L->rd = S * 4 / 100 < 2 ? 2 : S * 4 / 100;
}

/* Deckung 0-16 des abgerundeten Rechtecks (x, y, s, s, Radius r) im Pixel (px, py) */
static int rrect_cov(int px, int py, int x, int y, int s, int r)
{
    int n = 0, x0 = (x + r) * 8, x1 = (x + s - r) * 8, y0 = (y + r) * 8, y1 = (y + s - r) * 8, rr = r * 8 * r * 8;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int X = px * 8 + sx * 2 + 1, Y = py * 8 + sy * 2 + 1;
            if (X < x * 8 || X >= (x + s) * 8 || Y < y * 8 || Y >= (y + s) * 8)
                continue;
            int dx = X < x0 ? x0 - X : X > x1 ? X - x1 : 0, dy = Y < y0 ? y0 - Y : Y > y1 ? Y - y1 : 0;
            n += dx * dx + dy * dy <= rr;
        }
    return n;
}

/* Deckung 0-16 einer Kreisscheibe (Mitte und Radius in 1/8 Pixel) */
static int disc_cov(int px, int py, int cx8, int cy8, int r8)
{
    int n = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int dx = px * 8 + sx * 2 + 1 - cx8, dy = py * 8 + sy * 2 + 1 - cy8;
            n += dx * dx + dy * dy <= r8 * r8;
        }
    return n;
}

static void put(volatile uint32_t *fb, uint32_t pitch, uint32_t w, uint32_t h, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && (uint32_t)x < w && (uint32_t)y < h)
        fb[(uint64_t)y * pitch + (uint64_t)x] = c;
}

void splash_tick(volatile uint32_t *fb, uint32_t pitch, uint32_t w, uint32_t h, uint64_t ms)
{
    Layout L;
    layout(w, h, &L);
    int head = (int)((ms / 90) % NDOTS), m = L.R + L.rd + 2;
    for (int y = L.cy - m; y <= L.cy + m; y++)
        for (int x = L.cx - m; x <= L.cx + m; x++) {
            uint32_t c = BG;
            for (int i = 0; i < NDOTS; i++) {
                int dx8 = L.cx * 8 + L.R * 8 * dot_sin[i] / 1000, dy8 = L.cy * 8 - L.R * 8 * dot_cos[i] / 1000;
                if (x * 8 < dx8 - L.rd * 8 - 8 || x * 8 > dx8 + L.rd * 8 + 8 || y * 8 < dy8 - L.rd * 8 - 8 ||
                    y * 8 > dy8 + L.rd * 8 + 8)
                    continue;
                int cov = disc_cov(x, y, dx8, dy8, L.rd * 8);
                if (!cov)
                    continue;
                int age = (head - i + NDOTS) % NDOTS; /* 0 = der helle Punkt, dahinter verblassend */
                uint32_t bright = (uint32_t)(240 - age * 20);
                c = mix(c, mix(BG, 0xFFFFFF, bright), (uint32_t)cov * 16);
            }
            put(fb, pitch, w, h, x, y, c);
        }
}

void splash_draw(volatile uint32_t *fb, uint32_t pitch, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            fb[(uint64_t)y * pitch + x] = BG;
    Layout L;
    layout(w, h, &L);
    int S = L.S, r = S / 4, dot8 = S * 8 * 18 / 100;
    for (int y = L.ly; y < L.ly + S; y++)
        for (int x = L.lx; x < L.lx + S; x++) {
            int cov = rrect_cov(x, y, L.lx, L.ly, S, r);
            if (!cov)
                continue;
            uint32_t c = mix(TOP, BOT, (uint32_t)((y - L.ly) * 256 / S));
            int d = disc_cov(x, y, L.lx * 8 + S * 4, L.ly * 8 + S * 4, dot8);
            if (d)
                c = mix(c, 0xFFFFFF, (uint32_t)d * 15); /* weisser Punkt, leicht durchscheinend wie im Desktop */
            put(fb, pitch, w, h, x, y, mix(BG, c, (uint32_t)cov * 16));
        }
    splash_tick(fb, pitch, w, h, 0);
}
