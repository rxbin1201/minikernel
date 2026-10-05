/* Gemeinsames Aussehen fuer den Desktop und seine Fenster-Programme, siehe ui.h */

#include "ui.h"

int ui_pct = 100;
int FS, FS_SMALL, FS_MONO, ROW_H, CELL_W, CELL_H;

void ui_setup(int pct)
{
    static int loaded;
    ui_pct = pct > 0 ? pct : gfx_ui_scale();
    if (!loaded)
        fonts_init();
    loaded = 1;
    FS = U(13);
    FS_SMALL = U(11);
    FS_MONO = U(13);
    ROW_H = U(24);
    CELL_W = text_advance(font_mono, FS_MONO, 'M');
    CELL_H = text_height(font_mono, FS_MONO) + U(1);
}

float ui_sin(float x) /* auf [-pi/2, pi/2] falten (dort ist das Polynom genau), dann Taylor */
{
    const float pi = 3.14159265f, tau = 6.2831853f;
    x -= tau * (float)(int)(x / tau);
    if (x > pi) x -= tau;
    if (x < -pi) x += tau;
    if (x > pi / 2) x = pi - x;
    if (x < -pi / 2) x = -pi - x;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}

void ui_scrollbar(Surface *s, int x, int y, int h, int total, int visible, int top)
{
    if (total <= visible || visible <= 0)
        return;
    int bh = (h - U(8)) * visible / total;
    if (bh < U(24))
        bh = U(24);
    int range = total - visible;
    int by = y + U(4) + (h - U(8) - bh) * (top < 0 ? 0 : top > range ? range : top) / range;
    gfx_round_rect(s, x - U(10), by, U(6), bh, U(3), 0x000000, 70);
}

void ui_folder_icon(Surface *s, int x, int y, int sz)
{
    gfx_round_rect(s, x, y + sz / 8, sz * 45 / 100, sz / 4, sz / 10, 0x3B99F0, 255);
    gfx_round_rect_grad(s, x, y + sz / 4, sz, sz * 65 / 100, sz / 8, 0x74BCFA, 0x3B99F0, 255);
}

void ui_doc_icon(Surface *s, int x, int y, int sz, u32 accent)
{
    gfx_round_rect(s, x + sz / 8, y, sz * 3 / 4, sz, sz / 10, 0xFFFFFF, 255);
    gfx_round_frame(s, x + sz / 8, y, sz * 3 / 4, sz, sz / 10, 0xB0B0B8, 255);
    if (accent)
        gfx_round_rect(s, x + sz / 4, y + sz / 2, sz / 2, sz / 4, sz / 16, accent, 255);
}

/* Programmsymbole (gezeichnet, in jeder Groesse) */
/* ---------- Hintergrund ---------- */

static const struct {
    const char *name;
    u32         col[5];
} themes[UI_WALLPAPERS] = {
    {"Abendrot", {0x1B2A6B, 0x5B3FC4, 0xC64B9E, 0xF28C5A, 0xFBD28B}},
    {"Ozean", {0x0B1E3F, 0x0E4C7A, 0x1F8FB5, 0x55C6C9, 0xC9F2E7}},
    {"Wald", {0x0F2A1D, 0x1E5A3A, 0x3E8E4E, 0x9BC46A, 0xE9F0B5}},
    {"Lavendel", {0x241B4B, 0x4B3A8F, 0x8C6FD1, 0xC9A8EE, 0xF3E6FF}},
    {"Graphit", {0x16181D, 0x2B2F38, 0x4A505C, 0x7D8593, 0xC9CED6}},
};

const char *ui_wallpaper_name(int theme)
{
    return themes[theme >= 0 && theme < UI_WALLPAPERS ? theme : 0].name;
}

