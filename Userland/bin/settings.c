#include "gfx.h"
#include "settings.h"
#include "ui.h"
#include "winproto.h"

/* settings: Einstellungen des Systems im Stil des Desktops - links die Bereiche, rechts Karten mit den Schaltern.
 *   Anzeige      Aufloesung (mit Intel-Treiber sofort, mit Rueckfrage und Ruecksprung nach 15 s; sonst ab dem
 *                naechsten Start ueber cmdline.txt), Groesse der Oberflaeche (ab dem naechsten Start des Desktops)
 *   Zeiger       Groesse des Mauszeigers mit Vorschau
 *   Taskleiste   Groesse, Sekunden und Datum in der Uhr
 *   Hintergrund  Farbthema des Hintergrunds
 *   Tastatur     Layout (sofort, dauerhaft in cmdline.txt)
 *   Info         System und wo die Einstellungen liegen
 * Gespeichert wird in settings.cfg (settings.h); der Desktop uebernimmt die Aenderungen sofort (WP_SETTINGS).
 * Pfeil hoch/runter wechselt den Bereich, Esc schliesst. */

enum { P_DISPLAY, P_POINTER, P_DOCK, P_WALL, P_KEYS, P_INFO, NPAGES };
static const char *const page_name[NPAGES] = {"Anzeige", "Zeiger", "Taskleiste", "Hintergrund", "Tastatur", "Info"};

enum { H_PAGE = 1, H_MODE, H_UISCALE, H_CURSOR, H_DOCKSIZE, H_SECONDS, H_DATE, H_WALL, H_KBD, H_RESTART, H_KEEP,
       H_REVERT, H_LIST };

typedef struct {
    int x, y, w, h, id, arg;
} Hit;

#define MAXMODES 48
#define MAXHITS  160
#define CONFIRM_S 15

static Settings  cfg;
static int       page;
static VideoInfo modes[MAXMODES];
static int       nmodes, live;      /* live: Intel-Treiber, Modi mit Bildrate, sofort umschaltbar */
static int       boot_pick = -1;    /* ohne Intel-Treiber: fuer den naechsten Start gewaehlt */
static int       list_top;          /* erste sichtbare Zeile der Aufloesungen */
static int       list_rows, list_y0, list_y1;
static char      kbd[16];
static char      note[200];         /* Hinweis unten */
static int       restart_hint;      /* Knopf "Jetzt neu starten" zeigen */
static Hit       hits[MAXHITS];
static int       nhits;
static Surface   thumbs[UI_WALLPAPERS];

/* Rueckfrage nach dem Umschalten der Aufloesung */
static int       confirm;
static s64       confirm_end;
static VideoInfo prev_mode, new_mode;

static void hit(int x, int y, int w, int h, int id, int arg)
{
    if (nhits < MAXHITS)
        hits[nhits++] = (Hit){x, y, w, h, id, arg};
}

static void load_modes(void)
{
    nmodes = 0;
    for (VideoInfo vi; nmodes < MAXMODES && sys_videoinfo((u64)nmodes, &vi) == 0;)
        modes[nmodes++] = vi;
    live = nmodes && modes[0].hz100;
    for (int i = 1; i < nmodes; i++) { /* groesste zuerst, bei gleicher Groesse die hoehere Bildrate */
        VideoInfo x = modes[i];
        int j = i - 1;
        while (j >= 0 && ((u64)modes[j].width * modes[j].height < (u64)x.width * x.height ||
                          ((u64)modes[j].width * modes[j].height == (u64)x.width * x.height && modes[j].hz100 < x.hz100))) {
            modes[j + 1] = modes[j];
            j--;
        }
        modes[j + 1] = x;
    }
}

static int current_mode(VideoInfo *out)
{
    VideoInfo vi;
    for (u64 i = 0; sys_videoinfo(i, &vi) == 0; i++)
        if (vi.current) {
            *out = vi;
            return 1;
        }
    return 0;
}

/* ---------- Speichern ---------- */

static void apply(void)
{
    if (settings_save(&cfg) != 0)
        snprintf(note, sizeof(note), "Nicht gespeichert: kein Boot-Volume und keine Platte gefunden (gilt bis zum Neustart).");
    if (gfx_desktop_request(WP_SETTINGS, 0, 0, 0) != 0)
        gfx_set_cursor_size(cfg.cursor); /* ohne Desktop: der Zeiger gehoert uns */
}

