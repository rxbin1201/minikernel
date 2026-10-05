#include "gfx.h"

/* paint [datei.bmp]: Malprogramm mit Maus.
 *   Werkzeuge: Stift, Linie, Rechteck, Ellipse (je umrandet oder gefuellt), Fuellen, Radierer; 4 Pinselgroessen,
 *   16 Farben (rechte Maustaste auf eine Farbe: Hintergrundfarbe fuer den Radierer).
 *   Strg+S speichern (BMP, Standard /disk/BILD.BMP), Strg+Z rueckgaengig, Esc beenden.
 *   Unter dem Desktop im eigenen Fenster (Schliessen-Knopf wie Esc), sonst im Vollbild. */

#define BAR 52    /* Werkzeugleiste oben */
#define STATUS 18 /* Statuszeile unten */

enum { T_PEN, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_ERASER, T_COUNT };
static const char *const tool_names[T_COUNT] = {"Stift", "Linie", "Rechteck", "Ellipse", "F\xC3\xBCllen", "Radierer"};
static const u32 palette[16] = {
    0x000000, 0x808080, 0x800000, 0xFF0000, 0xFF8000, 0xFFFF00, 0x008000, 0x00FF00,
    0x008080, 0x00FFFF, 0x000080, 0x0000FF, 0x800080, 0xFF00FF, 0x8B4513, 0xFFFFFF,
};
static const int sizes[4] = {1, 3, 7, 15};

static Surface canvas, undo;
static int     tool = T_PEN, size_i = 1, filled;
static u32     color = 0x000000, back = 0xFFFFFF;
static char    path[PATH_MAX] = "/disk/BILD.BMP";
static char    msg[120];
static int     modified, quit_armed;
static int     W, H;

typedef struct {
    int x, y, w, h;
} Rect;

static int in(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* Lage der Bedienelemente */
static Rect tool_rect(int i) { Rect r = {6 + i * 74, 6, 70, 18}; return r; }
static Rect fill_rect(void) { Rect r = {6, 28, 70, 18}; return r; }
static Rect size_rect(int i) { Rect r = {82 + i * 26, 28, 22, 18}; return r; }
static Rect color_rect(int i) { Rect r = {460 + (i % 8) * 20, 6 + (i / 8) * 20, 18, 18}; return r; }
static Rect cur_color_rect(void) { Rect r = {628, 6, 38, 38}; return r; }
static Rect btn_rect(int i) { Rect r = {196 + i * 84, 28, 80, 18}; return r; }
static const char *const btn_names[3] = {"Neu", "Speichern", "Ende"};

static void button(Rect r, const char *label, int active)
{
    gfx_fill(&gfx_screen, r.x, r.y, r.w, r.h, active ? RGB(170, 200, 255) : RGB(225, 225, 232));
    gfx_bevel(&gfx_screen, r.x, r.y, r.w, r.h, !active);
    int tw = gfx_text_width(label);
    gfx_text(&gfx_screen, r.x + (r.w - tw) / 2, r.y + 1, label, RGB(20, 20, 30), GFX_TRANSPARENT);
}

static void draw_bar(void)
{
    gfx_no_clip();
    gfx_fill(&gfx_screen, 0, 0, W, BAR, RGB(205, 208, 218));
    gfx_fill(&gfx_screen, 0, BAR - 1, W, 1, RGB(90, 90, 100));
    for (int i = 0; i < T_COUNT; i++)
        button(tool_rect(i), tool_names[i], tool == i);
    button(fill_rect(), filled ? "gef\xC3\xBCllt" : "Umriss", filled);
    for (int i = 0; i < 4; i++) {
        Rect r = size_rect(i);
        button(r, "", size_i == i);
        int d = sizes[i] > 12 ? 12 : sizes[i];
        gfx_fill_circle(&gfx_screen, r.x + r.w / 2, r.y + r.h / 2, d / 2, 0);
    }
    for (int i = 0; i < 16; i++) {
        Rect r = color_rect(i);
        gfx_fill(&gfx_screen, r.x, r.y, r.w, r.h, palette[i]);
        gfx_rect(&gfx_screen, r.x, r.y, r.w, r.h, palette[i] == color ? RGB(255, 60, 60) : RGB(60, 60, 70));
    }
    Rect c = cur_color_rect(); /* Vorder- und Hintergrundfarbe */
    gfx_fill(&gfx_screen, c.x + 12, c.y + 12, 26, 26, back);
    gfx_rect(&gfx_screen, c.x + 12, c.y + 12, 26, 26, 0);
    gfx_fill(&gfx_screen, c.x, c.y, 26, 26, color);
    gfx_rect(&gfx_screen, c.x, c.y, 26, 26, 0);
    for (int i = 0; i < 3; i++)
        button(btn_rect(i), btn_names[i], 0);
    /* Statuszeile */
    int sy = H - STATUS;
    gfx_fill(&gfx_screen, 0, sy, W, STATUS, RGB(205, 208, 218));
    gfx_fill(&gfx_screen, 0, sy, W, 1, RGB(90, 90, 100));
    char info[240];
    snprintf(info, sizeof(info), " %s%s   %s   (Strg+S speichern, Strg+Z r\xC3\xBC" "ckg\xC3\xA4ngig, Esc beenden)", path,
             modified ? " *" : "", msg);
    gfx_text(&gfx_screen, 2, sy + 1, info, strstr(msg, "Fehler") || strstr(msg, "Nicht") ? RGB(170, 0, 0) : RGB(30, 30, 40),
             GFX_TRANSPARENT);
}

static void present_bars(void)
{
    gfx_present(0, 0, W, BAR);
    gfx_present(0, H - STATUS, W, STATUS);
}

static void show_canvas(int x, int y, int w, int h)
{
    /* Ausschnitt der Leinwand (Koordinaten der Leinwand) auf den Bildschirm */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvas.w) w = canvas.w - x;
    if (y + h > canvas.h) h = canvas.h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; r++)
        memcpy(gfx_screen.px + (u64)(y + r + BAR) * (u64)W + (u64)x, canvas.px + (u64)(y + r) * (u64)canvas.w + (u64)x,
               (u64)w * 4);
    gfx_present(x, y + BAR, w, h);
}