u32 ui_wallpaper_at(int theme, float u, float v)
{
    static const float stop[] = {0.0f, 0.30f, 0.55f, 0.78f, 1.0f};
    const u32 *col = themes[theme >= 0 && theme < UI_WALLPAPERS ? theme : 0].col;
    float wave = 0.10f * ui_sin(6.2831853f * (u * 1.1f + 0.15f)) + 0.06f * ui_sin(6.2831853f * (u * 2.3f + v * 0.7f));
    float t = v * 0.85f + u * 0.30f + wave - 0.08f;
    u32 c;
    if (t <= 0) {
        c = col[0];
    } else if (t >= 1) {
        c = col[4];
    } else {
        int i = 0;
        while (t > stop[i + 1])
            i++;
        float f = (t - stop[i]) / (stop[i + 1] - stop[i]);
        f = f * f * (3 - 2 * f);
        c = gfx_mix(col[i], col[i + 1], (int)(f * 255));
    }
    float hx = u - 0.78f, hy = v - 0.18f; /* heller Schein oben rechts */
    float glow = 1.0f - (hx * hx * 2.2f + hy * hy * 5.0f);
    if (glow > 0)
        c = gfx_mix(c, 0xFFFFFF, (int)(glow * glow * 70));
    return c;
}

/* ---------- Programmsymbole ---------- */