/* Schluessel in cmdline.txt aller Boot-Volumes setzen (NULL = entfernen); 0 = geschrieben */
static int save_boot(const char *key, const char *val, const char *key2, const char *val2)
{
    char dirs[4][40];
    int n = find_boot_volumes(dirs, 4), rc = n ? 0 : -1;
    for (int i = 0; i < n; i++) {
        if (boot_cmdline_set(dirs[i], key, val) != 0)
            rc = -1;
        if (key2 && boot_cmdline_set(dirs[i], key2, val2) != 0)
            rc = -1;
    }
    return rc;
}

static void mode_text(const VideoInfo *m, char *out, int n)
{
    snprintf(out, (size_t)n, "%u \xC3\x97 %u", m->width, m->height);
}

static void keep_mode(void)
{
    confirm = 0;
    char val[32];
    snprintf(val, sizeof(val), "%ux%u@%u", new_mode.width, new_mode.height, (new_mode.hz100 + 50) / 100);
    if (save_boot("igdmode=", val, "mode=", "max") == 0)
        snprintf(note, sizeof(note), "Aufl\xC3\xB6sung %u \xC3\x97 %u gespeichert (gilt auch nach dem Neustart).",
                 new_mode.width, new_mode.height);
    else
        snprintf(note, sizeof(note), "Aufl\xC3\xB6sung gilt bis zum Neustart (cmdline.txt nicht beschreibbar).");
}

static void revert_mode(void)
{
    confirm = 0;
    gfx_desktop_request(WP_SETMODE, (int)prev_mode.width, (int)prev_mode.height, (int)prev_mode.hz100);
    snprintf(note, sizeof(note), "Zur\xC3\xBC" "ck zu %u \xC3\x97 %u.", prev_mode.width, prev_mode.height);
}

static void pick_mode(int i)
{
    if (i < 0 || i >= nmodes || confirm)
        return;
    if (live) {
        if (modes[i].current)
            return;
        if (!gfx_windowed()) {
            snprintf(note, sizeof(note), "Sofort umschalten geht nur im Desktop (sonst: resolution in der Shell).");
            return;
        }
        if (!current_mode(&prev_mode))
            return;
        new_mode = modes[i];
        gfx_desktop_request(WP_SETMODE, (int)new_mode.width, (int)new_mode.height, (int)new_mode.hz100);
        confirm = 1;
        confirm_end = sys_time() + CONFIRM_S;
        note[0] = 0;
        return;
    }
    char val[24];
    snprintf(val, sizeof(val), "%ux%u", modes[i].width, modes[i].height);
    boot_pick = i;
    if (save_boot("mode=", modes[i].current ? NULL : val, 0, 0) == 0) {
        snprintf(note, sizeof(note), "%u \xC3\x97 %u wird beim n\xC3\xA4" "chsten Start eingestellt.", modes[i].width,
                 modes[i].height);
        restart_hint = !modes[i].current;
    } else {
        snprintf(note, sizeof(note), "Kein Boot-Volume gefunden: die Aufl\xC3\xB6sung l\xC3\xA4sst sich nicht speichern.");
    }
}

static void pick_kbd(int i)
{
    static const char *const names[3] = {"de", "us", "uk"};
    char cur[16];
    if (sys_keymap(names[i], cur) != 0)
        return;
    snprintf(kbd, sizeof(kbd), "%s", cur);
    if (save_boot("kbd=", i == 1 ? NULL : names[i], 0, 0) == 0)
        snprintf(note, sizeof(note), "Tastaturlayout gespeichert (gilt sofort und nach dem Neustart).");
    else
        snprintf(note, sizeof(note), "Tastaturlayout gilt bis zum Neustart (kein Boot-Volume zum Speichern).");
}

/* ---------- Zeichnen: Bausteine ---------- */

static void card(Surface *s, int x, int y, int w, int h)
{
    gfx_shadow(s, x, y + U(1), w, h, U(10), U(4), 18);
    gfx_round_rect(s, x, y, w, h, U(10), C_WINDOW, 255);
    gfx_round_frame(s, x, y, w, h, U(10), 0x000000, 20);
}

static void section(Surface *s, int x, int y, const char *t)
{
    text_draw(s, font_bold, FS_SMALL, x + U(4), y, t, C_TEXT2);
}

static int row_h(void)
{
    return U(46);
}