static void redraw_all(void)
{
    draw_bar();
    show_canvas(0, 0, canvas.w, canvas.h);
    present_bars();
}

static void save_undo(void)
{
    memcpy(undo.px, canvas.px, (u64)canvas.w * (u64)canvas.h * 4);
}

static void clear_canvas(void)
{
    gfx_no_clip();
    gfx_fill(&canvas, 0, 0, canvas.w, canvas.h, back);
}

/* Fuellen mit Zeilen-Stapel (Scanline) */
static void flood(int x, int y, u32 c)
{
    u32 target = gfx_get(&canvas, x, y);
    if (target == c)
        return;
    u64 cap = 1 << 20;
    int *stack = gfx_alloc(cap * 2 * sizeof(int));
    if (!stack)
        return;
    u64 sp = 0;
    stack[sp++] = x;
    stack[sp++] = y;
    while (sp) {
        int py = stack[--sp], px = stack[--sp];
        u32 *row = canvas.px + (u64)py * (u64)canvas.w;
        if (row[px] != target)
            continue;
        int l = px, r = px;
        while (l > 0 && row[l - 1] == target)
            l--;
        while (r + 1 < canvas.w && row[r + 1] == target)
            r++;
        for (int i = l; i <= r; i++)
            row[i] = c;
        for (int dy = -1; dy <= 1; dy += 2) { /* darueber und darunter: je Abschnitt einen Startpunkt */
            int ny = py + dy;
            if (ny < 0 || ny >= canvas.h)
                continue;
            u32 *nrow = canvas.px + (u64)ny * (u64)canvas.w;
            for (int i = l; i <= r; i++) {
                if (nrow[i] == target && (i == l || nrow[i - 1] != target) && sp + 2 < cap * 2) {
                    stack[sp++] = i;
                    stack[sp++] = ny;
                }
            }
        }
    }
    gfx_free(stack, cap * 2 * sizeof(int));
}

