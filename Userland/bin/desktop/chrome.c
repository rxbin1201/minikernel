/* Desktop: Masse, Hintergrundbild, Menueleiste, Dock, Menues und die gezeichneten Programmsymbole */

#include "desktop.h"

int ui_pct = 100;
int MENUBAR_H, TITLE_H, DOCK_H, ROW_H, RADIUS, SHADOW, FS, FS_SMALL, FS_MONO, CELL_W, CELL_H;
int menu_open, menu_hover = -1, dock_hover = -1;

static int ICON, DOCK_PAD, DOCK_GAP;

void ui_init(void)
{
    ui_pct = H >= 1300 ? 125 : 100;
    fonts_init();
    MENUBAR_H = U(26);
    TITLE_H = U(30);
    ROW_H = U(24);
    RADIUS = U(10);
    SHADOW = U(28);
    FS = U(13);
    FS_SMALL = U(11);
    FS_MONO = U(13);
    CELL_W = text_advance(font_mono, FS_MONO, 'M');
    CELL_H = text_height(font_mono, FS_MONO) + U(1);
    ICON = U(52);
    DOCK_PAD = U(7);
    DOCK_GAP = U(8);
    DOCK_H = ICON + 2 * DOCK_PAD + U(10);
}

/* ======================================================================================================================
 * Hintergrund: weicher Farbverlauf mit Wellen (berechnet), dazu eine weichgezeichnete Kopie fuer das Milchglas
 * ==================================================================================================================== */

static float fsin(float x) /* sin ohne Bibliothek: auf [-pi/2, pi/2] falten (dort ist das Polynom genau), dann Taylor */
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

/* Bogen um (cx, cy) mit Radius r von Winkel a0 bis a1 (Bogenmass, 0 = rechts, gegen den Uhrzeigersinn nach oben) */
static void arc(Surface *s, float cx, float cy, float r, float a0, float a1, float w, u32 c, int alpha)
{
    const int n = 6;
    for (int i = 0; i < n; i++) {
        float t0 = a0 + (a1 - a0) * i / n, t1 = a0 + (a1 - a0) * (i + 1) / n;
        gfx_capsule(s, cx + fsin(t0 + 1.5707963f) * r, cy - fsin(t0) * r, cx + fsin(t1 + 1.5707963f) * r, cy - fsin(t1) * r, w,
                    c, alpha);
    }
}

static u32 palette(float t)
{
    static const float stop[] = {0.0f, 0.30f, 0.55f, 0.78f, 1.0f};
    static const u32 col[] = {0x1B2A6B, 0x5B3FC4, 0xC64B9E, 0xF28C5A, 0xFBD28B};
    if (t <= 0) return col[0];
    if (t >= 1) return col[4];
    int i = 0;
    while (t > stop[i + 1])
        i++;
    float f = (t - stop[i]) / (stop[i + 1] - stop[i]);
    f = f * f * (3 - 2 * f);
    return gfx_mix(col[i], col[i + 1], (int)(f * 255));
}

void make_background(void)
{
    surface_new(&bg, W, H);
    unsigned rnd = 12345;
    for (int y = 0; y < H; y++) {
        float v = (float)y / (float)H;
        for (int x = 0; x < W; x++) {
            float u = (float)x / (float)W;
            float wave = 0.10f * fsin(6.2831853f * (u * 1.1f + 0.15f)) + 0.06f * fsin(6.2831853f * (u * 2.3f + v * 0.7f));
            float t = v * 0.85f + u * 0.30f + wave - 0.08f;
            u32 c = palette(t);
            float hx = u - 0.78f, hy = v - 0.18f; /* heller Schein oben rechts */
            float glow = 1.0f - (hx * hx * 2.2f + hy * hy * 5.0f);
            if (glow > 0)
                c = gfx_mix(c, 0xFFFFFF, (int)(glow * glow * 70));
            rnd = rnd * 1103515245u + 12345u; /* etwas Rauschen gegen Stufen im Verlauf */
            int n = (int)((rnd >> 16) % 3) - 1;
            int r = (int)(c >> 16 & 0xFF) + n, g = (int)(c >> 8 & 0xFF) + n, b = (int)(c & 0xFF) + n;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            bg.px[(u64)y * (u64)W + (u64)x] = (u32)r << 16 | (u32)g << 8 | (u32)b;
        }
    }
    surface_new(&bg_blur, W, H);
    memcpy(bg_blur.px, bg.px, (u64)W * (u64)H * 4);
    gfx_blur(&bg_blur, U(18));
}