static void row_label(Surface *s, int x, int y, const char *label, const char *sub)
{
    int th = text_height(font_ui, FS), sh = sub ? text_height(font_ui, FS_SMALL) : 0;
    int ty = y + (row_h() - th - sh) / 2;
    text_draw(s, font_ui, FS, x + U(16), ty, label, C_TEXT);
    if (sub)
        text_draw(s, font_ui, FS_SMALL, x + U(16), ty + th, sub, C_TEXT2);
}

static void divider(Surface *s, int x, int y, int w)
{
    gfx_blend_fill(s, x + U(16), y, w - U(16), 1, 0x000000, 22);
}

/* Segmente, rechtsbuendig bei xr, mittig auf yc */
static void segmented(Surface *s, int xr, int yc, const char *const *items, int n, int sel, int id)
{
    int pad = U(12), h = U(28), w[8], total = U(4);
    for (int i = 0; i < n; i++) {
        w[i] = text_width(font_ui, FS_SMALL, items[i]) + 2 * pad;
        total += w[i];
    }
    int x = xr - total, y = yc - h / 2;
    gfx_round_rect(s, x, y, total, h, U(8), 0x767680, 30);
    x += U(2);
    for (int i = 0; i < n; i++) {
        if (i == sel) {
            gfx_shadow(s, x, y + U(2) + U(1), w[i], h - U(4), U(6), U(3), 35);
            gfx_round_rect(s, x, y + U(2), w[i], h - U(4), U(6), 0xFFFFFF, 255);
        } else if (i && i - 1 != sel) {
            gfx_blend_fill(s, x, y + U(8), 1, h - U(16), 0x000000, 40);
        }
        Font *f = i == sel ? font_bold : font_ui;
        int tw = text_width(f, FS_SMALL, items[i]);
        text_draw(s, f, FS_SMALL, x + (w[i] - tw) / 2, y + (h - text_height(f, FS_SMALL)) / 2, items[i], C_TEXT);
        hit(x, y, w[i], h, id, i);
        x += w[i];
    }
}

static void toggle(Surface *s, int xr, int yc, int on, int id)
{
    int w = U(42), h = U(24), x = xr - w, y = yc - h / 2;
    gfx_round_rect(s, x, y, w, h, h / 2, on ? 0x34C759 : 0xD8D8DC, 255);
    float r = (float)h / 2 - U(2), cx = on ? (float)(x + w) - (float)h / 2 : (float)x + (float)h / 2;
    gfx_disc(s, cx, (float)y + (float)h / 2 + U(1), r, 0x000000, 30);
    gfx_disc(s, cx, (float)y + (float)h / 2, r, 0xFFFFFF, 255);
    hit(x, y, w, h, id, 0);
}

static void check_mark(Surface *s, float x, float y, float k, u32 c)
{
    gfx_capsule(s, x, y, x + 4 * k, y + 4 * k, 2 * k, c, 255);
    gfx_capsule(s, x + 4 * k, y + 4 * k, x + 11 * k, y - 4 * k, 2 * k, c, 255);
}

static void button(Surface *s, int x, int y, const char *t, int primary, int id)
{
    int w = text_width(font_ui, FS, t) + U(28), h = U(30);
    if (primary) {
        gfx_round_rect_grad(s, x, y, w, h, U(7), 0x3B8BF7, C_ACCENT, 255);
    } else {
        gfx_round_rect(s, x, y, w, h, U(7), 0xFFFFFF, 255);
        gfx_round_frame(s, x, y, w, h, U(7), 0x000000, 35);
    }
    text_draw(s, font_ui, FS, x + U(14), y + (h - text_height(font_ui, FS)) / 2, t, primary ? 0xFFFFFF : C_TEXT);
    hit(x, y, w, h, id, 0);
}

