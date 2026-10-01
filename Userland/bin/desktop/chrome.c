/* Desktop: Masse, Hintergrundbild, Menueleiste, Dock und Menues (Programmsymbole: ui.c) */

#include "desktop.h"

int MENUBAR_H, TITLE_H, DOCK_H, RADIUS, SHADOW;
int menu_open, menu_hover = -1, dock_hover = -1;

static int ICON, DOCK_PAD, DOCK_GAP;

void desk_init(void)
{
    ui_setup(H >= 1300 ? 125 : 100);
    MENUBAR_H = U(26);
    TITLE_H = U(30);
    RADIUS = U(10);
    SHADOW = U(28);
    ICON = U(52);
    DOCK_PAD = U(7);
    DOCK_GAP = U(8);
    DOCK_H = ICON + 2 * DOCK_PAD + U(10);
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
    surface_new(&bg, W, H);
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

const char *app_name(const Win *w)
{
    return w ? w->name : "Schreibtisch";
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

/* ---------- Netzwerk: Zustand (einmal je Sekunde geholt), Symbol und Menue ---------- */

static NetInfo net;
static int     net_ok, net_idx, net_x = -1; /* Karte vorhanden, ihre Nummer; linke Kante des Symbols */
static u64     net_rx_rate, net_tx_rate;

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
    if (net_state() != old)
        damage_menubar();
    if (menu_open == 3)
        damage_menu();
}

void net_dhcp(void)
{
    if (net_ok) {
        sys_net_dhcp((u64)net_idx);
        net.dhcp = 1;
        memset(net.ip, 0, 4);
        damage_menubar();
    }
}

/* Kabel-Netzwerk: oben ein Kasten, darunter zwei, mit Leitungen verbunden. Ohne Verbindung blass und durchgestrichen,
 * ohne Adresse blass */
static void draw_net_icon(Surface *s, int x)
{
    int st = net_state();
    if (st < 0)
        return;
    float k = (float)U(1), cx = x + 8 * k, cy = MENUBAR_H * 0.5f;
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
    net_x = vol >= 0 ? x - U(68) : x - U(36);
    if (menu_open == 3)
        gfx_round_rect(s, net_x - U(6), U(3), U(28), MENUBAR_H - U(6), U(5), 0x000000, 36);
    draw_net_icon(s, net_x);
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
    if (net_ok && net_x >= 0 && x >= net_x - U(6) && x < net_x + U(22))
        return 3;
    return 0;
}

/* ======================================================================================================================
 * Menues
 * ==================================================================================================================== */

static const MenuItem logo_menu[] = {
    {"\xC3\x9C" "ber MiniKernel", A_ABOUT, 0}, {"", A_SEP, 0}, {"Neues Terminal", A_TERM, 0}, {"Dateien", A_FILES, 0},
    {"Texteditor", A_EDIT, 0}, {"Musik", A_MUSIC, 0},
    {"Rechner", A_CALC, 0}, {"Uhr", A_CLOCK, 0}, {"", A_SEP, 0}, {"Malen", A_PAINT, 0}, {"Snake", A_SNAKE, 0},
    {"Tetris", A_TETRIS, 0}, {"", A_SEP, 0}, {"Neu starten \xE2\x80\xA6", A_RESTART, 0},
    {"Ausschalten \xE2\x80\xA6", A_POWEROFF, 0}, {"", A_SEP, 0}, {"Zur Konsole", A_QUIT, 0},
};
static const MenuItem app_menu[] = {
    {"Neues Fenster", A_WIN_NEW, "Alt+N"},          {"N\xC3\xA4" "chstes Fenster", A_NEXT_WIN, "Alt+Tab"},
    {"", A_SEP, 0},
    {"Minimieren", A_WIN_MIN, "Alt+M"},             {"Zoomen", A_WIN_ZOOM, "Alt+F"},
    {"Links anordnen", A_SNAP_LEFT, "Alt+\xE2\x86\x90"}, {"Rechts anordnen", A_SNAP_RIGHT, "Alt+\xE2\x86\x92"},
    {"", A_SEP, 0},
    {"Fenster schlie\xC3\x9F" "en", A_WIN_CLOSE, "Alt+W"}, {"Programm beenden", A_APP_QUIT, "Alt+Q"},
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
    if (menu_open == 3) {
        net_menu_texts();
        *n = (int)(sizeof(net_menu) / sizeof(net_menu[0]));
        return net_menu;
    }
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
    *w = U(250);
    if (menu_open == 3) { /* rechts an der Leiste: unter dem Symbol, rechtsbuendig */
        *w = U(340);
        *x = net_x + U(22) - *w;
        if (*x + *w > W - U(6))
            *x = W - U(6) - *w;
    }
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
        if (m[i].action == A_INFO) { /* Name grau links, Wert rechts */
            int ty = iy + (U(24) - text_height(font_ui, FS)) / 2;
            text_draw(s, font_ui, FS, x + U(14), ty, m[i].label, 0x8E8E93);
            text_draw(s, i == 0 ? font_bold : font_ui, FS, x + w - U(14) - text_width(i == 0 ? font_bold : font_ui, FS, m[i].keys),
                      ty, m[i].keys, C_TEXT);
            continue;
        }
        int hover = i == menu_hover;
        if (hover)
            gfx_round_rect(s, x + U(5), iy, w - U(10), U(24), U(5), C_ACCENT, 255);
        int ty = iy + (U(24) - text_height(font_ui, FS)) / 2;
        text_draw(s, font_ui, FS, x + U(14), ty, m[i].label, hover ? 0xFFFFFF : C_TEXT);
        if (m[i].keys) /* Tastenkuerzel rechts, grau */
            text_draw(s, font_ui, FS, x + w - U(14) - text_width(font_ui, FS, m[i].keys), ty, m[i].keys,
                      hover ? 0xFFFFFF : 0x8E8E93);
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
        if (m[i].action != A_SEP && m[i].action != A_INFO && py >= iy && py < iy + U(24))
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

/* ======================================================================================================================
 * Dock: Programme, Trennstrich, minimierte Fenster
 * ==================================================================================================================== */

static const struct {
    int         action;
    const char *name;
} dock_apps[] = {
    {A_FILES, "Dateien"}, {A_TERM, "Terminal"}, {A_EDIT, "Texteditor"}, {A_MUSIC, "Musik"}, {A_CALC, "Rechner"},
    {A_CLOCK, "Uhr"},
    {A_PAINT, "Malen"},   {A_SNAKE, "Snake"},   {A_TETRIS, "Tetris"},
};
#define NAPPS ((int)(sizeof(dock_apps) / sizeof(dock_apps[0])))

static int win_of_app(const Win *w, int i) /* gehoert das Fenster zu Dock-Programm i? */
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

/* Vergroesserung unter der Maus (wie bei macOS): mag laeuft weich zwischen 0 und 1, jedes Symbol waechst je nach
 * Abstand seiner (unvergroesserten) Mitte von der Maus um bis zu 60 %. Das Dock wird entsprechend breiter; die
 * Symbole bleiben unten buendig und ragen nach oben heraus. */
#define MAG_MAX   0.6f
#define MAXSLOTS  (NAPPS + MAXW)

static float dock_mag;
static int   dock_mx = -1, dock_in;

typedef struct {
    int n, nm, box_x, box_w, y, h;
    int x[MAXSLOTS], sz[MAXSLOTS];
} DockLayout;

static void dock_layout(DockLayout *L)
{
    L->nm = n_minimized();
    L->n = NAPPS + L->nm;
    L->h = ICON + DOCK_PAD * 2;
    L->y = H - L->h - U(8);
    int sep = L->nm ? DOCK_GAP + 1 : 0;
    int w0 = DOCK_PAD * 2 + L->n * ICON + (L->n - 1) * DOCK_GAP + sep, bx0 = (W - w0) / 2, total = 0;
    for (int i = 0; i < L->n; i++) {
        float c = bx0 + DOCK_PAD + i * (ICON + DOCK_GAP) + (i >= NAPPS ? sep : 0) + ICON * 0.5f;
        float d = dock_mx < 0 ? 9 : (dock_mx - c) / (ICON * 2.3f);
        d = d < 0 ? -d : d;
        float f = d < 1 ? (1 + ui_sin(3.14159265f * d + 1.5707963f)) * 0.5f : 0; /* (1 + cos(pi d)) / 2 */
        L->sz[i] = (int)(ICON * (1 + MAG_MAX * dock_mag * f) + 0.5f);
        total += L->sz[i];
    }
    total += (L->n - 1) * DOCK_GAP + sep;
    L->box_w = total + DOCK_PAD * 2;
    L->box_x = (W - L->box_w) / 2;
    int x = L->box_x + DOCK_PAD;
    for (int i = 0; i < L->n; i++) {
        if (i == NAPPS)
            x += sep;
        L->x[i] = x;
        x += L->sz[i] + DOCK_GAP;
    }
}

int dock_top(void)
{
    return H - (ICON + DOCK_PAD * 2) - U(8) - U(6);
}

void dock_slot_of(const Win *w, int *x, int *y, int *size)
{
    DockLayout L;
    dock_layout(&L);
    int k = 0;
    for (int i = 0; i < MAXW && &wins[i] != w; i++)
        k += wins[i].used && wins[i].minimized;
    int i = NAPPS + k < L.n ? NAPPS + k : L.n - 1;
    *x = L.x[i];
    *size = L.sz[i];
    *y = L.y + L.h - DOCK_PAD - L.sz[i];
}

void draw_dock(void)
{
    Surface *s = &gfx_screen;
    DockLayout L;
    dock_layout(&L);
    int r = U(18);
    gfx_shadow(s, L.box_x, L.y + U(2), L.box_w, L.h, r, U(22), 55);
    gfx_blit_round(s, &bg_blur, L.box_x, L.y, L.box_x, L.y, L.box_w, L.h, r);
    gfx_round_rect(s, L.box_x, L.y, L.box_w, L.h, r, 0xFFFFFF, 100);
    gfx_round_frame(s, L.box_x, L.y, L.box_w, L.h, r, 0xFFFFFF, 150);
    for (int i = 0; i < L.n; i++) {
        int ix = L.x[i], sz = L.sz[i], iy = L.y + L.h - DOCK_PAD - sz;
        if (i < NAPPS) {
            ui_app_icon(s, dock_apps[i].action, ix, iy, sz);
            int running = 0;
            for (int k = 0; k < MAXW; k++)
                running |= wins[k].used && win_of_app(&wins[k], i);
            if (running)
                gfx_disc(s, ix + sz * 0.5f, L.y + L.h - U(4), U(2) * 1.1f, 0x1D1D1F, 200);
        } else {
            Win *mw = minimized_win(i - NAPPS);
            if (mw && mw->anim != ANIM_MIN) /* noch auf dem Weg ins Dock: Platz frei lassen */
                ui_app_icon(s, icon_of_win(mw), ix, iy, sz);
        }
    }
    if (L.nm)
        gfx_blend_fill(s, L.x[NAPPS] - DOCK_GAP / 2 - 1, L.y + DOCK_PAD, 1, ICON, 0x000000, 40);
    if (dock_hover >= 0 && dock_hover < L.n) { /* Name ueber dem Symbol */
        Win *mw = dock_hover >= NAPPS ? minimized_win(dock_hover - NAPPS) : 0;
        const char *name = dock_hover < NAPPS ? dock_apps[dock_hover].name : mw ? mw->title : "";
        int tw = text_width(font_ui, FS, name), bw = tw + U(22), bh = U(24);
        int sz = L.sz[dock_hover];
        int bx = L.x[dock_hover] + sz / 2 - bw / 2, by = L.y + L.h - DOCK_PAD - sz - bh - U(10);
        if (bx < U(4)) bx = U(4);
        if (bx + bw > W - U(4)) bx = W - U(4) - bw;
        gfx_shadow(s, bx, by + U(2), bw, bh, U(6), U(10), 45);
        gfx_round_rect(s, bx, by, bw, bh, U(6), 0xF2F2F4, 245);
        gfx_round_frame(s, bx, by, bw, bh, U(6), 0x000000, 30);
        text_draw(s, font_ui, FS, bx + U(11), by + (bh - text_height(font_ui, FS)) / 2, name, C_TEXT);
    }
}

/* Streifen unten, in dem Dock, vergroesserte Symbole und Namen liegen koennen */
void damage_dock(void)
{
    int top = H - (ICON + DOCK_PAD * 2) - U(8) - (int)(ICON * MAG_MAX) - U(60);
    damage(0, top, W, H - top);
}

int dock_hit(int px, int py)
{
    DockLayout L;
    dock_layout(&L);
    if (py >= L.y + L.h || px < L.box_x || px >= L.box_x + L.box_w)
        return -1;
    for (int i = 0; i < L.n; i++)
        if (px >= L.x[i] - DOCK_GAP / 2 && px < L.x[i] + L.sz[i] + DOCK_GAP / 2 &&
            py >= (py >= L.y ? L.y : L.y + L.h - DOCK_PAD - L.sz[i]))
            return i;
    return -1;
}

void dock_hover_at(int px, int py)
{
    DockLayout L;
    dock_layout(&L);
    /* Bereich der Vergroesserung: das Dock und darueber, so hoch die Symbole gerade ragen */
    int zone_top = L.y - (int)(ICON * MAG_MAX * dock_mag);
    int in = px >= L.box_x && px < L.box_x + L.box_w && py >= zone_top && py < H;
    int moved = in && px != dock_mx;
    dock_in = in;
    if (in)
        dock_mx = px;
    int h = dock_hit(px, py);
    if (h != dock_hover || (moved && dock_mag > 0.01f)) {
        dock_hover = h;
        damage_dock();
    }
}

void dock_tick(s64 dt_us)
{
    float target = dock_in ? 1.0f : 0.0f;
    if (dock_mag == target)
        return;
    float k = (float)dt_us / 90000.0f; /* etwa 90 ms bis fast ganz */
    dock_mag += (target - dock_mag) * (k > 1 ? 1 : k);
    if ((target - dock_mag) * (target - dock_mag) < 0.0001f)
        dock_mag = target;
    if (dock_mag == 0)
        dock_mx = -1;
    damage_dock();
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