/* ======================================================================================================================
 * Programmsymbole (gezeichnet, in jeder Groesse)
 * ==================================================================================================================== */

#define ICON_TEXT  100
#define ICON_IMAGE 101

void draw_app_icon(Surface *s, int kind, int x, int y, int size)
{
    float S = (float)size;
    int r = size * 225 / 1000;
    gfx_shadow(s, x, y + size / 40, size, size, r, size / 12, 45);
    switch (kind) {
    case A_FILES: {
        gfx_round_rect_grad(s, x, y, size, size, r, 0x6FC3FF, 0x1C74E9, 255);
        int fx = x + size * 18 / 100, fy = y + size * 30 / 100, fw = size * 64 / 100, fh = size * 46 / 100;
        gfx_round_rect(s, fx, fy - size / 12, fw * 42 / 100, size / 6, size / 24, 0xDDEBFF, 255);
        gfx_round_rect(s, fx, fy, fw, fh, size / 16, 0xFFFFFF, 245);
        gfx_blend_fill(s, fx, fy + fh / 4, fw, 1, 0x1C74E9, 40);
        break;
    }
    case A_TERM:
        gfx_round_rect_grad(s, x, y, size, size, r, 0x3A3A3C, 0x161618, 255);
        gfx_round_frame(s, x, y, size, size, r, 0x6E6E73, 120);
        text_draw(s, font_mono, size * 30 / 100, x + size * 14 / 100, y + size * 12 / 100, ">_", 0x4ADE80);
        break;
    case A_CALC: {
        gfx_round_rect_grad(s, x, y, size, size, r, 0x5A5A5F, 0x2C2C2E, 255);
        float cs = S * 0.19f, gap = S * 0.045f, ox = x + (S - 3 * cs - 2 * gap) / 2, oy = y + S * 0.20f;
        for (int i = 0; i < 9; i++) {
            int col = i % 3, row = i / 3;
            u32 c = col == 2 ? 0xFF9F0A : row == 0 ? 0xA5A5A5 : 0x8E8E93;
            gfx_disc(s, ox + col * (cs + gap) + cs / 2, oy + row * (cs + gap) + cs / 2, cs / 2, c, 255);
        }
        break;
    }
    case A_CLOCK: {
        gfx_round_rect_grad(s, x, y, size, size, r, 0xFFFFFF, 0xE5E5EA, 255);
        float cx = x + S / 2, cy = y + S / 2, R = S * 0.38f;
        gfx_disc(s, cx, cy, R, 0x1C1C1E, 255);
        gfx_disc(s, cx, cy, R - S * 0.03f, 0xFFFFFF, 255);
        for (int i = 0; i < 12; i++) {
            float a = 6.2831853f * i / 12, sx = fsin(a), cy2 = fsin(a + 1.5707963f);
            gfx_capsule(s, cx + sx * R * 0.78f, cy - cy2 * R * 0.78f, cx + sx * R * 0.86f, cy - cy2 * R * 0.86f, S * 0.02f,
                        0x3A3A3C, 255);
        }
        gfx_capsule(s, cx, cy, cx + R * 0.35f, cy - R * 0.30f, S * 0.045f, 0x1C1C1E, 255);
        gfx_capsule(s, cx, cy, cx - R * 0.05f, cy - R * 0.70f, S * 0.035f, 0x1C1C1E, 255);
        gfx_capsule(s, cx, cy, cx - R * 0.55f, cy + R * 0.45f, S * 0.015f, 0xFF3B30, 255);
        gfx_disc(s, cx, cy, S * 0.035f, 0xFF3B30, 255);
        break;
    }
    case A_PAINT:
        gfx_round_rect_grad(s, x, y, size, size, r, 0xFFFFFF, 0xECECF0, 255);
        gfx_disc(s, x + S * 0.38f, y + S * 0.40f, S * 0.20f, 0xFF3B30, 230);
        gfx_disc(s, x + S * 0.62f, y + S * 0.40f, S * 0.20f, 0xFFCC00, 210);
        gfx_disc(s, x + S * 0.50f, y + S * 0.62f, S * 0.20f, 0x0A84FF, 200);
        gfx_capsule(s, x + S * 0.22f, y + S * 0.85f, x + S * 0.80f, y + S * 0.20f, S * 0.05f, 0x8E5A2B, 255);
        break;
    case A_SNAKE:
        gfx_round_rect_grad(s, x, y, size, size, r, 0x4ADE80, 0x16A34A, 255);
        gfx_capsule(s, x + S * 0.22f, y + S * 0.70f, x + S * 0.50f, y + S * 0.70f, S * 0.12f, 0xFFFFFF, 255);
        gfx_capsule(s, x + S * 0.50f, y + S * 0.70f, x + S * 0.50f, y + S * 0.38f, S * 0.12f, 0xFFFFFF, 255);
        gfx_capsule(s, x + S * 0.50f, y + S * 0.38f, x + S * 0.76f, y + S * 0.38f, S * 0.12f, 0xFFFFFF, 255);
        gfx_disc(s, x + S * 0.78f, y + S * 0.36f, S * 0.02f, 0x14532D, 255);
        gfx_disc(s, x + S * 0.24f, y + S * 0.28f, S * 0.06f, 0xFF3B30, 255);
        break;
    case A_TETRIS: {
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
    case A_ABOUT:
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

static int icon_of_win(const Win *w)
{
    switch (w->kind) {
    case W_TERM: return A_TERM;
    case W_FILES: return A_FILES;
    case W_CALC: return A_CALC;
    case W_CLOCK: return A_CLOCK;
    case W_ABOUT: return A_ABOUT;
    case W_IMAGE: return ICON_IMAGE;
    default: return ICON_TEXT;
    }
}

const char *app_name(const Win *w)
{
    if (!w)
        return "Schreibtisch";
    switch (w->kind) {
    case W_TERM: return "Terminal";
    case W_FILES: return "Dateien";
    case W_TEXT: return "Textansicht";
    case W_IMAGE: return "Bildansicht";
    case W_CALC: return "Rechner";
    case W_CLOCK: return "Uhr";
    default: return "Info";
    }
}

/* ======================================================================================================================
 * Menueleiste
 * ==================================================================================================================== */

static int logo_x(void) { return U(12); }
static int appname_x(void) { return U(40); }

static void draw_logo(Surface *s, int x, int y, int size)
{
    gfx_round_rect_grad(s, x, y, size, size, size / 4, 0x6A7CFF, 0xC04BD6, 255);
    gfx_disc(s, x + size * 0.5f, y + size * 0.5f, size * 0.18f, 0xFFFFFF, 235);
}

void draw_menubar(void)
{
    Surface *s = &gfx_screen;
    gfx_blit_round(s, &bg_blur, 0, 0, 0, 0, W, MENUBAR_H, 0); /* Milchglas: weichgezeichneter Hintergrund, aufgehellt */
    gfx_blend_fill(s, 0, 0, W, MENUBAR_H, 0xFFFFFF, 150);
    gfx_blend_fill(s, 0, MENUBAR_H - 1, W, 1, 0x000000, 28);
    int ty = (MENUBAR_H - text_height(font_ui, FS)) / 2;
    if (menu_open == 1)
        gfx_round_rect(s, logo_x() - U(6), U(3), U(28), MENUBAR_H - U(6), U(5), 0x000000, 36);
    draw_logo(s, logo_x(), (MENUBAR_H - U(15)) / 2, U(15));
    const char *name = app_name(focused());
    if (menu_open == 2)
        gfx_round_rect(s, appname_x() - U(8), U(3), text_width(font_bold, FS, name) + U(16), MENUBAR_H - U(6), U(5),
                       0x000000, 36);
    text_draw(s, font_bold, FS, appname_x(), ty, name, C_TEXT);

    /* rechts: Lautstaerke und Datum/Uhrzeit */
    char t[48] = "";
    s64 now = sys_time();
    if (now > 0) {
        static const char *wd[7] = {"So.", "Mo.", "Di.", "Mi.", "Do.", "Fr.", "Sa."};
        static const char *mo[12] = {"Jan.", "Feb.", "M\xC3\xA4rz", "Apr.", "Mai", "Juni", "Juli", "Aug.", "Sep.", "Okt.", "Nov.", "Dez."};
        DateTime dt;
        time_to_date((u64)now, &dt);
        snprintf(t, sizeof(t), "%s %d. %s  %02d:%02d", wd[dt.wday % 7], dt.day, mo[(dt.month + 11) % 12], dt.hour, dt.min);
    }
    int tw = text_width(font_ui, FS, t), x = W - U(14) - tw;
    text_draw(s, font_ui, FS, x, ty, t, C_TEXT);
    s64 vol = sys_audio(4, (u64)-1, 0);
    if (vol >= 0) { /* Lautsprecher: Kasten, Trichter, Schallwellen je nach Lautstaerke */
        float sx = (float)(x - U(36)), sy = MENUBAR_H * 0.5f, k = (float)U(1);
        gfx_round_rect(s, (int)sx, (int)(sy - 2.5f * k), (int)(3.5f * k), (int)(5 * k), 1, C_TEXT, 255);
        for (int i = 0; i <= 4; i++) /* Trichter: gefuelltes Trapez aus senkrechten Strichen */
            gfx_capsule(s, sx + (3 + i) * k, sy - (2.5f + i * 0.9f) * k, sx + (3 + i) * k, sy + (2.5f + i * 0.9f) * k, 1.4f * k,
                        C_TEXT, 255);
        if (vol > 0)
            arc(s, sx + 8 * k, sy, 4 * k, -0.9f, 0.9f, 1.4f * k, C_TEXT, 230);
        if (vol > 50)
            arc(s, sx + 8 * k, sy, 7.5f * k, -0.9f, 0.9f, 1.4f * k, C_TEXT, 230);
    }
}

void damage_menubar(void) { damage(0, 0, W, MENUBAR_H); }

int menubar_hit(int x, int y)
{
    if (y >= MENUBAR_H)
        return 0;
    if (x >= logo_x() - U(6) && x < logo_x() + U(22))
        return 1;
    if (x >= appname_x() - U(8) && x < appname_x() + text_width(font_bold, FS, app_name(focused())) + U(8))
        return 2;
    return 0;
}

/* ======================================================================================================================
 * Menues
 * ==================================================================================================================== */

static const MenuItem logo_menu[] = {
    {"\xC3\x9C" "ber MiniKernel", A_ABOUT}, {"", A_SEP}, {"Neues Terminal", A_TERM}, {"Dateien", A_FILES},
    {"Rechner", A_CALC}, {"Uhr", A_CLOCK}, {"", A_SEP}, {"Malen", A_PAINT}, {"Snake", A_SNAKE}, {"Tetris", A_TETRIS},
    {"", A_SEP}, {"Zur Konsole", A_QUIT},
};
static const MenuItem app_menu[] = {
    {"Neues Fenster", A_WIN_NEW}, {"Minimieren", A_WIN_MIN}, {"Zoomen", A_WIN_ZOOM}, {"", A_SEP},
    {"Fenster schlie\xC3\x9F" "en", A_WIN_CLOSE},
};

static const MenuItem *menu_items(int *n)
{
    if (menu_open == 1) {
        *n = (int)(sizeof(logo_menu) / sizeof(logo_menu[0]));
        return logo_menu;
    }
    *n = (int)(sizeof(app_menu) / sizeof(app_menu[0]));
    return app_menu;
}

static void menu_box(int *x, int *y, int *w, int *h)
{
    int n;
    const MenuItem *m = menu_items(&n);
    *x = menu_open == 1 ? logo_x() - U(6) : appname_x() - U(8);
    *y = MENUBAR_H + U(2);
    *w = U(230);
    *h = U(10);
    for (int i = 0; i < n; i++)
        *h += m[i].action == A_SEP ? U(11) : U(24);
}

static int menu_item_y(int i)
{
    int n, x, y, w, h;
    const MenuItem *m = menu_items(&n);
    menu_box(&x, &y, &w, &h);
    y += U(5);
    for (int k = 0; k < i; k++)
        y += m[k].action == A_SEP ? U(11) : U(24);
    return y;
}

void draw_menu(void)
{
    if (!menu_open)
        return;
    Surface *s = &gfx_screen;
    int n, x, y, w, h;
    const MenuItem *m = menu_items(&n);
    menu_box(&x, &y, &w, &h);
    gfx_shadow(s, x, y + U(4), w, h, U(8), U(18), 60);
    gfx_round_rect(s, x, y, w, h, U(8), 0xF6F6F8, 248);
    gfx_round_frame(s, x, y, w, h, U(8), 0x000000, 30);
    for (int i = 0; i < n; i++) {
        int iy = menu_item_y(i);
        if (m[i].action == A_SEP) {
            gfx_blend_fill(s, x + U(10), iy + U(5), w - U(20), 1, 0x000000, 30);
            continue;
        }
        int hover = i == menu_hover;
        if (hover)
            gfx_round_rect(s, x + U(5), iy, w - U(10), U(24), U(5), C_ACCENT, 255);
        text_draw(s, font_ui, FS, x + U(14), iy + (U(24) - text_height(font_ui, FS)) / 2, m[i].label,
                  hover ? 0xFFFFFF : C_TEXT);
    }
}

void damage_menu(void)
{
    if (!menu_open)
        return;
    int x, y, w, h;
    menu_box(&x, &y, &w, &h);
    damage(x - U(20), y - U(4), w + U(40), h + U(40));
}

int menu_hit(int px, int py)
{
    if (!menu_open)
        return -1;
    int n, x, y, w, h;
    const MenuItem *m = menu_items(&n);
    menu_box(&x, &y, &w, &h);
    if (px < x || px >= x + w)
        return -1;
    for (int i = 0; i < n; i++) {
        int iy = menu_item_y(i);
        if (m[i].action != A_SEP && py >= iy && py < iy + U(24))
            return i;
    }
    return -1;
}

int menu_action(int i)
{
    int n;
    const MenuItem *m = menu_items(&n);
    return i >= 0 && i < n ? m[i].action : A_NONE;
}

/* ======================================================================================================================
 * Dock: Programme, Trennstrich, minimierte Fenster
 * ==================================================================================================================== */

static const struct {
    int         action;
    int         kind; /* Fensterart fuer den Punkt "laeuft", -1 = Vollbildprogramm */
    const char *name;
} dock_apps[] = {
    {A_FILES, W_FILES, "Dateien"}, {A_TERM, W_TERM, "Terminal"}, {A_CALC, W_CALC, "Rechner"}, {A_CLOCK, W_CLOCK, "Uhr"},
    {A_PAINT, -1, "Malen"},        {A_SNAKE, -1, "Snake"},       {A_TETRIS, -1, "Tetris"},
};
#define NAPPS ((int)(sizeof(dock_apps) / sizeof(dock_apps[0])))

static Win *minimized_win(int k) /* k-tes minimierte Fenster */
{
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used && wins[i].minimized && k-- == 0)
            return &wins[i];
    return 0;
}

static int n_minimized(void)
{
    int n = 0;
    for (int i = 0; i < MAXW; i++)
        n += wins[i].used && wins[i].minimized;
    return n;
}

static void dock_box(int *x, int *y, int *w, int *h)
{
    int nm = n_minimized();
    *w = DOCK_PAD * 2 + NAPPS * ICON + (NAPPS - 1) * DOCK_GAP + (nm ? DOCK_GAP * 2 + 1 + nm * (ICON + DOCK_GAP) : 0);
    *h = ICON + DOCK_PAD * 2;
    *x = (W - *w) / 2;
    *y = H - *h - U(8);
}

int dock_top(void)
{
    int x, y, w, h;
    dock_box(&x, &y, &w, &h);
    return y - U(6);
}

static int slot_x(int i) /* linke Kante von Symbol i (Programme, dann minimierte Fenster) */
{
    int x, y, w, h;
    dock_box(&x, &y, &w, &h);
    int sx = x + DOCK_PAD + i * (ICON + DOCK_GAP);
    if (i >= NAPPS)
        sx += DOCK_GAP + 1;
    return sx;
}

void draw_dock(void)
{
    Surface *s = &gfx_screen;
    int x, y, w, h, r = U(18);
    dock_box(&x, &y, &w, &h);
    gfx_shadow(s, x, y + U(2), w, h, r, U(22), 55);
    gfx_blit_round(s, &bg_blur, x, y, x, y, w, h, r);
    gfx_round_rect(s, x, y, w, h, r, 0xFFFFFF, 100);
    gfx_round_frame(s, x, y, w, h, r, 0xFFFFFF, 150);
    int nm = n_minimized();
    for (int i = 0; i < NAPPS + nm; i++) {
        int ix = slot_x(i), iy = y + DOCK_PAD;
        if (i < NAPPS) {
            draw_app_icon(s, dock_apps[i].action, ix, iy, ICON);
            int running = 0;
            for (int k = 0; k < MAXW; k++)
                running |= wins[k].used && wins[k].kind == dock_apps[i].kind;
            if (running)
                gfx_disc(s, ix + ICON * 0.5f, y + h - U(4), U(2) * 1.1f, 0x1D1D1F, 200);
        } else {
            Win *mw = minimized_win(i - NAPPS);
            if (mw)
                draw_app_icon(s, icon_of_win(mw), ix, iy, ICON);
        }
    }
    if (nm) {
        int sx = slot_x(NAPPS) - DOCK_GAP / 2 - 1;
        gfx_blend_fill(s, sx, y + DOCK_PAD, 1, ICON, 0x000000, 40);
    }
    if (dock_hover >= 0 && dock_hover < NAPPS + nm) { /* Name ueber dem Symbol */
        const char *name = dock_hover < NAPPS ? dock_apps[dock_hover].name : minimized_win(dock_hover - NAPPS)->title;
        int tw = text_width(font_ui, FS, name), bw = tw + U(22), bh = U(24);
        int bx = slot_x(dock_hover) + ICON / 2 - bw / 2, by = y - bh - U(10);
        if (bx < U(4)) bx = U(4);
        if (bx + bw > W - U(4)) bx = W - U(4) - bw;
        gfx_shadow(s, bx, by + U(2), bw, bh, U(6), U(10), 45);
        gfx_round_rect(s, bx, by, bw, bh, U(6), 0xF2F2F4, 245);
        gfx_round_frame(s, bx, by, bw, bh, U(6), 0x000000, 30);
        text_draw(s, font_ui, FS, bx + U(11), by + (bh - text_height(font_ui, FS)) / 2, name, C_TEXT);
    }
}

void damage_dock(void)
{
    int x, y, w, h;
    dock_box(&x, &y, &w, &h);
    int grow = ICON + DOCK_GAP; /* ein Symbol mehr/weniger und der Name darueber */
    damage(x - grow - U(30), y - U(50), w + 2 * grow + U(60), h + U(60));
}

int dock_hit(int px, int py)
{
    int x, y, w, h;
    dock_box(&x, &y, &w, &h);
    if (py < y || py >= y + h || px < x || px >= x + w)
        return -1;
    int nm = n_minimized();
    for (int i = 0; i < NAPPS + nm; i++) {
        int ix = slot_x(i);
        if (px >= ix - DOCK_GAP / 2 && px < ix + ICON + DOCK_GAP / 2)
            return i;
    }
    return -1;
}

void dock_hover_at(int px, int py)
{
    int h = dock_hit(px, py);
    if (h != dock_hover) {
        dock_hover = h;
        damage_dock();
    }
}

void dock_click(int i)
{
    if (i < 0)
        return;
    if (i >= NAPPS) {
        Win *w = minimized_win(i - NAPPS);
        if (w)
            raise_win(w);
        return;
    }
    int kind = dock_apps[i].kind;
    if (kind < 0) {
        do_action(dock_apps[i].action);
        return;
    }
    /* Programm hat Fenster: das oberste nach vorn; ist es schon vorn, ein neues */
    Win *top = 0;
    for (int k = nord - 1; k >= 0 && !top; k--)
        if (order[k]->kind == kind && !order[k]->minimized)
            top = order[k];
    if (top && top != focused()) {
        raise_win(top);
        return;
    }
    if (!top)
        for (int k = 0; k < MAXW && !top; k++)
            if (wins[k].used && wins[k].kind == kind && kind != W_TERM && kind != W_FILES)
                top = &wins[k];
    if (top && top != focused())
        raise_win(top);
    else
        do_action(dock_apps[i].action);
}