static void shape(Surface *s, int t, int x0, int y0, int x1, int y1, u32 c, int d)
{
    if (t == T_LINE) {
        gfx_thick_line(s, x0, y0, x1, y1, d, c);
    } else if (t == T_RECT) {
        int ax = x0 < x1 ? x0 : x1, ay = y0 < y1 ? y0 : y1, w = (x0 > x1 ? x0 - x1 : x1 - x0) + 1,
            h = (y0 > y1 ? y0 - y1 : y1 - y0) + 1;
        if (filled) {
            gfx_fill(s, ax, ay, w, h, c);
        } else {
            for (int k = 0; k < d && k * 2 < w && k * 2 < h; k++)
                gfx_rect(s, ax + k, ay + k, w - 2 * k, h - 2 * k, c);
        }
    } else if (t == T_ELLIPSE) {
        if (filled)
            gfx_ellipse(s, x0, y0, x1, y1, c, 1);
        else
            for (int k = 0; k < d; k++)
                gfx_ellipse(s, x0 < x1 ? x0 + k : x0 - k, y0 < y1 ? y0 + k : y0 - k, x1 < x0 ? x1 + k : x1 - k,
                            y1 < y0 ? y1 + k : y1 - k, c, 0);
    }
}

static void do_save(void)
{
    int r = bmp_save(path, &canvas, 0, 0, canvas.w, canvas.h);
    if (r == 0) {
        modified = 0;
        snprintf(msg, sizeof(msg), "gespeichert (%dx%d)", canvas.w, canvas.h);
    } else {
        snprintf(msg, sizeof(msg), "Fehler beim Speichern (%d)", r);
    }
}

