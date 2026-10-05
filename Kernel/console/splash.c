/* Startbild: solange der Kernel startet (ohne "verbose" in der Kommandozeile), bleibt der Bildschirm schwarz mit dem
 * Startlogo der Firmware an derselben Stelle wie vor dem Bootloader (ACPI-Tabelle BGRT, vom Bootloader kopiert - wie
 * bei Windows und Linux); darunter ein schmaler, abgerundeter Ladebalken wie bei macOS. Hat der Bootloader oder die
 * Intel-Grafik die Aufloesung gewechselt, steht das Logo an derselben relativen Stelle (bei mindestens doppelter
 * Groesse doppelt so gross). Ohne BGRT nur der Balken. Nur Ganzzahlen (der Kernel rechnet ohne FPU); die runden
 * Enden des Balkens werden mit 4 x 4 Abtastpunkten je Pixel geglaettet. */

#include "console/splash.h"

#define TRACK 0x3A3A3Cu /* Balken: Rinne und Fuellung */
#define FILL  0xE5E5EAu

static const uint8_t *bmp;
static uint64_t       bmp_size;
static uint32_t       logo_x, logo_y, logo_sw, logo_sh;
static int            bw, bh, bpp, row_bytes, top_down; /* Bild der BMP */
static uint32_t       pix_off;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void splash_logo(const void *data, uint64_t size, uint32_t x, uint32_t y, uint32_t scr_w, uint32_t scr_h)
{
    const uint8_t *b = data;
    bmp = 0;
    if (!b || size < 54 || b[0] != 'B' || b[1] != 'M' || !scr_w || !scr_h)
        return;
    int32_t w = (int32_t)rd32(b + 18), h = (int32_t)rd32(b + 22);
    int depth = b[28] | b[29] << 8;
    uint32_t comp = rd32(b + 30), off = rd32(b + 10);
    if (w <= 0 || h == 0 || w > 4096 || h > 4096 || h < -4096 || (depth != 24 && depth != 32) || (comp != 0 && comp != 3))
        return;
    int rb = ((w * (depth / 8)) + 3) & ~3, ah = h < 0 ? -h : h;
    if ((uint64_t)off + (uint64_t)rb * (uint64_t)ah > size)
        return;
    bmp = b;
    bmp_size = size;
    bw = w;
    bh = ah;
    bpp = depth / 8;
    row_bytes = rb;
    top_down = h < 0;
    pix_off = off;
    logo_x = x;
    logo_y = y;
    logo_sw = scr_w;
    logo_sh = scr_h;
}

/* Lage des Logos auf diesem Bildschirm: links oben, Massstab; 0 = kein Logo */
static int logo_place(uint32_t w, uint32_t h, int *x, int *y, int *k)
{
    if (!bmp)
        return 0;
    *k = w >= 2 * logo_sw && h >= 2 * logo_sh ? 2 : 1;
    if (w == logo_sw && h == logo_sh) {
        *x = (int)logo_x;
        *y = (int)logo_y;
    } else { /* gleiche relative Lage der Mitte */
        *x = (int)(((uint64_t)logo_x * 2 + (uint64_t)bw) * w / (2 * (uint64_t)logo_sw)) - bw * *k / 2;
        *y = (int)(((uint64_t)logo_y * 2 + (uint64_t)bh) * h / (2 * (uint64_t)logo_sh)) - bh * *k / 2;
    }
    return 1;
}

/* Balken: links oben, Breite, Hoehe */
static void bar_place(uint32_t w, uint32_t h, int *x, int *y, int *bw_, int *bh_)
{
    int m = (int)(w < h ? w : h);
    int width = m / 5, height = m / 160;
    if (width < 120)
        width = 120;
    if (height < 4)
        height = 4;
    int lx, ly, k, top;
    if (logo_place(w, h, &lx, &ly, &k))
        top = ly + bh * k + (int)h / 10;
    else
        top = (int)h * 62 / 100;
    if (top > (int)h - 4 * height)
        top = (int)h - 4 * height;
    *x = ((int)w - width) / 2;
    *y = top;
    *bw_ = width;
    *bh_ = height;
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t) /* t: 0 = a, 256 = b */
{
    uint32_t r = 0;
    for (int s = 0; s < 24; s += 8)
        r |= ((((a >> s) & 0xFF) * (256 - t) + ((b >> s) & 0xFF) * t) >> 8) << s;
    return r;
}

/* Deckung 0-16 der Kapsel (x, y, w, h; Radius h/2) im Pixel (px, py), in 1/8 Pixel gerechnet */
static int capsule_cov(int px, int py, int x, int y, int w, int h)
{
    if (w <= 0)
        return 0;
    int r8 = h * 4, cy8 = y * 8 + h * 4, x0 = x * 8 + r8, x1 = (x + w) * 8 - r8, n = 0;
    if (x1 < x0)
        x0 = x1 = (x0 + x1) / 2;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int X = px * 8 + sx * 2 + 1, Y = py * 8 + sy * 2 + 1;
            int dx = X < x0 ? x0 - X : X > x1 ? X - x1 : 0, dy = Y - cy8;
            n += dx * dx + dy * dy <= r8 * r8;
        }
    return n;
}

void splash_bar(volatile uint32_t *fb, uint32_t pitch, uint32_t w, uint32_t h, int permille)
{
    int x, y, width, height;
    bar_place(w, h, &x, &y, &width, &height);
    if (permille < 0)
        permille = 0;
    if (permille > 1000)
        permille = 1000;
    int fw = permille ? height + (width - height) * permille / 1000 : 0; /* Fuellung: mindestens ein runder Punkt */
    for (int py = y - 1; py <= y + height; py++)
        for (int px = x - 1; px <= x + width; px++) {
            if (px < 0 || py < 0 || (uint32_t)px >= w || (uint32_t)py >= h)
                continue;
            uint32_t c = mix(0, TRACK, (uint32_t)capsule_cov(px, py, x, y, width, height) * 16);
            int f = capsule_cov(px, py, x, y, fw, height);
            if (f)
                c = mix(c, FILL, (uint32_t)f * 16);
            fb[(uint64_t)py * pitch + (uint64_t)px] = c;
        }
}

void splash_draw(volatile uint32_t *fb, uint32_t pitch, uint32_t w, uint32_t h, int permille)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            fb[(uint64_t)y * pitch + x] = 0;
    int lx, ly, k;
    if (logo_place(w, h, &lx, &ly, &k))
        for (int y = 0; y < bh * k; y++) {
            int sy = y / k, row = top_down ? sy : bh - 1 - sy, py = ly + y;
            if (py < 0 || (uint32_t)py >= h)
                continue;
            const uint8_t *src = bmp + pix_off + (uint64_t)row * (uint64_t)row_bytes;
            for (int x = 0; x < bw * k; x++) {
                int px = lx + x;
                if (px < 0 || (uint32_t)px >= w)
                    continue;
                const uint8_t *p = src + (x / k) * bpp; /* B, G, R */
                fb[(uint64_t)py * pitch + (uint64_t)px] = (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
            }
        }
    splash_bar(fb, pitch, w, h, permille);
}