/* kleine Symbole der Bereiche: farbiges Quadrat mit Zeichen */
static void page_icon(Surface *s, int p, int x, int y, int sz)
{
    static const u32 top[NPAGES] = {0x5AC8FA, 0xC17BF5, 0x8E8E93, 0xFFB340, 0x9A9AA0, 0x64B5FF};
    static const u32 bot[NPAGES] = {0x0A84FF, 0x8E44CF, 0x5A5A5F, 0xFF4F6A, 0x636366, 0x2F6FE0};
    float S = (float)sz, k = S / 22;
    gfx_round_rect_grad(s, x, y, sz, sz, sz / 4, top[p], bot[p], 255);
    float cx = (float)x + S / 2, cy = (float)y + S / 2;
    switch (p) {
    case P_DISPLAY:
        gfx_round_rect(s, (int)(cx - 7 * k), (int)(cy - 6 * k), (int)(14 * k), (int)(9 * k), (int)(2 * k), 0xFFFFFF, 255);
        gfx_capsule(s, cx - 3 * k, cy + 6 * k, cx + 3 * k, cy + 6 * k, 1.6f * k, 0xFFFFFF, 255);
        break;
    case P_POINTER:
        gfx_capsule(s, cx - 3 * k, cy - 6 * k, cx - 3 * k, cy + 5 * k, 2 * k, 0xFFFFFF, 255);
        gfx_capsule(s, cx - 3 * k, cy - 6 * k, cx + 5 * k, cy + 2 * k, 2 * k, 0xFFFFFF, 255);
        gfx_capsule(s, cx - 3 * k, cy + 5 * k, cx + 5 * k, cy + 2 * k, 2 * k, 0xFFFFFF, 255);
        break;
    case P_DOCK:
        gfx_round_rect(s, (int)(cx - 7 * k), (int)(cy + 2 * k), (int)(14 * k), (int)(4 * k), (int)(2 * k), 0xFFFFFF, 255);
        gfx_round_rect(s, (int)(cx - 7 * k), (int)(cy - 6 * k), (int)(14 * k), (int)(6 * k), (int)(1.5f * k), 0xFFFFFF, 140);
        break;
    case P_WALL:
        gfx_disc(s, cx + 3 * k, cy - 3 * k, 2.4f * k, 0xFFFFFF, 255);
        gfx_capsule(s, cx - 7 * k, cy + 5 * k, cx - 1 * k, cy - 1 * k, 2 * k, 0xFFFFFF, 255);
        gfx_capsule(s, cx - 1 * k, cy - 1 * k, cx + 7 * k, cy + 5 * k, 2 * k, 0xFFFFFF, 255);
        break;
    case P_KEYS:
        gfx_round_rect(s, (int)(cx - 8 * k), (int)(cy - 5 * k), (int)(16 * k), (int)(10 * k), (int)(2 * k), 0xFFFFFF, 255);
        for (int i = 0; i < 4; i++)
            gfx_fill(s, (int)(cx - 6 * k + (float)i * 3.5f * k), (int)(cy - 3 * k), (int)(2 * k), (int)(2 * k), bot[p]);
        gfx_fill(s, (int)(cx - 4 * k), (int)(cy + 1.5f * k), (int)(8 * k), (int)(1.6f * k), bot[p]);
        break;
    default:
        gfx_disc(s, cx, cy - 4.5f * k, 1.6f * k, 0xFFFFFF, 255);
        gfx_capsule(s, cx, cy - 1 * k, cx, cy + 6 * k, 2.6f * k, 0xFFFFFF, 255);
        break;
    }
}

/* ---------- Seiten ---------- */

