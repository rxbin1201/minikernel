/* Desktop: Masse, Hintergrundbild, Taskleiste und Menues (Programmsymbole: ui.c) */

#include "desktop.h"

int MENUBAR_H, TITLE_H, DOCK_H, RADIUS, SHADOW;
int menu_open, menu_hover = -1, dock_hover = -1;

static void bar_metrics(void);

void desk_init(void)
{
    ui_setup(H >= 1300 ? 125 : 100);
    MENUBAR_H = 0; /* keine Leiste oben: alles sitzt in der Taskleiste unten */
    TITLE_H = U(30);
    RADIUS = U(10);
    SHADOW = U(28);
    bar_metrics();
    DOCK_H = U(48);
}

/* ======================================================================================================================
 * Hintergrund: weicher Farbverlauf mit Wellen (berechnet), dazu eine weichgezeichnete Kopie fuer das Milchglas
 * ==================================================================================================================== */

/* Bogen um (cx, cy) mit Radius r von Winkel a0 bis a1 (Bogenmass, 0 = rechts, gegen den Uhrzeigersinn nach oben) */
static void arc(Surface *s, float cx, float cy, float r, float a0, float a1, float w, u32 c, int alpha)
{
    const int n = 6;
    for (int i = 0; i < n; i++) {
        float t0 = a0 + (a1 - a0) * i / n, t1 = a0 + (a1 - a0) * (i + 1) / n;
        gfx_capsule(s, cx + ui_sin(t0 + 1.5707963f) * r, cy - ui_sin(t0) * r, cx + ui_sin(t1 + 1.5707963f) * r, cy - ui_sin(t1) * r, w,
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
    gsurf_new(&bg, W, H); /* die GPU kopiert daraus */
    unsigned rnd = 12345;
    for (int y = 0; y < H; y++) {
        float v = (float)y / (float)H;
        for (int x = 0; x < W; x++) {
            float u = (float)x / (float)W;
            float wave = 0.10f * ui_sin(6.2831853f * (u * 1.1f + 0.15f)) + 0.06f * ui_sin(6.2831853f * (u * 2.3f + v * 0.7f));
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

static int icon_of_win(const Win *w)
{
    return w->app ? w->app : ICON_TEXT;
}

/* ======================================================================================================================
 * Taskleiste unten: vier freistehende Segmente aus Milchglas
 *   Programme (Strich darunter = laeuft, lang und blau = aktives Fenster; rechts die minimierten Fenster)
 *   Suche, Start, Fenster  |  Uhrzeit und Datum  |  Netzwerk, Lautstaerke, Systemmenue
 * Die Knoepfe oeffnen ihre Menues nach oben.
 * ==================================================================================================================== */

static void draw_logo(Surface *s, int x, int y, int size)
{
    gfx_round_rect_grad(s, x, y, size, size, size / 4, 0x6A7CFF, 0xC04BD6, 255);
    gfx_disc(s, x + size * 0.5f, y + size * 0.5f, size * 0.18f, 0xFFFFFF, 235);
}

static const struct {
    int         action;
    const char *name;
} dock_apps[] = {
    {A_FILES, "Dateien"}, {A_TERM, "Terminal"}, {A_EDIT, "Texteditor"}, {A_MUSIC, "Musik"}, {A_CALC, "Rechner"},
    {A_CLOCK, "Uhr"},     {A_PAINT, "Malen"},   {A_SNAKE, "Snake"},     {A_TETRIS, "Tetris"},
};
#define NAPPS    ((int)(sizeof(dock_apps) / sizeof(dock_apps[0])))
#define MAXSLOTS (NAPPS + MAXW)

enum { B_SEARCH, B_START, B_WINDOWS, B_CLOCK, B_NET, B_VOL, B_SYS, NBTN };
#define HIT_BTN 1000 /* bar_hit: Knoepfe ab hier, darunter die Programm-Slots */

static int win_of_app(const Win *w, int i) /* gehoert das Fenster zu Programm i? */
{
    return w->app == dock_apps[i].action;
}

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

/* ---------- Netzwerk und Lautstaerke: Zustand (einmal je Sekunde geholt) ---------- */

static NetInfo net;
static int     net_ok, net_idx;    /* Karte vorhanden, ihre Nummer */
static u64     net_rx_rate, net_tx_rate;
static s64     vol_level = -1;     /* Gesamtlautstaerke 0-100, -1 = kein Tonausgang */
static s64     vol_saved = 50;     /* Lautstaerke vor dem Stummschalten */

static int net_has_ip(void) { return net.ip[0] || net.ip[1] || net.ip[2] || net.ip[3]; }

static int net_state(void) /* -1 keine Karte, 0 kein Kabel, 1 ohne Adresse, 2 verbunden */
{
    return !net_ok ? -1 : !net.link ? 0 : net_has_ip() ? 2 : 1;
}

void net_tick(void)
{
    static s64 last_us;
    if (last_us && now_us - last_us < 1000000)
        return;
    s64 dt = last_us ? now_us - last_us : 0;
    last_us = now_us;
    int old = net_state();
    s64 old_vol = vol_level;
    u64 prx = net.rx_bytes, ptx = net.tx_bytes;
    NetInfo ni, pick;
    int found = 0;
    for (u64 i = 0; i < 4 && sys_netinfo(i, &ni) == 0; i++) /* die erste Karte mit Verbindung, sonst die erste */
        if (!found || (ni.link && !pick.link)) {
            pick = ni;
            net_idx = (int)i;
            found = 1;
        }
    int same = found && net_ok && strcmp(pick.name, net.name) == 0;
    net_ok = found;
    if (found)
        net = pick;
    net_rx_rate = same && dt > 0 && net.rx_bytes >= prx ? (net.rx_bytes - prx) * 1000000 / (u64)dt : 0;
    net_tx_rate = same && dt > 0 && net.tx_bytes >= ptx ? (net.tx_bytes - ptx) * 1000000 / (u64)dt : 0;
    vol_level = sys_audio(4, (u64)-1, 0);
    if (vol_level < 0)
        vol_level = -1;
    if (net_state() != old || vol_level != old_vol)
        damage_dock();
    if (menu_open == 3)
        damage_menu();
}

void net_dhcp(void)
{
    if (net_ok) {
        sys_net_dhcp((u64)net_idx);
        net.dhcp = 1;
        memset(net.ip, 0, 4);
        damage_dock();
    }
}

static void set_volume(s64 v)
{
    if (vol_level < 0)
        return;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    s64 r = sys_audio(4, (u64)v, 0);
    vol_level = r >= 0 ? r : v;
    damage_dock_seg(3);
}

/* ---------- Masse der Leiste ---------- */

static int SEG_H, SLOT, ICO, BTN, TBTN, SPAD, SGAP, SRAD;

static void bar_metrics(void)
{
    SEG_H = U(48);
    SLOT = U(44);
    ICO = U(30);
    BTN = U(40);
    TBTN = U(34);
    SPAD = U(6);
    SGAP = U(10);
    SRAD = U(16);
}

typedef struct {
    int  y, h;
    int  sx[4], sw[4];           /* Segmente */
    int  n, nm, sep;             /* Slots (Programme + minimierte Fenster), Trennstrich davor */
    int  ax[MAXSLOTS];
    int  bx[NBTN], bw[NBTN];     /* Knoepfe; bw 0 = gibt es nicht */
    char time[8], date[48];
} Bar;

static void clock_texts(char *t, int tn, char *d, int dn)
{
    t[0] = d[0] = 0;
    s64 now = sys_time();
    if (now <= 0)
        return;
    static const char *wd[7] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};
    static const char *mo[12] = {"Januar", "Februar", "M\xC3\xA4rz", "April",   "Mai",      "Juni",
                                 "Juli",   "August",  "September", "Oktober", "November", "Dezember"};
    DateTime dt;
    time_to_date((u64)now, &dt);
    snprintf(t, tn, "%02d:%02d", dt.hour, dt.min);
    snprintf(d, dn, "%s, %d. %s", wd[dt.wday % 7], dt.day, mo[(dt.month + 11) % 12]);
}

static void bar_layout(Bar *b)
{
    b->h = SEG_H;
    b->y = H - U(8) - SEG_H;
    clock_texts(b->time, sizeof(b->time), b->date, sizeof(b->date));
    int tw = text_width(font_bold, FS, b->time), dw = text_width(font_ui, FS_SMALL, b->date);
    b->sw[1] = SPAD * 2 + 3 * BTN;
    b->sw[2] = (tw > dw ? tw : dw) + U(18) * 2;
    b->sw[3] = SPAD * 2 + TBTN * (1 + (net_ok != 0) + (vol_level >= 0));
    /* minimierte Fenster: so viele, wie auf den Bildschirm passen (die anderen im Fenstermenue) */
    int room = W - U(32) - b->sw[1] - b->sw[2] - b->sw[3] - 3 * SGAP - SPAD * 2 - U(9);
    int fit = room / SLOT - NAPPS;
    b->nm = n_minimized();
    if (b->nm > fit)
        b->nm = fit > 0 ? fit : 0;
    b->n = NAPPS + b->nm;
    b->sep = b->nm ? U(9) : 0;
    b->sw[0] = SPAD * 2 + b->n * SLOT + b->sep;
    int total = b->sw[0] + b->sw[1] + b->sw[2] + b->sw[3] + 3 * SGAP, x = (W - total) / 2;
    for (int i = 0; i < 4; i++) {
        b->sx[i] = x;
        x += b->sw[i] + SGAP;
    }
    x = b->sx[0] + SPAD;
    for (int i = 0; i < b->n; i++) {
        if (i == NAPPS)
            x += b->sep;
        b->ax[i] = x;
        x += SLOT;
    }
    for (int k = 0; k < 3; k++) {
        b->bx[B_SEARCH + k] = b->sx[1] + SPAD + k * BTN;
        b->bw[B_SEARCH + k] = BTN;
    }
    b->bx[B_CLOCK] = b->sx[2];
    b->bw[B_CLOCK] = b->sw[2];
    x = b->sx[3] + SPAD;
    b->bx[B_NET] = x;
    b->bw[B_NET] = net_ok ? TBTN : 0;
    x += b->bw[B_NET];
    b->bx[B_VOL] = x;
    b->bw[B_VOL] = vol_level >= 0 ? TBTN : 0;
    x += b->bw[B_VOL];
    b->bx[B_SYS] = x;
    b->bw[B_SYS] = TBTN;
}

int dock_top(void)
{
    return H - U(8) - SEG_H - U(8);
}

static int bar_hit(int px, int py)
{
    Bar b;
    bar_layout(&b);
    if (py < b.y || py >= b.y + b.h)
        return -1;
    for (int i = 0; i < b.n; i++)
        if (px >= b.ax[i] && px < b.ax[i] + SLOT)
            return i;
    for (int k = 0; k < NBTN; k++)
        if (b.bw[k] && px >= b.bx[k] && px < b.bx[k] + b.bw[k])
            return HIT_BTN + k;
    return -1;
}

void dock_slot_of(const Win *w, int *x, int *y, int *size)
{
    Bar b;
    bar_layout(&b);
    int k = 0;
    for (int i = 0; i < MAXW && &wins[i] != w; i++)
        k += wins[i].used && wins[i].minimized;
    int i = NAPPS + k < b.n ? NAPPS + k : b.n - 1;
    *x = b.ax[i] + (SLOT - ICO) / 2;
    *y = b.y + (b.h - ICO) / 2 - U(2);
    *size = ICO;
}

/* ---------- Zeichnen ---------- */

static void seg_bg(Surface *s, int x, int y, int w, int h)
{
    gfx_shadow(s, x, y + U(3), w, h, SRAD, U(16), 45);
    gfx_blit_round(s, &bg_blur, x, y, x, y, w, h, SRAD);
    gfx_round_rect(s, x, y, w, h, SRAD, 0xFFFFFF, 178);
    gfx_round_frame(s, x, y, w, h, SRAD, 0xFFFFFF, 220);
}

static void ring(Surface *s, float cx, float cy, float r, float w, u32 c, int alpha)
{
    const int n = 20;
    for (int i = 0; i < n; i++) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        gfx_capsule(s, cx + ui_sin(a0 + 1.5707963f) * r, cy + ui_sin(a0) * r, cx + ui_sin(a1 + 1.5707963f) * r,
                    cy + ui_sin(a1) * r, w, c, alpha);
    }
}

static void draw_search_icon(Surface *s, float cx, float cy, float k, u32 c)
{
    ring(s, cx - 1.5f * k, cy - 1.5f * k, 5 * k, 1.8f * k, c, 255);
    gfx_capsule(s, cx + 2.3f * k, cy + 2.3f * k, cx + 6 * k, cy + 6 * k, 2.2f * k, c, 255);
}

static void rect_outline(Surface *s, float x, float y, float w, float h, float lw, u32 c, int a)
{
    gfx_capsule(s, x, y, x + w, y, lw, c, a);
    gfx_capsule(s, x + w, y, x + w, y + h, lw, c, a);
    gfx_capsule(s, x + w, y + h, x, y + h, lw, c, a);
    gfx_capsule(s, x, y + h, x, y, lw, c, a);
}

static void draw_windows_icon(Surface *s, float cx, float cy, float k) /* zwei uebereinanderliegende Fenster */
{
    rect_outline(s, cx - 3 * k, cy - 7 * k, 10 * k, 10 * k, 1.7f * k, C_TEXT, 255);
    gfx_round_rect(s, (int)(cx - 7 * k), (int)(cy - 3 * k), (int)(10 * k), (int)(10 * k), (int)(2 * k), 0xFFFFFF, 255);
    rect_outline(s, cx - 7 * k, cy - 3 * k, 10 * k, 10 * k, 1.7f * k, C_TEXT, 255);
}

/* Kabel-Netzwerk: oben ein Kasten, darunter zwei, mit Leitungen verbunden. Ohne Verbindung blass und durchgestrichen,
 * ohne Adresse blass */
static void draw_net_icon(Surface *s, float cx, float cy, float k)
{
    int st = net_state();
    if (st < 0)
        return;
    int a = st == 2 ? 255 : 110;
    gfx_round_rect(s, (int)(cx - 3 * k), (int)(cy - 6.5f * k), (int)(6 * k), (int)(4.5f * k), 1, C_TEXT, a);
    gfx_round_rect(s, (int)(cx - 8 * k), (int)(cy + 2 * k), (int)(6 * k), (int)(4.5f * k), 1, C_TEXT, a);
    gfx_round_rect(s, (int)(cx + 2 * k), (int)(cy + 2 * k), (int)(6 * k), (int)(4.5f * k), 1, C_TEXT, a);
    gfx_capsule(s, cx, cy - 2 * k, cx, cy - 0.5f * k, 1.3f * k, C_TEXT, a);
    gfx_capsule(s, cx - 5 * k, cy - 0.5f * k, cx + 5 * k, cy - 0.5f * k, 1.3f * k, C_TEXT, a);
    gfx_capsule(s, cx - 5 * k, cy - 0.5f * k, cx - 5 * k, cy + 2 * k, 1.3f * k, C_TEXT, a);
    gfx_capsule(s, cx + 5 * k, cy - 0.5f * k, cx + 5 * k, cy + 2 * k, 1.3f * k, C_TEXT, a);
    if (st == 0)
        gfx_capsule(s, cx - 8 * k, cy - 7 * k, cx + 8 * k, cy + 7 * k, 1.6f * k, C_TEXT, 220);
}

/* Lautsprecher: Kasten, Trichter, Schallwellen je nach Lautstaerke; stumm: durchgestrichen */
static void draw_vol_icon(Surface *s, float cx, float cy, float k)
{
    float sx = cx - 7 * k;
    gfx_round_rect(s, (int)sx, (int)(cy - 2.5f * k), (int)(3.5f * k), (int)(5 * k), 1, C_TEXT, 255);
    for (int i = 0; i <= 4; i++)
        gfx_capsule(s, sx + (3 + i) * k, cy - (2.5f + i * 0.9f) * k, sx + (3 + i) * k, cy + (2.5f + i * 0.9f) * k, 1.4f * k,
                    C_TEXT, 255);
    if (vol_level > 0)
        arc(s, sx + 8 * k, cy, 4 * k, -0.9f, 0.9f, 1.4f * k, C_TEXT, 230);
    if (vol_level > 50)
        arc(s, sx + 8 * k, cy, 7.5f * k, -0.9f, 0.9f, 1.4f * k, C_TEXT, 230);
    if (vol_level == 0)
        gfx_capsule(s, sx + 9.5f * k, cy - 3 * k, sx + 15 * k, cy + 3 * k, 1.5f * k, C_TEXT, 230);
}

static int start_via_search; /* Startmenue ueber die Lupe geoeffnet (dann ist die Lupe hervorgehoben) */

static int open_button(void) /* Knopf, dessen Menue offen ist */
{
    switch (menu_open) {
    case 1: return start_via_search ? B_SEARCH : B_START;
    case 2: return B_WINDOWS;
    case 3: return B_NET;
    case 4: return B_SYS;
    }
    return -1;
}

static const char *hover_name(const Bar *b, int h, char *buf, int max)
{
    if (h < 0)
        return 0;
    if (h < NAPPS)
        return dock_apps[h].name;
    if (h < b->n) {
        Win *mw = minimized_win(h - NAPPS);
        return mw ? mw->title : 0;
    }
    switch (h - HIT_BTN) {
    case B_SEARCH: return "Suchen";
    case B_START: return "Start";
    case B_WINDOWS: return "Fenster";
    case B_NET: return "Netzwerk";
    case B_VOL:
        snprintf(buf, max, vol_level ? "Lautst\xC3\xA4rke %lld %%" : "Stumm", (long long)vol_level);
        return buf;
    case B_SYS: return "System";
    }
    return 0;
}

void draw_dock(void)
{
    Surface *s = &gfx_screen;
    Bar b;
    bar_layout(&b);
    float k = (float)U(1), cy = b.y + b.h * 0.5f;
    for (int i = 0; i < 4; i++)
        seg_bg(s, b.sx[i], b.y, b.sw[i], b.h);

    /* Hervorhebung: Maus darueber bzw. Menue offen */
    int ob = open_button();
    for (int i = 0; i < b.n; i++)
        if (dock_hover == i)
            gfx_round_rect(s, b.ax[i] + U(2), b.y + U(5), SLOT - U(4), b.h - U(10), U(10), 0x000000, 16);
    for (int i = 0; i < NBTN; i++)
        if (b.bw[i] && (dock_hover == HIT_BTN + i || i == ob)) {
            int inset = i == B_CLOCK ? U(5) : U(2);
            gfx_round_rect(s, b.bx[i] + inset, b.y + U(5), b.bw[i] - 2 * inset, b.h - U(10), U(10), 0x000000,
                           i == ob ? 30 : 16);
        }

    /* Programme und minimierte Fenster */
    Win *f = focused();
    for (int i = 0; i < b.n; i++) {
        int ix = b.ax[i] + (SLOT - ICO) / 2, iy = b.y + (b.h - ICO) / 2 - U(2);
        float mx = b.ax[i] + SLOT * 0.5f, my = b.y + b.h - U(5);
        if (i < NAPPS) {
            ui_app_icon(s, dock_apps[i].action, ix, iy, ICO);
            int running = 0;
            for (int w = 0; w < MAXW; w++)
                running |= wins[w].used && win_of_app(&wins[w], i);
            int active = f && !f->minimized && win_of_app(f, i);
            if (running) {
                float hw = active ? 7 * k : 2.5f * k;
                gfx_capsule(s, mx - hw, my, mx + hw, my, 3 * k, active ? C_ACCENT : 0x8E8E93, 255);
            }
        } else {
            Win *mw = minimized_win(i - NAPPS);
            if (mw && mw->anim != ANIM_MIN) { /* noch auf dem Weg in die Leiste: Platz frei lassen */
                ui_app_icon(s, icon_of_win(mw), ix, iy, ICO);
                gfx_capsule(s, mx - 2.5f * k, my, mx + 2.5f * k, my, 3 * k, 0x8E8E93, 255);
            }
        }
    }
    if (b.nm)
        gfx_blend_fill(s, b.ax[NAPPS] - b.sep / 2 - 1, b.y + U(12), 1, b.h - U(24), 0x000000, 40);

    /* Suche, Start, Fenster */
    draw_search_icon(s, b.bx[B_SEARCH] + BTN * 0.5f, cy, k, C_TEXT);
    draw_logo(s, b.bx[B_START] + (BTN - U(20)) / 2, (int)cy - U(10), U(20));
    draw_windows_icon(s, b.bx[B_WINDOWS] + BTN * 0.5f, cy, k);

    /* Uhrzeit und Datum */
    int th = text_height(font_bold, FS), dh = text_height(font_ui, FS_SMALL), ty = b.y + (b.h - th - dh - U(1)) / 2;
    text_draw(s, font_bold, FS, b.sx[2] + U(18), ty, b.time, C_TEXT);
    text_draw(s, font_ui, FS_SMALL, b.sx[2] + U(18), ty + th + U(1), b.date, C_TEXT2);

    /* Netzwerk, Lautstaerke, System */
    if (b.bw[B_NET])
        draw_net_icon(s, b.bx[B_NET] + TBTN * 0.5f, cy, k);
    if (b.bw[B_VOL])
        draw_vol_icon(s, b.bx[B_VOL] + TBTN * 0.5f, cy, k);
    float sx = b.bx[B_SYS] + TBTN * 0.5f;
    gfx_capsule(s, sx - 4.5f * k, cy + 2.2f * k, sx, cy - 2.3f * k, 1.8f * k, C_TEXT, 255);
    gfx_capsule(s, sx, cy - 2.3f * k, sx + 4.5f * k, cy + 2.2f * k, 1.8f * k, C_TEXT, 255);

    /* Name ueber dem Knopf unter der Maus */
    char buf[48];
    const char *name = menu_open ? 0 : hover_name(&b, dock_hover, buf, sizeof(buf));
    if (name && name[0]) {
        int cx = dock_hover < HIT_BTN ? b.ax[dock_hover] + SLOT / 2 : b.bx[dock_hover - HIT_BTN] + b.bw[dock_hover - HIT_BTN] / 2;
        int tw = text_width(font_ui, FS, name), bw = tw + U(22), bh = U(26);
        int bx = cx - bw / 2, by = b.y - bh - U(8);
        if (bx < U(4)) bx = U(4);
        if (bx + bw > W - U(4)) bx = W - U(4) - bw;
        gfx_shadow(s, bx, by + U(2), bw, bh, U(8), U(10), 45);
        gfx_round_rect(s, bx, by, bw, bh, U(8), 0xF8F8FA, 245);
        gfx_round_frame(s, bx, by, bw, bh, U(8), 0x000000, 25);
        text_draw(s, font_ui, FS, bx + U(11), by + (bh - text_height(font_ui, FS)) / 2, name, C_TEXT);
    }
}

/* Streifen unten: Leiste und die Namen darueber */
void damage_dock(void)
{
    int z[4];
    dock_zone(z);
    damage(z[0], z[1], z[2], z[3]);
    overlay_dirty(z[0], z[1], z[2], z[3]);
}

void dock_zone(int *r)
{
    int top = H - U(8) - SEG_H - U(44);
    r[0] = 0;
    r[1] = top;
    r[2] = W;
    r[3] = H - top;
}

void damage_menubar(void) { damage_dock(); }

int dock_hit(int x, int y) { return bar_hit(x, y); }

/* Bereich, den die Hervorhebung von Feld h und sein Name darueber (mit Schatten) belegen */
static int hover_area(int h, int *r)
{
    if (h < 0)
        return 0;
    Bar b;
    bar_layout(&b);
    int x0, x1;
    if (h < HIT_BTN) {
        x0 = b.ax[h];
        x1 = x0 + SLOT;
    } else {
        x0 = b.bx[h - HIT_BTN];
        x1 = x0 + b.bw[h - HIT_BTN];
    }
    int y0 = b.y, y1 = b.y + b.h;
    char buf[48];
    const char *name = menu_open ? 0 : hover_name(&b, h, buf, sizeof(buf));
    if (name && name[0]) { /* wie in draw_dock */
        int cx = (x0 + x1) / 2, bw = text_width(font_ui, FS, name) + U(22), bh = U(26);
        int bx = cx - bw / 2, by = b.y - bh - U(8);
        if (bx < U(4)) bx = U(4);
        if (bx + bw > W - U(4)) bx = W - U(4) - bw;
        if (bx - U(14) < x0) x0 = bx - U(14);
        if (bx + bw + U(14) > x1) x1 = bx + bw + U(14);
        y0 = by - U(14);
    }
    r[0] = x0 - U(2);
    r[1] = y0;
    r[2] = x1 - x0 + U(4);
    r[3] = y1 - y0;
    return 1;
}

static void damage_hover(int h)
{
    int r[4];
    if (hover_area(h, r)) {
        damage(r[0], r[1], r[2], r[3]);
        overlay_dirty(r[0], r[1], r[2], r[3]);
    }
}

/* Segment i der Taskleiste (0 Programme, 1 Suche/Start/Fenster, 2 Uhr, 3 Netz/Ton/System) samt Schatten neu */
void damage_dock_seg(int i)
{
    Bar b;
    bar_layout(&b);
    int m = U(20);
    damage(b.sx[i] - m, b.y - m, b.sw[i] + 2 * m, b.h + 2 * m);
    overlay_dirty(b.sx[i] - m, b.y - m, b.sw[i] + 2 * m, b.h + 2 * m);
}

/* Knopf des offenen Menues (Hervorhebung) und der Name unter der Maus (bei offenem Menue ausgeblendet) */
void damage_menu_button(void)
{
    int ob = open_button();
    if (ob >= 0) {
        Bar b;
        bar_layout(&b);
        int m = U(4);
        if (b.bw[ob]) {
            damage(b.bx[ob] - m, b.y - m, b.bw[ob] + 2 * m, b.h + 2 * m);
            overlay_dirty(b.bx[ob] - m, b.y - m, b.bw[ob] + 2 * m, b.h + 2 * m);
        }
    }
    damage_hover(dock_hover);
}

void dock_hover_at(int px, int py)
{
    int h = bar_hit(px, py);
    if (h != dock_hover) { /* nur das alte und das neue Feld (samt Namen) neu */
        damage_hover(dock_hover);
        dock_hover = h;
        damage_hover(h);
    }
}

int dock_wheel(int px, int py, int delta)
{
    if (bar_hit(px, py) != HIT_BTN + B_VOL)
        return 0;
    set_volume(vol_level + (delta > 0 ? 5 : -5));
    return 1;
}

void dock_click(int i)
{
    if (i < 0)
        return;
    if (i >= HIT_BTN) {
        if (i - HIT_BTN == B_CLOCK) {
            do_action(A_CLOCK);
        } else if (i - HIT_BTN == B_VOL) { /* stumm schalten bzw. zurueck */
            if (vol_level > 0) {
                vol_saved = vol_level;
                set_volume(0);
            } else {
                set_volume(vol_saved > 0 ? vol_saved : 50);
            }
        }
        return;
    }
    if (i >= NAPPS) {
        Win *w = minimized_win(i - NAPPS);
        if (w)
            raise_win(w);
        return;
    }
    int act = dock_apps[i].action;
    /* Programm hat Fenster: das oberste nach vorn; ist es schon vorn, ein neues */
    Win *top = 0;
    for (int k = nord - 1; k >= 0 && !top; k--)
        if (win_of_app(order[k], i) && !order[k]->minimized)
            top = order[k];
    if (top && top != focused()) {
        raise_win(top);
        return;
    }
    if (!top) /* nur minimiert: zurueckholen (Terminal und Dateien: lieber ein neues) */
        for (int k = 0; k < MAXW && !top; k++)
            if (wins[k].used && win_of_app(&wins[k], i) && act != A_TERM && act != A_FILES)
                top = &wins[k];
    if (top && top != focused())
        raise_win(top);
    else
        do_action(dock_apps[i].action);
}

/* Knopf mit Menue: 1 Start, 2 Fenster, 3 Netzwerk, 4 System, 5 Start ueber die Suche; 0 = keiner */
int menubar_hit(int x, int y)
{
    switch (bar_hit(x, y) - HIT_BTN) {
    case B_SEARCH: return 5;
    case B_START: return 1;
    case B_WINDOWS: return 2;
    case B_NET: return 3;
    case B_SYS: return 4;
    }
    return 0;
}

/* ======================================================================================================================
 * Menues (oeffnen sich nach oben ueber ihrem Knopf)
 * ==================================================================================================================== */

static char start_query[32];

int menu_current(void)
{
    return menu_open == 1 && start_via_search ? 5 : menu_open;
}

void open_menu(int m)
{
    damage_menu_button(); /* ein vorher offenes Menue */
    start_via_search = m == 5;
    menu_open = m == 5 ? 1 : m;
    start_query[0] = 0;
    menu_hover = -1;
    damage_menu();
    damage_menu_button();
}

/* Startmenue: Suchfeld, dann die passenden Programme */
static const struct {
    int         action;
    const char *name;
} start_apps[] = {
    {A_TERM, "Terminal"}, {A_FILES, "Dateien"}, {A_EDIT, "Texteditor"}, {A_MUSIC, "Musik"}, {A_CALC, "Rechner"},
    {A_CLOCK, "Uhr"},     {A_PAINT, "Malen"},   {A_SNAKE, "Snake"},     {A_TETRIS, "Tetris"},
    {A_ABOUT, "\xC3\x9C" "ber MiniKernel"},
};
#define NSTART ((int)(sizeof(start_apps) / sizeof(start_apps[0])))
static MenuItem start_items[NSTART + 2];

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* 0 = passt nicht, 1 = enthaelt die Suche, 2 = beginnt damit (Gross/Klein egal) */
static int matches(const char *name, const char *q)
{
    int n = (int)strlen(q);
    if (!n)
        return 2;
    for (const char *p = name; *p; p++) {
        int i = 0;
        while (i < n && lower((unsigned char)p[i]) == lower((unsigned char)q[i]))
            i++;
        if (i == n)
            return p == name ? 2 : 1;
    }
    return 0;
}

static int build_start(void)
{
    int n = 0;
    start_items[n++] = (MenuItem){"", A_SEARCH, start_query};
    for (int pass = 2; pass >= 1; pass--) /* erst, was mit der Suche beginnt */
        for (int i = 0; i < NSTART; i++)
            if (matches(start_apps[i].name, start_query) == pass)
                start_items[n++] = (MenuItem){start_apps[i].name, start_apps[i].action, 0};
    if (n == 1)
        start_items[n++] = (MenuItem){"Keine Treffer", A_INFO, ""};
    return n;
}

/* Fenster: alle offenen Fenster (oberstes zuerst), dann die Befehle fuer das aktive */
static const MenuItem win_actions[] = {
    {"Neues Fenster", A_WIN_NEW, "Alt+N"},          {"N\xC3\xA4" "chstes Fenster", A_NEXT_WIN, "Alt+Tab"},
    {"Minimieren", A_WIN_MIN, "Alt+M"},             {"Zoomen", A_WIN_ZOOM, "Alt+F"},
    {"Links anordnen", A_SNAP_LEFT, "Alt+\xE2\x86\x90"}, {"Rechts anordnen", A_SNAP_RIGHT, "Alt+\xE2\x86\x92"},
    {"Fenster schlie\xC3\x9F" "en", A_WIN_CLOSE, "Alt+W"}, {"Programm beenden", A_APP_QUIT, "Alt+Q"},
};
#define NWACT ((int)(sizeof(win_actions) / sizeof(win_actions[0])))
#define WINMENU_MAX 30 /* so viele Fenster zeigt das Menue (die obersten); mehr passen nicht auf den Bildschirm */
static MenuItem win_items[WINMENU_MAX + NWACT + 3];
static char     win_more[40];

static int build_windows(void)
{
    int n = 0, more = 0;
    for (int i = nord - 1; i >= 0; i--)
        if (order[i]->used && order[i]->anim != ANIM_CLOSE) {
            if (n == WINMENU_MAX) {
                more++;
                continue;
            }
            win_items[n++] = (MenuItem){order[i]->title, A_WINSEL + (int)(order[i] - wins),
                                        order[i]->minimized ? "minimiert" : 0};
        }
    if (more) {
        snprintf(win_more, sizeof(win_more), "%d weitere (Alt+Tab)", more);
        win_items[n++] = (MenuItem){"\xE2\x80\xA6", A_INFO, win_more};
    }
    if (!n)
        win_items[n++] = (MenuItem){"Keine Fenster", A_INFO, ""};
    win_items[n++] = (MenuItem){"", A_SEP, 0};
    for (int i = 0; i < NWACT; i++)
        win_items[n++] = win_actions[i];
    return n;
}

static const MenuItem sys_menu[] = {
    {"\xC3\x9C" "ber MiniKernel", A_ABOUT, 0}, {"", A_SEP, 0}, {"Neu starten \xE2\x80\xA6", A_RESTART, 0},
    {"Ausschalten \xE2\x80\xA6", A_POWEROFF, 0}, {"", A_SEP, 0}, {"Zur Konsole", A_QUIT, 0},
};

static char nm_status[40], nm_model[40], nm_ip[24], nm_gw[20], nm_dns[20], nm_speed[32], nm_rx[40], nm_tx[40];
static MenuItem net_menu[] = {
    {"Ethernet", A_INFO, nm_status},   {"Karte", A_INFO, nm_model},      {"IP-Adresse", A_INFO, nm_ip},
    {"Gateway", A_INFO, nm_gw},        {"DNS-Server", A_INFO, nm_dns},   {"Verbindung", A_INFO, nm_speed},
    {"Empfangen", A_INFO, nm_rx},      {"Gesendet", A_INFO, nm_tx},      {"", A_SEP, 0},
    {"Adresse neu anfragen (DHCP)", A_NET_DHCP, 0},
};

static void bytes_str(char *out, int max, u64 b)
{
    if (b < 1024)
        snprintf(out, max, "%llu B", b);
    else if (b < 1024 * 1024)
        snprintf(out, max, "%llu.%llu KB", b / 1024, b % 1024 * 10 / 1024);
    else if (b < 1024ULL * 1024 * 1024)
        snprintf(out, max, "%llu.%llu MB", b / (1024 * 1024), b % (1024 * 1024) * 10 / (1024 * 1024));
    else
        snprintf(out, max, "%llu.%llu GB", b >> 30, (b & ((1ULL << 30) - 1)) * 10 >> 30);
}

static void ip_str(char *out, int max, const unsigned char *ip)
{
    if (ip[0] || ip[1] || ip[2] || ip[3])
        snprintf(out, max, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    else
        snprintf(out, max, "\xE2\x80\x93"); /* Gedankenstrich */
}

static void net_menu_texts(void)
{
    int st = net_state();
    snprintf(nm_status, sizeof(nm_status), "%s",
             st == 2 ? "Verbunden" : st == 0 ? "Kein Kabel" : net.dhcp == 1 ? "Sucht Adresse \xE2\x80\xA6" :
             net.dhcp == 3 ? "Keine Adresse (DHCP)" : "Ohne Adresse");
    snprintf(nm_model, sizeof(nm_model), "%.30s (%s)", net.model, net.name);
    if (net_has_ip()) {
        int bits = 0;
        for (int i = 0; i < 4; i++)
            for (int b = 7; b >= 0; b--)
                bits += net.mask[i] >> b & 1;
        snprintf(nm_ip, sizeof(nm_ip), "%u.%u.%u.%u/%d", net.ip[0], net.ip[1], net.ip[2], net.ip[3], bits);
    } else {
        ip_str(nm_ip, sizeof(nm_ip), net.ip);
    }
    ip_str(nm_gw, sizeof(nm_gw), net.gateway);
    ip_str(nm_dns, sizeof(nm_dns), net.dns);
    if (net.link)
        snprintf(nm_speed, sizeof(nm_speed), "%u Mbit/s, %s%s", net.mbps, net.full_duplex ? "Vollduplex" : "Halbduplex",
                 net.dhcp == 2 ? ", DHCP" : net.dhcp == 0 && net_has_ip() ? ", fest" : "");
    else
        snprintf(nm_speed, sizeof(nm_speed), "getrennt");
    char a[16], r[16];
    bytes_str(a, sizeof(a), net.rx_bytes);
    bytes_str(r, sizeof(r), net_rx_rate);
    snprintf(nm_rx, sizeof(nm_rx), "%s  (%s/s)", a, r);
    bytes_str(a, sizeof(a), net.tx_bytes);
    bytes_str(r, sizeof(r), net_tx_rate);
    snprintf(nm_tx, sizeof(nm_tx), "%s  (%s/s)", a, r);
}

static const MenuItem *menu_items(int *n)
{
    switch (menu_open) {
    case 1:
        *n = build_start();
        return start_items;
    case 2:
        *n = build_windows();
        return win_items;
    case 3:
        net_menu_texts();
        *n = (int)(sizeof(net_menu) / sizeof(net_menu[0]));
        return net_menu;
    }
    *n = (int)(sizeof(sys_menu) / sizeof(sys_menu[0]));
    return sys_menu;
}

static int row_h(const MenuItem *m)
{
    if (m->action == A_SEP)
        return U(11);
    if (m->action == A_SEARCH)
        return U(40);
    if (menu_open == 1 || m->action >= A_WINSEL)
        return U(32); /* mit Programmsymbol */
    return U(26);
}

static int is_item(const MenuItem *m)
{
    return m->action != A_SEP && m->action != A_INFO && m->action != A_SEARCH;
}

static void menu_box(int *x, int *y, int *w, int *h)
{
    int n;
    const MenuItem *m = menu_items(&n);
    Bar b;
    bar_layout(&b);
    int btn = open_button();
    *w = menu_open == 3 ? U(340) : menu_open == 4 ? U(230) : U(290);
    *h = U(12);
    for (int i = 0; i < n; i++)
        *h += row_h(&m[i]);
    *y = b.y - U(8) - *h;
    *x = btn >= 0 ? b.bx[btn] + b.bw[btn] / 2 - *w / 2 : (W - *w) / 2;
    if (*x + *w > W - U(6))
        *x = W - U(6) - *w;
    if (*x < U(6))
        *x = U(6);
}

static int menu_item_y(int i)
{
    int n, x, y, w, h;
    const MenuItem *m = menu_items(&n);
    menu_box(&x, &y, &w, &h);
    y += U(6);
    for (int k = 0; k < i; k++)
        y += row_h(&m[k]);
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
    int r = U(12);
    gfx_shadow(s, x, y + U(4), w, h, r, U(18), 60);
    gfx_blit_round(s, &bg_blur, x, y, x, y, w, h, r);
    gfx_round_rect(s, x, y, w, h, r, 0xFFFFFF, 215);
    gfx_round_frame(s, x, y, w, h, r, 0x000000, 28);
    int iy = y + U(6);
    for (int i = 0; i < n; iy += row_h(&m[i]), i++) {
        int rh = row_h(&m[i]), ty = iy + (rh - text_height(font_ui, FS)) / 2;
        if (m[i].action == A_SEP) {
            gfx_blend_fill(s, x + U(12), iy + U(5), w - U(24), 1, 0x000000, 30);
            continue;
        }
        if (m[i].action == A_SEARCH) { /* Suchfeld mit Lupe und Schreibmarke */
            int fx = x + U(8), fy = iy + U(4), fw = w - U(16), fh = rh - U(8);
            gfx_round_rect(s, fx, fy, fw, fh, U(8), 0x000000, 14);
            draw_search_icon(s, fx + U(16), fy + fh * 0.5f, (float)U(1) * 0.8f, C_TEXT2);
            int tx = fx + U(32);
            if (start_query[0])
                text_draw(s, font_ui, FS, tx, ty, start_query, C_TEXT);
            else
                text_draw(s, font_ui, FS, tx, ty, "Programme suchen", C_TEXT2);
            int cx = tx + (start_query[0] ? text_width(font_ui, FS, start_query) + U(1) : 0);
            gfx_blend_fill(s, cx, fy + U(7), U(2) > 1 ? U(2) : 2, fh - U(14), C_ACCENT, 255);
            continue;
        }
        if (m[i].action == A_INFO) { /* Name grau links, Wert rechts */
            text_draw(s, font_ui, FS, x + U(14), ty, m[i].label, 0x8E8E93);
            Font *vf = menu_open == 3 && i == 0 ? font_bold : font_ui;
            text_draw(s, vf, FS, x + w - U(14) - text_width(vf, FS, m[i].keys), ty, m[i].keys, C_TEXT);
            continue;
        }
        int hover = i == menu_hover;
        if (hover)
            gfx_round_rect(s, x + U(6), iy, w - U(12), rh, U(7), C_ACCENT, 255);
        int tx = x + U(14);
        if (menu_open == 1 || m[i].action >= A_WINSEL) { /* Programmsymbol davor */
            int icon = m[i].action >= A_WINSEL ? icon_of_win(&wins[m[i].action - A_WINSEL]) : m[i].action;
            ui_app_icon(s, icon, x + U(12), iy + (rh - U(22)) / 2, U(22));
            tx = x + U(44);
        }
        text_draw(s, font_ui, FS, tx, ty, m[i].label, hover ? 0xFFFFFF : C_TEXT);
        if (m[i].keys) /* Tastenkuerzel oder Zusatz rechts, grau */
            text_draw(s, font_ui, FS, x + w - U(14) - text_width(font_ui, FS, m[i].keys), ty, m[i].keys,
                      hover ? 0xFFFFFF : 0x8E8E93);
    }
}

int menu_zone(int *r)
{
    if (!menu_open)
        return 0;
    int x, y, w, h;
    menu_box(&x, &y, &w, &h);
    r[0] = x - U(24); /* mit Schatten ringsum */
    r[1] = y - U(24);
    r[2] = w + U(48);
    r[3] = h + U(52);
    return 1;
}

void damage_menu(void)
{
    int z[4];
    if (!menu_zone(z))
        return;
    damage(z[0], z[1], z[2], z[3]);
    overlay_dirty(z[0], z[1], z[2], z[3]);
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
        if (is_item(&m[i]) && py >= iy && py < iy + row_h(&m[i]))
            return i;
    }
    return -1;
}

int menu_inside(int px, int py)
{
    if (!menu_open)
        return 0;
    int x, y, w, h;
    menu_box(&x, &y, &w, &h);
    return px >= x && px < x + w && py >= y && py < y + h;
}

int menu_action(int i)
{
    int n;
    const MenuItem *m = menu_items(&n);
    return i >= 0 && i < n ? m[i].action : A_NONE;
}

/* Tasten bei offenem Menue: Pfeile waehlen, Enter fuehrt aus; im Startmenue wird getippt gesucht. 1 = verbraucht */
int menu_key(int k)
{
    if (!menu_open || (k & (KEY_MOD_ALT | KEY_MOD_CTRL)))
        return 0;
    int n;
    const MenuItem *m = menu_items(&n);
    k &= 0xFF;
    if (k == KEY_UP || k == KEY_DOWN) {
        int i = menu_hover;
        for (int t = 0; t < n; t++) {
            i = k == KEY_DOWN ? (i + 1) % n : (i <= 0 ? n - 1 : i - 1);
            if (is_item(&m[i]))
                break;
        }
        if (is_item(&m[i]))
            menu_hover = i;
        damage_menu();
        return 1;
    }
    if (k == '\n' || k == '\r') {
        int i = menu_hover;
        if (i < 0 || i >= n || !is_item(&m[i]))
            for (i = 0; i < n && !is_item(&m[i]); i++)
                ;
        if (i < n)
            do_action(m[i].action);
        return 1;
    }
    if (menu_open != 1)
        return 0;
    int len = (int)strlen(start_query);
    damage_menu(); /* alte Groesse */
    if (k == '\b' || k == 0x7F) {
        if (len)
            start_query[len - 1] = 0;
    } else if (k >= 32 && k < 127) {
        if (len < (int)sizeof(start_query) - 1) {
            start_query[len] = (char)k;
            start_query[len + 1] = 0;
        }
    } else {
        return 0;
    }
    m = menu_items(&n);
    menu_hover = start_query[0] && n > 1 && is_item(&m[1]) ? 1 : -1; /* erster Treffer: Enter startet ihn */
    damage_menu();
    damage_dock();
    return 1;
}