void _start(int argc, char **argv)
{
    if (argc > 1)
        snprintf(path, sizeof(path), "%s", argv[1]);
    if (gfx_open_window(0, 0, "Malen") != 0)
        sys_exit(1);
    sys_tty_fg(0); /* Strg+C beendet das Programm nicht aus Versehen */
    W = gfx_screen.w;
    H = gfx_screen.h;
    if (surface_new(&canvas, W, H - BAR - STATUS) != 0 || surface_new(&undo, W, H - BAR - STATUS) != 0) {
        gfx_close();
        fprintf(2, "paint: kein Speicher\n");
        sys_exit(1);
    }
    clear_canvas();
    Surface img = {0, 0, 0};
    char err[80];
    Stat st;
    if (sys_stat(path, &st) == 0) {
        if (bmp_load(path, &img, err, sizeof(err)) == 0) { /* vorhandenes Bild laden (oben links) */
            for (int y = 0; y < img.h && y < canvas.h; y++)
                memcpy(canvas.px + (u64)y * (u64)canvas.w, img.px + (u64)y * (u64)img.w,
                       (u64)(img.w < canvas.w ? img.w : canvas.w) * 4);
            snprintf(msg, sizeof(msg), "geladen (%dx%d)", img.w, img.h);
            surface_free(&img);
        } else {
            snprintf(msg, sizeof(msg), "Fehler: %s", err);
        }
    } else {
        snprintf(msg, sizeof(msg), "neues Bild - Strg+S speichert");
    }
    save_undo();
    redraw_all();

    int drawing = 0, sx = 0, sy = 0, lx = 0, ly = 0;
    for (;;) {
        Event e;
        if (!gfx_wait(&e, -1))
            continue;
        int cx = e.x, cy = e.y - BAR; /* Leinwand-Koordinaten */
        if (e.type == EV_CLOSE) { /* Schliessen-Knopf des Fensters: wie Esc */
            e.type = EV_KEY;
            e.key = 0x1B;
        }
        if (e.type == EV_KEY) {
            if (e.key == 0x1B) {
                if (modified && !quit_armed) {
                    quit_armed = 1;
                    snprintf(msg, sizeof(msg), gfx_windowed() ? "Nicht gespeichert! Nochmal schliessen = beenden, Strg+S = speichern"
                                                     : "Nicht gespeichert! Esc nochmal = beenden, Strg+S = speichern");
                    draw_bar();
                    present_bars();
                    continue;
                }
                break;
            }
            quit_armed = 0;
            if (e.key == 19) /* Strg+S */
                do_save();
            else if (e.key == 26) { /* Strg+Z */
                memcpy(canvas.px, undo.px, (u64)canvas.w * (u64)canvas.h * 4);
                show_canvas(0, 0, canvas.w, canvas.h);
                snprintf(msg, sizeof(msg), "rueckgaengig");
            }
            draw_bar();
            present_bars();
            continue;
        }
        if (e.type == EV_DOWN && e.y < BAR) { /* Werkzeugleiste */
            quit_armed = 0;
            for (int i = 0; i < T_COUNT; i++)
                if (in(tool_rect(i), e.x, e.y))
                    tool = i;
            if (in(fill_rect(), e.x, e.y))
                filled = !filled;
            for (int i = 0; i < 4; i++)
                if (in(size_rect(i), e.x, e.y))
                    size_i = i;
            for (int i = 0; i < 16; i++) {
                if (in(color_rect(i), e.x, e.y)) {
                    if (e.button == 2)
                        back = palette[i];
                    else
                        color = palette[i];
                }
            }
            if (in(btn_rect(0), e.x, e.y)) {
                save_undo();
                clear_canvas();
                modified = 1;
                show_canvas(0, 0, canvas.w, canvas.h);
            } else if (in(btn_rect(1), e.x, e.y)) {
                do_save();
            } else if (in(btn_rect(2), e.x, e.y)) {
                if (!modified || quit_armed)
                    break;
                quit_armed = 1;
                snprintf(msg, sizeof(msg), "Nicht gespeichert! Nochmal 'Ende' = beenden");
            }
            draw_bar();
            present_bars();
            continue;
        }
        if (e.type == EV_DOWN && e.button == 1 && cy >= 0 && cy < canvas.h) {
            quit_armed = 0;
            save_undo();
            modified = 1;
            if (tool == T_FILL) {
                flood(cx, cy, color);
                show_canvas(0, 0, canvas.w, canvas.h);
                draw_bar();
                present_bars();
                continue;
            }
            drawing = 1;
            sx = lx = cx;
            sy = ly = cy;
            if (tool == T_PEN || tool == T_ERASER) {
                gfx_no_clip();
                gfx_fill_circle(&canvas, cx, cy, sizes[size_i] / 2, tool == T_ERASER ? back : color);
                int r = sizes[size_i] / 2 + 1;
                show_canvas(cx - r, cy - r, 2 * r + 1, 2 * r + 1);
            }
        } else if (e.type == EV_MOVE && drawing) {
            if (cy < 0)
                cy = 0;
            if (cy >= canvas.h)
                cy = canvas.h - 1;
            int d = tool == T_ERASER ? sizes[size_i] + 8 : sizes[size_i];
            if (tool == T_PEN || tool == T_ERASER) {
                gfx_no_clip();
                gfx_thick_line(&canvas, lx, ly, cx, cy, d, tool == T_ERASER ? back : color);
                int r = d / 2 + 1;
                int x0 = lx < cx ? lx : cx, y0 = ly < cy ? ly : cy, x1 = lx > cx ? lx : cx, y1 = ly > cy ? ly : cy;
                show_canvas(x0 - r, y0 - r, x1 - x0 + 2 * r + 1, y1 - y0 + 2 * r + 1);
                lx = cx;
                ly = cy;
            } else { /* Vorschau: Leinwand + Form auf dem Bildschirm */
                int x0 = sx < lx ? sx : lx, y0 = sy < ly ? sy : ly, x1 = sx > lx ? sx : lx, y1 = sy > ly ? sy : ly;
                int m = sizes[size_i] + 2;
                show_canvas(x0 - m, y0 - m, x1 - x0 + 2 * m + 1, y1 - y0 + 2 * m + 1); /* alte Vorschau weg */
                lx = cx;
                ly = cy;
                gfx_set_clip(0, BAR, W, canvas.h);
                Surface view = gfx_screen;
                /* die Form direkt in den Bildschirm-Puffer (um BAR verschoben) */
                shape(&view, tool, sx, sy + BAR, lx, ly + BAR, color, sizes[size_i]);
                gfx_no_clip();
                x0 = sx < lx ? sx : lx;
                y0 = sy < ly ? sy : ly;
                x1 = sx > lx ? sx : lx;
                y1 = sy > ly ? sy : ly;
                gfx_present(x0 - m, y0 - m + BAR, x1 - x0 + 2 * m + 1, y1 - y0 + 2 * m + 1);
            }
        } else if (e.type == EV_UP && e.button == 1 && drawing) {
            drawing = 0;
            if (tool != T_PEN && tool != T_ERASER) {
                gfx_no_clip();
                shape(&canvas, tool, sx, sy, lx, ly, color, sizes[size_i]);
                int m = sizes[size_i] + 2;
                int x0 = sx < lx ? sx : lx, y0 = sy < ly ? sy : ly, x1 = sx > lx ? sx : lx, y1 = sy > ly ? sy : ly;
                show_canvas(x0 - m, y0 - m, x1 - x0 + 2 * m + 1, y1 - y0 + 2 * m + 1);
            }
            draw_bar();
            present_bars();
        }
    }
    gfx_close();
    sys_exit(0);
}