static int page_display(Surface *s, int x, int y, int w)
{
    char t[48];
    section(s, x, y, "AUFL\xC3\x96SUNG");
    y += U(22);
    int rh = U(36), vis = nmodes < 6 ? nmodes : 6;
    if (!nmodes) {
        card(s, x, y, w, row_h());
        VideoInfo cur;
        if (current_mode(&cur))
            mode_text(&cur, t, sizeof(t));
        else
            snprintf(t, sizeof(t), "unbekannt");
        row_label(s, x, y, t, "Die Firmware meldet keine weiteren Modi");
        y += row_h();
    } else {
        if (list_top > nmodes - vis)
            list_top = nmodes - vis;
        if (list_top < 0)
            list_top = 0;
        card(s, x, y, w, vis * rh);
        list_rows = vis;
        list_y0 = y;
        list_y1 = y + vis * rh;
        hit(x, y, w, vis * rh, H_LIST, 0);
        for (int r = 0; r < vis; r++) {
            int i = list_top + r, ry = y + r * rh;
            const VideoInfo *m = &modes[i];
            if (r)
                divider(s, x, ry, w);
            mode_text(m, t, sizeof(t));
            int th = text_height(font_ui, FS), ty = ry + (rh - th) / 2;
            text_draw(s, m->current ? font_bold : font_ui, FS, x + U(40), ty, t, C_TEXT);
            int chosen = live ? (int)m->current : boot_pick >= 0 ? boot_pick == i : (int)m->current;
            if (chosen)
                check_mark(s, (float)(x + U(14)), (float)(ry + rh / 2), (float)U(1), C_ACCENT);
            char right[32] = "";
            if (m->hz100)
                snprintf(right, sizeof(right), "%u Hz", (m->hz100 + 50) / 100);
            if (m->current && !live)
                snprintf(right, sizeof(right), "aktuell");
            int rw = text_width(font_ui, FS_SMALL, right);
            text_draw(s, font_ui, FS_SMALL, x + w - U(16) - rw - (nmodes > vis ? U(10) : 0),
                      ry + (rh - text_height(font_ui, FS_SMALL)) / 2, right, C_TEXT2);
            hit(x, ry, w - U(16), rh, H_MODE, i);
        }
        if (nmodes > vis)
            ui_scrollbar(s, x + w - U(4), y + U(4), vis * rh - U(8), nmodes, vis, list_top);
        y += vis * rh;
    }
    text_draw(s, font_ui, FS_SMALL, x + U(4), y + U(6),
              live ? "Schaltet sofort um; ohne Best\xC3\xA4tigung geht es nach 15 Sekunden zur\xC3\xBC" "ck."
                   : "Der Bootloader stellt die Aufl\xC3\xB6sung ein: gilt ab dem n\xC3\xA4" "chsten Start.",
              C_TEXT2);
    y += U(36);

    section(s, x, y, "OBERFL\xC3\x84" "CHE");
    y += U(22);
    card(s, x, y, w, row_h());
    row_label(s, x, y, "Gr\xC3\xB6\xC3\x9F" "e", "Ab dem n\xC3\xA4" "chsten Start des Desktops");
    static const char *const sc[4] = {"Automatisch", "100 %", "125 %", "150 %"};
    int sel = cfg.ui_scale == 100 ? 1 : cfg.ui_scale == 125 ? 2 : cfg.ui_scale == 150 ? 3 : 0;
    segmented(s, x + w - U(14), y + row_h() / 2, sc, 4, sel, H_UISCALE);
    return y + row_h();
}

static const int cursor_pcts[6] = {0, 100, 125, 150, 200, 250};

static int page_pointer(Surface *s, int x, int y, int w)
{
    section(s, x, y, "MAUSZEIGER");
    y += U(22);
    card(s, x, y, w, row_h());
    row_label(s, x, y, "Gr\xC3\xB6\xC3\x9F" "e", 0);
    static const char *const it[6] = {"Auto", "100 %", "125 %", "150 %", "200 %", "250 %"};
    int sel = 0;
    for (int i = 0; i < 6; i++)
        if (cfg.cursor == cursor_pcts[i])
            sel = i;
    segmented(s, x + w - U(14), y + row_h() / 2, it, 6, sel, H_CURSOR);
    y += row_h() + U(20);

    section(s, x, y, "VORSCHAU");
    y += U(22);
    int ph = U(150);
    card(s, x, y, w, ph);
    gfx_round_rect_grad(s, x + U(10), y + U(10), w - U(20), ph - U(20), U(8), 0xEEF3FB, 0xDCE6F5, 255);
    /* ein Knopf zum "Zeigen" und daneben alle Groessen zum Vergleich */
    int bx = x + U(40), by = y + ph / 2 - U(16);
    gfx_round_rect_grad(s, bx, by, U(120), U(32), U(7), 0x3B8BF7, C_ACCENT, 255);
    text_draw(s, font_ui, FS, bx + U(22), by + (U(32) - text_height(font_ui, FS)) / 2, "Schaltfl\xC3\xA4" "che", 0xFFFFFF);
    gfx_cursor_draw(s, bx + U(84), by + U(18), cfg.cursor);
    int gx = x + U(230);
    for (int i = 1; i < 6; i++) {
        int cx = gx + (i - 1) * U(56);
        gfx_cursor_draw(s, cx, y + U(40), cursor_pcts[i]);
        char t[8];
        snprintf(t, sizeof(t), "%d", cursor_pcts[i]);
        text_draw(s, cfg.cursor == cursor_pcts[i] ? font_bold : font_ui, FS_SMALL, cx, y + ph - U(34), t,
                  cfg.cursor == cursor_pcts[i] ? C_ACCENT : C_TEXT2);
    }
    return y + ph;
}