void ui_app_icon(Surface *s, int icon, int x, int y, int size)
{
    float S = (float)size;
    int r = size * 225 / 1000;
    gfx_shadow(s, x, y + size / 40, size, size, r, size / 12, 45);
    switch (icon) {
    case ICON_FILES: {
        gfx_round_rect_grad(s, x, y, size, size, r, 0x6FC3FF, 0x1C74E9, 255);
        int fx = x + size * 18 / 100, fy = y + size * 30 / 100, fw = size * 64 / 100, fh = size * 46 / 100;
        gfx_round_rect(s, fx, fy - size / 12, fw * 42 / 100, size / 6, size / 24, 0xDDEBFF, 255);
        gfx_round_rect(s, fx, fy, fw, fh, size / 16, 0xFFFFFF, 245);
        gfx_blend_fill(s, fx, fy + fh / 4, fw, 1, 0x1C74E9, 40);
        break;
    }
    case ICON_TERM:
        gfx_round_rect_grad(s, x, y, size, size, r, 0x3A3A3C, 0x161618, 255);
        gfx_round_frame(s, x, y, size, size, r, 0x6E6E73, 120);
        text_draw(s, font_mono, size * 30 / 100, x + size * 14 / 100, y + size * 12 / 100, ">_", 0x4ADE80);
        break;
    case ICON_CALC: {
        gfx_round_rect_grad(s, x, y, size, size, r, 0x5A5A5F, 0x2C2C2E, 255);
        float cs = S * 0.19f, gap = S * 0.045f, ox = x + (S - 3 * cs - 2 * gap) / 2, oy = y + S * 0.20f;
        for (int i = 0; i < 9; i++) {
            int col = i % 3, row = i / 3;
            u32 c = col == 2 ? 0xFF9F0A : row == 0 ? 0xA5A5A5 : 0x8E8E93;
            gfx_disc(s, ox + col * (cs + gap) + cs / 2, oy + row * (cs + gap) + cs / 2, cs / 2, c, 255);
        }
        break;
    }
    case ICON_CLOCK: {
        gfx_round_rect_grad(s, x, y, size, size, r, 0xFFFFFF, 0xE5E5EA, 255);
        float cx = x + S / 2, cy = y + S / 2, R = S * 0.38f;
        gfx_disc(s, cx, cy, R, 0x1C1C1E, 255);
        gfx_disc(s, cx, cy, R - S * 0.03f, 0xFFFFFF, 255);
        for (int i = 0; i < 12; i++) {
            float a = 6.2831853f * i / 12, sx = ui_sin(a), cy2 = ui_cos(a);
            gfx_capsule(s, cx + sx * R * 0.78f, cy - cy2 * R * 0.78f, cx + sx * R * 0.86f, cy - cy2 * R * 0.86f, S * 0.02f,
                        0x3A3A3C, 255);
        }
        gfx_capsule(s, cx, cy, cx + R * 0.35f, cy - R * 0.30f, S * 0.045f, 0x1C1C1E, 255);
        gfx_capsule(s, cx, cy, cx - R * 0.05f, cy - R * 0.70f, S * 0.035f, 0x1C1C1E, 255);
        gfx_capsule(s, cx, cy, cx - R * 0.55f, cy + R * 0.45f, S * 0.015f, 0xFF3B30, 255);
        gfx_disc(s, cx, cy, S * 0.035f, 0xFF3B30, 255);
        break;
    }
    case ICON_PAINT:
        gfx_round_rect_grad(s, x, y, size, size, r, 0xFFFFFF, 0xECECF0, 255);
        gfx_disc(s, x + S * 0.38f, y + S * 0.40f, S * 0.20f, 0xFF3B30, 230);
        gfx_disc(s, x + S * 0.62f, y + S * 0.40f, S * 0.20f, 0xFFCC00, 210);
        gfx_disc(s, x + S * 0.50f, y + S * 0.62f, S * 0.20f, 0x0A84FF, 200);
        gfx_capsule(s, x + S * 0.22f, y + S * 0.85f, x + S * 0.80f, y + S * 0.20f, S * 0.05f, 0x8E5A2B, 255);
        break;
    case ICON_SNAKE:
        gfx_round_rect_grad(s, x, y, size, size, r, 0x4ADE80, 0x16A34A, 255);
        gfx_capsule(s, x + S * 0.22f, y + S * 0.70f, x + S * 0.50f, y + S * 0.70f, S * 0.12f, 0xFFFFFF, 255);
        gfx_capsule(s, x + S * 0.50f, y + S * 0.70f, x + S * 0.50f, y + S * 0.38f, S * 0.12f, 0xFFFFFF, 255);
        gfx_capsule(s, x + S * 0.50f, y + S * 0.38f, x + S * 0.76f, y + S * 0.38f, S * 0.12f, 0xFFFFFF, 255);
        gfx_disc(s, x + S * 0.78f, y + S * 0.36f, S * 0.02f, 0x14532D, 255);
        gfx_disc(s, x + S * 0.24f, y + S * 0.28f, S * 0.06f, 0xFF3B30, 255);
        break;
    case ICON_TETRIS: {
        gfx_round_rect_grad(s, x, y, size, size, r, 0x3B3B64, 0x1B1B33, 255);
        static const int blk[4][2] = {{0, 2}, {1, 2}, {2, 2}, {1, 1}};
        static const int blk2[4][2] = {{2, 1}, {3, 1}, {3, 0}, {3, 2}};
        float b = S * 0.17f, ox = x + S * 0.16f, oy = y + S * 0.22f;
        for (int i = 0; i < 4; i++) {
            gfx_round_rect(s, (int)(ox + blk[i][0] * b), (int)(oy + blk[i][1] * b), (int)b - 1, (int)b - 1, size / 30,
                           0xA855F7, 255);
            gfx_round_rect(s, (int)(ox + blk2[i][0] * b), (int)(oy + blk2[i][1] * b), (int)b - 1, (int)b - 1, size / 30,
                           0x22D3EE, 255);
        }
        break;
    }
    case ICON_ABOUT:
        gfx_round_rect_grad(s, x, y, size, size, r, 0x8E8EF0, 0x5856D6, 255);
        gfx_disc(s, x + S / 2, y + S * 0.28f, S * 0.07f, 0xFFFFFF, 255);
        gfx_capsule(s, x + S / 2, y + S * 0.45f, x + S / 2, y + S * 0.76f, S * 0.12f, 0xFFFFFF, 255);
        break;
    case ICON_EDIT: { /* Notizblock mit gelbem Kopf und Stift */
        gfx_round_rect_grad(s, x, y, size, size, r, 0xFFFFFF, 0xF2F2F5, 255);
        gfx_round_frame(s, x, y, size, size, r, 0xC7C7CC, 200);
        gfx_round_rect_grad(s, x, y, size, size * 26 / 100, r, 0xFFD60A, 0xF5B800, 255);
        gfx_fill(s, x, y + size * 16 / 100, size, size * 10 / 100, 0xF5B800);
        for (int i = 0; i < 4; i++)
            gfx_fill(s, x + size * 16 / 100, y + size * 40 / 100 + i * size * 13 / 100, size * (i == 3 ? 40 : 66) / 100,
                     size / 40 + 1, 0xB8B8BE);
        gfx_capsule(s, x + S * 0.52f, y + S * 0.86f, x + S * 0.86f, y + S * 0.42f, S * 0.07f, 0xFF9F0A, 255);
        gfx_capsule(s, x + S * 0.47f, y + S * 0.92f, x + S * 0.53f, y + S * 0.85f, S * 0.05f, 0x3A3A3C, 255);
        break;
    }
    case ICON_MUSIC: { /* zwei Noten mit Balken auf rot-violettem Grund */
        gfx_round_rect_grad(s, x, y, size, size, r, 0xFF6B81, 0xB43CF0, 255);
        float nx = x + S * 0.2f, ny = y + S * 0.2f, n = S * 0.6f;
        gfx_disc(s, nx + n * 0.28f, ny + n * 0.78f, n * 0.13f, 0xFFFFFF, 255);
        gfx_disc(s, nx + n * 0.70f, ny + n * 0.70f, n * 0.13f, 0xFFFFFF, 255);
        gfx_capsule(s, nx + n * 0.39f, ny + n * 0.78f, nx + n * 0.39f, ny + n * 0.22f, n * 0.06f, 0xFFFFFF, 255);
        gfx_capsule(s, nx + n * 0.81f, ny + n * 0.70f, nx + n * 0.81f, ny + n * 0.14f, n * 0.06f, 0xFFFFFF, 255);
        gfx_capsule(s, nx + n * 0.39f, ny + n * 0.24f, nx + n * 0.81f, ny + n * 0.16f, n * 0.10f, 0xFFFFFF, 255);
        break;
    }
    case ICON_SETTINGS: { /* Zahnrad auf grauem Grund */
        gfx_round_rect_grad(s, x, y, size, size, r, 0xB8BCC6, 0x6E7280, 255);
        float cx = x + S / 2, cy = y + S / 2, R = S * 0.27f;
        for (int i = 0; i < 8; i++) {
            float a = 6.2831853f * (float)i / 8, sx = ui_sin(a), sy = ui_cos(a);
            gfx_capsule(s, cx + sx * R * 0.9f, cy - sy * R * 0.9f, cx + sx * R * 1.32f, cy - sy * R * 1.32f, S * 0.11f,
                        0xF4F4F6, 255);
        }
        gfx_disc(s, cx, cy, R, 0xF4F4F6, 255);
        gfx_disc(s, cx, cy, R * 0.42f, 0x8A8E99, 255);
        break;
    }
    case ICON_TEXT:
        gfx_round_rect_grad(s, x + size / 8, y, size * 3 / 4, size, size / 12, 0xFFFFFF, 0xF2F2F5, 255);
        gfx_round_frame(s, x + size / 8, y, size * 3 / 4, size, size / 12, 0xC7C7CC, 255);
        for (int i = 0; i < 6; i++)
            gfx_fill(s, x + size / 4, y + size / 4 + i * size / 10, size / 2 - (i % 3) * size / 12, size / 40 + 1, 0xA1A1A6);
        break;
    case ICON_IMAGE:
        gfx_round_rect_grad(s, x, y, size, size, r, 0x7DD3FC, 0x38BDF8, 255);
        gfx_disc(s, x + S * 0.70f, y + S * 0.30f, S * 0.10f, 0xFDE047, 255);
        gfx_disc(s, x + S * 0.30f, y + S * 1.05f, S * 0.45f, 0x22C55E, 255);
        gfx_disc(s, x + S * 0.80f, y + S * 1.10f, S * 0.45f, 0x16A34A, 255);
        gfx_round_frame(s, x, y, size, size, r, 0xFFFFFF, 90);
        break;
    default:
        gfx_round_rect_grad(s, x, y, size, size, r, 0xC7C7CC, 0x8E8E93, 255);
    }
}
