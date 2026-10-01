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