static int page_dock(Surface *s, int x, int y, int w)
{
    section(s, x, y, "TASKLEISTE");
    y += U(22);
    card(s, x, y, w, 3 * row_h());
    row_label(s, x, y, "Gr\xC3\xB6\xC3\x9F" "e", 0);
    static const char *const it[3] = {"Klein", "Normal", "Gro\xC3\x9F"};
    segmented(s, x + w - U(14), y + row_h() / 2, it, 3, cfg.dock < 100 ? 0 : cfg.dock > 100 ? 2 : 1, H_DOCKSIZE);
    divider(s, x, y + row_h(), w);
    row_label(s, x, y + row_h(), "Uhr mit Sekunden", 0);
    toggle(s, x + w - U(16), y + row_h() * 3 / 2, cfg.seconds, H_SECONDS);
    divider(s, x, y + 2 * row_h(), w);
    row_label(s, x, y + 2 * row_h(), "Datum unter der Uhrzeit", 0);
    toggle(s, x + w - U(16), y + row_h() * 5 / 2, cfg.date, H_DATE);
    return y + 3 * row_h();
}

static void make_thumb(int t, int w, int h)
{
    if (thumbs[t].px && thumbs[t].w == w && thumbs[t].h == h)
        return;
    if (thumbs[t].px)
        surface_free(&thumbs[t]);
    if (surface_new(&thumbs[t], w, h) != 0)
        return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            thumbs[t].px[(u64)y * (u64)w + (u64)x] = ui_wallpaper_at(t, (float)x / (float)w, (float)y / (float)h);
}

static int page_wall(Surface *s, int x, int y, int w)
{
    section(s, x, y, "FARBTHEMA");
    y += U(22);
    int gap = U(16), tw = (w - 2 * gap) / 3, th = tw * 10 / 16, rowh = th + U(34);
    for (int t = 0; t < UI_WALLPAPERS; t++) {
        int cx = x + (t % 3) * (tw + gap), cy = y + (t / 3) * rowh;
        make_thumb(t, tw, th);
        if (cfg.wallpaper == t) {
            gfx_round_rect(s, cx - U(4), cy - U(4), tw + U(8), th + U(8), U(12), C_ACCENT, 255);
            gfx_round_rect(s, cx - U(2), cy - U(2), tw + U(4), th + U(4), U(10), 0xF5F5F7, 255);
        }
        gfx_shadow(s, cx, cy + U(1), tw, th, U(8), U(4), 30);
        if (thumbs[t].px)
            gfx_blit_round(s, &thumbs[t], 0, 0, cx, cy, tw, th, U(8));
        /* angedeutete Taskleiste */
        gfx_round_rect(s, cx + tw / 4, cy + th - th / 6 - U(3), tw / 2, th / 7, U(3), 0xFFFFFF, 150);
        const char *n = ui_wallpaper_name(t);
        Font *f = cfg.wallpaper == t ? font_bold : font_ui;
        text_draw(s, f, FS_SMALL, cx + (tw - text_width(f, FS_SMALL, n)) / 2, cy + th + U(8), n,
                  cfg.wallpaper == t ? C_TEXT : C_TEXT2);
        hit(cx, cy, tw, th + U(26), H_WALL, t);
    }
    return y + ((UI_WALLPAPERS + 2) / 3) * rowh;
}

static int page_keys(Surface *s, int x, int y, int w)
{
    section(s, x, y, "TASTATUR");
    y += U(22);
    card(s, x, y, w, row_h());
    row_label(s, x, y, "Layout", 0);
    static const char *const it[3] = {"Deutsch", "Englisch (US)", "Englisch (UK)"};
    int sel = strcmp(kbd, "us") == 0 ? 1 : strcmp(kbd, "uk") == 0 ? 2 : 0;
    segmented(s, x + w - U(14), y + row_h() / 2, it, 3, sel, H_KBD);
    y += row_h();
    text_draw(s, font_ui, FS_SMALL, x + U(4), y + U(6),
              "Gilt sofort f\xC3\xBCr alle Programme und dauerhaft (cmdline.txt auf dem Boot-Volume).", C_TEXT2);
    return y + U(30);
}

static int page_info(Surface *s, int x, int y, int w)
{
    section(s, x, y, "SYSTEM");
    y += U(22);
    char v[4][96];
    VideoInfo cur = {0};
    int sw, sh;
    gfx_display_size(&sw, &sh);
    if (current_mode(&cur))
        snprintf(v[0], sizeof(v[0]), "%u \xC3\x97 %u%s", cur.width, cur.height, cur.hz100 ? "" : " (Firmware)");
    else
        snprintf(v[0], sizeof(v[0]), "%d \xC3\x97 %d", sw, sh);
    if (cur.hz100)
        snprintf(v[0] + strlen(v[0]), sizeof(v[0]) - strlen(v[0]), ", %u Hz", (cur.hz100 + 50) / 100);
    snprintf(v[1], sizeof(v[1]), "%s", live ? "Intel (Aufl\xC3\xB6sung sofort umschaltbar)" : "Bildspeicher der Firmware");
    snprintf(v[2], sizeof(v[2]), "%d %%", ui_pct);
    const char *f = settings_file();
    snprintf(v[3], sizeof(v[3]), "%s", f[0] ? f : "nirgends (kein Boot-Volume, keine Platte)");
    static const char *const lab[4] = {"Bildschirm", "Grafik", "Oberfl\xC3\xA4" "che", "Einstellungen"};
    card(s, x, y, w, 4 * row_h());
    for (int i = 0; i < 4; i++) {
        int ry = y + i * row_h();
        if (i)
            divider(s, x, ry, w);
        row_label(s, x, ry, lab[i], 0);
        int vw = text_width(font_ui, FS, v[i]);
        text_draw(s, font_ui, FS, x + w - U(16) - vw, ry + (row_h() - text_height(font_ui, FS)) / 2, v[i], C_TEXT2);
    }
    return y + 4 * row_h();
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    nhits = 0;
    gfx_fill(s, 0, 0, s->w, s->h, 0xF5F5F7);

    /* Seitenleiste */
    int sbw = U(196);
    gfx_fill(s, 0, 0, sbw, s->h, 0xEDEDF0);
    gfx_fill(s, sbw - 1, 0, 1, s->h, C_HAIRLINE);
    int y = U(14), ih = U(34);
    for (int i = 0; i < NPAGES; i++) {
        if (i == page)
            gfx_round_rect(s, U(8), y, sbw - U(16), ih, U(8), C_ACCENT, 255);
        page_icon(s, i, U(18), y + (ih - U(22)) / 2, U(22));
        text_draw(s, i == page ? font_bold : font_ui, FS, U(50), y + (ih - text_height(font_ui, FS)) / 2, page_name[i],
                  i == page ? 0xFFFFFF : C_TEXT);
        hit(U(8), y, sbw - U(16), ih, H_PAGE, i);
        y += ih + U(2);
    }

    /* Inhalt */
    int x = sbw + U(26), w = s->w - x - U(26);
    text_draw(s, font_bold, U(22), x, U(16), page_name[page], C_TEXT);
    y = U(62);
    switch (page) {
    case P_DISPLAY: page_display(s, x, y, w); break;
    case P_POINTER: page_pointer(s, x, y, w); break;
    case P_DOCK: page_dock(s, x, y, w); break;
    case P_WALL: page_wall(s, x, y, w); break;
    case P_KEYS: page_keys(s, x, y, w); break;
    default: page_info(s, x, y, w); break;
    }

    /* Hinweis unten, ggf. mit "Jetzt neu starten" */
    int fy = s->h - U(46);
    if (note[0] || restart_hint) {
        gfx_blend_fill(s, sbw, fy - U(8), s->w - sbw, 1, 0x000000, 20);
        int tx = x;
        if (restart_hint) {
            button(s, s->w - U(26) - text_width(font_ui, FS, "Jetzt neu starten") - U(28), fy, "Jetzt neu starten", 0,
                   H_RESTART);
        }
        text_draw(s, font_ui, FS_SMALL, tx, fy + (U(30) - text_height(font_ui, FS_SMALL)) / 2, note, C_TEXT2);
    }

    /* Rueckfrage nach dem Umschalten */
    if (confirm) {
        nhits = 0;
        gfx_blend_fill(s, 0, 0, s->w, s->h, 0x000000, 60);
        int dw = U(380), dh = U(150), dx = (s->w - dw) / 2, dy = (s->h - dh) / 2;
        gfx_shadow(s, dx, dy + U(4), dw, dh, U(14), U(20), 70);
        gfx_round_rect(s, dx, dy, dw, dh, U(14), 0xFBFBFD, 255);
        char t[96];
        snprintf(t, sizeof(t), "Aufl\xC3\xB6sung %u \xC3\x97 %u beibehalten?", new_mode.width, new_mode.height);
        text_draw(s, font_bold, FS, dx + U(20), dy + U(20), t, C_TEXT);
        s64 left = confirm_end - sys_time();
        snprintf(t, sizeof(t), "Sonst zur\xC3\xBC" "ck zu %u \xC3\x97 %u in %lld s.", prev_mode.width, prev_mode.height,
                 (long long)(left < 0 ? 0 : left));
        text_draw(s, font_ui, FS_SMALL, dx + U(20), dy + U(50), t, C_TEXT2);
        int by = dy + dh - U(48);
        int kw = text_width(font_ui, FS, "Beibehalten") + U(28);
        button(s, dx + dw - U(20) - kw, by, "Beibehalten", 1, H_KEEP);
        button(s, dx + dw - U(32) - kw - text_width(font_ui, FS, "Zur\xC3\xBC" "cksetzen") - U(28), by,
               "Zur\xC3\xBC" "cksetzen", 0, H_REVERT);
    }
    gfx_present_all();
}

/* ---------- Eingaben ---------- */

static void click(int mx, int my)
{
    for (int i = nhits - 1; i >= 0; i--) {
        Hit *h = &hits[i];
        if (mx < h->x || my < h->y || mx >= h->x + h->w || my >= h->y + h->h)
            continue;
        switch (h->id) {
        case H_PAGE:
            page = h->arg;
            note[0] = 0;
            restart_hint = 0;
            break;
        case H_MODE: pick_mode(h->arg); break;
        case H_UISCALE: {
            static const int v[4] = {0, 100, 125, 150};
            if (cfg.ui_scale == v[h->arg])
                break;
            cfg.ui_scale = v[h->arg];
            note[0] = 0;
            apply();
            if (!note[0])
                snprintf(note, sizeof(note), "Die neue Gr\xC3\xB6\xC3\x9F" "e gilt ab dem n\xC3\xA4" "chsten Start des Desktops.");
            restart_hint = 1;
            break;
        }
        case H_CURSOR: cfg.cursor = cursor_pcts[h->arg]; apply(); break;
        case H_DOCKSIZE: {
            static const int v[3] = {85, 100, 125};
            cfg.dock = v[h->arg];
            apply();
            break;
        }
        case H_SECONDS: cfg.seconds = !cfg.seconds; apply(); break;
        case H_DATE: cfg.date = !cfg.date; apply(); break;
        case H_WALL: cfg.wallpaper = h->arg; apply(); break;
        case H_KBD: pick_kbd(h->arg); break;
        case H_RESTART: gfx_desktop_request(WP_SETTINGS, 1, 0, 0); break;
        case H_KEEP: keep_mode(); break;
        case H_REVERT: revert_mode(); break;
        case H_LIST: continue; /* nur fuer das Mausrad */
        }
        draw();
        return;
    }
}

static void wheel(int mx, int my, int d)
{
    if (page != P_DISPLAY || confirm || my < list_y0 || my >= list_y1 || nmodes <= list_rows)
        return;
    (void)mx;
    list_top += d > 0 ? -1 : 1;
    draw();
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    settings_load(&cfg);
    load_modes();
    for (int i = 0; i < nmodes; i++) /* die aktuelle Aufloesung sichtbar (drittoberste Zeile) */
        if (modes[i].current)
            list_top = i - 2;
    sys_keymap(0, kbd);
    ui_setup(0);
    if (gfx_open_window(U(820), U(560), "Einstellungen") != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    draw();
    s64 shown = sys_time();
    for (;;) {
        Event e;
        int got = gfx_wait(&e, 250);
        if (got) {
            if (e.type == EV_CLOSE)
                break;
            if (e.type == EV_KEY) {
                int k = KEY_BASE(e.key);
                if (k == 0x1B) {
                    if (!confirm)
                        break;
                    revert_mode();
                    draw();
                } else if ((k == KEY_UP || k == KEY_DOWN) && !confirm) {
                    page = (page + (k == KEY_UP ? NPAGES - 1 : 1)) % NPAGES;
                    note[0] = 0;
                    restart_hint = 0;
                    draw();
                } else if (k == '\n' && confirm) {
                    keep_mode();
                    draw();
                }
            } else if (e.type == EV_DOWN && e.button == 1) {
                click(e.x, e.y);
            } else if (e.type == EV_WHEEL) {
                wheel(e.x, e.y, e.wheel);
            } else if (e.type == EV_RESIZE || e.type == EV_FOCUS) {
                draw();
            }
        }
        if (sys_time() != shown) { /* Rueckfrage: Sekunden zaehlen; nach dem Umschalten stimmt "aktuell" wieder */
            shown = sys_time();
            if (confirm && shown >= confirm_end)
                revert_mode();
            if (confirm || page == P_DISPLAY || page == P_INFO) {
                load_modes();
                draw();
            }
        }
    }
    if (confirm)
        revert_mode();
    gfx_close();
    sys_exit(0);
}
