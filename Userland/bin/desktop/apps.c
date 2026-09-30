/* Desktop: Fenster fuer Dateien, Text, Bilder, Rechner, Uhr und Info */

#include "desktop.h"

/* ======================================================================================================================
 * Dateien, Text, Bild
 * ==================================================================================================================== */

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static void files_load(Win *w)
{
    u_free(w->ents);
    w->ents = 0;
    w->nent = 0;
    int cap = 64;
    FileEnt *e = u_malloc(sizeof(FileEnt) * (u64)cap);
    if (!e)
        return;
    int n = 0;
    if (strcmp(w->dir, "/") != 0) {
        snprintf(e[0].name, sizeof(e[0].name), "..");
        e[0].is_dir = 1;
        n = 1;
    }
    DirEnt de;
    for (u64 i = 0; sys_readdir(w->dir, i, &de) == 0; i++) {
        if (n == cap) {
            FileEnt *ne = u_malloc(sizeof(FileEnt) * (u64)cap * 2);
            if (!ne)
                break;
            memcpy(ne, e, sizeof(FileEnt) * (u64)n);
            u_free(e);
            e = ne;
            cap *= 2;
        }
        snprintf(e[n].name, sizeof(e[n].name), "%s", de.name);
        e[n].is_dir = (int)de.is_dir;
        e[n].size = de.size;
        n++;
    }
    int first = strcmp(w->dir, "/") != 0; /* ".." bleibt oben; sonst Ordner zuerst, alphabetisch */
    for (int i = first + 1; i < n; i++) {
        FileEnt x = e[i];
        int j = i - 1;
        while (j >= first && (e[j].is_dir < x.is_dir || (e[j].is_dir == x.is_dir && strcasecmp(e[j].name, x.name) > 0))) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = x;
    }
    w->ents = e;
    w->nent = n;
    w->scroll = 0;
    w->sel = 0;
    const char *base = strrchr(w->dir, '/');
    snprintf(w->title, sizeof(w->title), "%s", strcmp(w->dir, "/") == 0 ? "System" : base ? base + 1 : w->dir);
    win_dirty_all(w);
    damage_menubar();
}

void open_files(const char *dir)
{
    Win *w = new_window(W_FILES, "Dateien", U(560), U(420));
    if (!w)
        return;
    snprintf(w->dir, sizeof(w->dir), "%s", dir);
    files_load(w);
}

static void open_text(const char *path)
{
    const char *base = strrchr(path, '/');
    Win *w = new_window(W_TEXT, base ? base + 1 : path, 82 * CELL_W + U(24), U(460));
    if (!w)
        return;
    Stat st;
    if (sys_stat(path, &st) != 0 || st.size > (4u << 20)) {
        w->text = u_malloc(64);
        snprintf(w->text, 64, "(nicht lesbar oder groesser als 4 MB)");
        st.size = strlen(w->text);
    } else {
        w->text = u_malloc(st.size + 1);
        s64 fd = sys_open(path, O_RDONLY);
        u64 got = 0;
        s64 r;
        while (fd >= 0 && got < st.size && (r = sys_read((int)fd, w->text + got, st.size - got)) > 0)
            got += (u64)r;
        if (fd >= 0)
            sys_close((int)fd);
        w->text[got] = 0;
        st.size = got;
        int binary = 0;
        for (u64 i = 0; i < got && i < 4096; i++)
            if (w->text[i] == 0)
                binary = 1;
        if (binary) { /* Programme und andere Binaerdateien nicht als Text zeigen */
            int is_prog = got >= 4 && w->text[0] == 0x7F && w->text[1] == 'E' && w->text[2] == 'L' && w->text[3] == 'F';
            char *msg = u_malloc(512);
            snprintf(msg, 512, "%s (%llu Bytes) - kein Text.\n\n%s", is_prog ? "Programm" : "Bin\xC3\xA4rdatei",
                     (unsigned long long)got,
                     is_prog ? "Programme startet man im Terminal, z.B. mit ihrem Namen." : "Ansehen im Terminal mit: hexdump datei");
            u_free(w->text);
            w->text = msg;
            st.size = strlen(msg);
        }
    }
    int n = 1;
    for (u64 i = 0; i < st.size; i++)
        if (w->text[i] == '\n')
            n++;
    w->lines = u_malloc(sizeof(char *) * (u64)n);
    w->nlines = 0;
    char *p = w->text;
    for (u64 i = 0; i <= st.size; i++) {
        if (i == st.size || w->text[i] == '\n') {
            w->text[i] = 0;
            if (i > 0 && w->text[i - 1] == '\r')
                w->text[i - 1] = 0;
            w->lines[w->nlines++] = p;
            p = w->text + i + 1;
        }
    }
}

static void open_image(const char *path)
{
    const char *base = strrchr(path, '/');
    Win *w = new_window(W_IMAGE, base ? base + 1 : path, U(560), U(440));
    if (!w)
        return;
    if (bmp_load(path, &w->img, w->err, sizeof(w->err)) != 0)
        w->img.px = 0;
}

void files_open_entry(Win *w, int i)
{
    if (i < 0 || i >= w->nent)
        return;
    FileEnt *e = &w->ents[i];
    char path[400];
    if (strcmp(e->name, "..") == 0) {
        char *s = strrchr(w->dir, '/');
        if (s && s != w->dir)
            *s = 0;
        else
            snprintf(w->dir, sizeof(w->dir), "/");
        files_load(w);
        return;
    }
    snprintf(path, sizeof(path), "%s%s%s", w->dir, strcmp(w->dir, "/") == 0 ? "" : "/", e->name);
    if (e->is_dir) {
        snprintf(w->dir, sizeof(w->dir), "%s", path);
        files_load(w);
    } else if (ends_with(e->name, ".bmp")) {
        open_image(path);
    } else {
        open_text(path);
    }
}

static void folder_icon(Surface *s, int x, int y, int sz)
{
    gfx_round_rect(s, x, y + sz / 8, sz * 45 / 100, sz / 4, sz / 10, 0x3B99F0, 255);
    gfx_round_rect_grad(s, x, y + sz / 4, sz, sz * 65 / 100, sz / 8, 0x74BCFA, 0x3B99F0, 255);
}

static void doc_icon(Surface *s, int x, int y, int sz, u32 accent)
{
    gfx_round_rect(s, x + sz / 8, y, sz * 3 / 4, sz, sz / 10, 0xFFFFFF, 255);
    gfx_round_frame(s, x + sz / 8, y, sz * 3 / 4, sz, sz / 10, 0xB0B0B8, 255);
    if (accent)
        gfx_round_rect(s, x + sz / 4, y + sz / 2, sz / 2, sz / 4, sz / 16, accent, 255);
}

void draw_files(Win *w, int x, int y, int cw, int ch)
{
    Surface *s = tgt;
    int head = U(28);
    gfx_fill(s, x, y, cw, ch, C_WINDOW);
    gfx_fill(s, x, y, cw, head, 0xFAFAFA);
    gfx_fill(s, x, y + head - 1, cw, 1, 0xE5E5E5);
    int ty = (head - text_height(font_bold, FS_SMALL)) / 2;
    text_draw(s, font_bold, FS_SMALL, x + U(40), y + ty, "Name", C_TEXT2);
    text_draw(s, font_bold, FS_SMALL, x + cw - U(80), y + ty, "Gr\xC3\xB6\xC3\x9F" "e", C_TEXT2);
    int vis = (ch - head) / ROW_H;
    for (int i = 0; i < vis && w->scroll + i < w->nent; i++) {
        FileEnt *e = &w->ents[w->scroll + i];
        int ry = y + head + i * ROW_H;
        int selected = w->scroll + i == w->sel, active = w == focused();
        if (selected)
            gfx_round_rect(s, x + U(6), ry + 1, cw - U(12), ROW_H - 2, U(5), active ? C_ACCENT : 0xDCDCE0, 255);
        else if (i % 2)
            gfx_fill(s, x, ry, cw, ROW_H, 0xF5F5F7);
        int isz = ROW_H - U(8), iy = ry + U(4);
        if (e->is_dir)
            folder_icon(s, x + U(14), iy, isz);
        else
            doc_icon(s, x + U(14), iy, isz, ends_with(e->name, ".bmp") ? 0x34C759 : ends_with(e->name, ".sh") ? 0x0A84FF :
                     ends_with(e->name, ".wav") || ends_with(e->name, ".mp3") ? 0xFF2D55 : 0);
        u32 tc = selected && active ? 0xFFFFFF : C_TEXT;
        int tty = ry + (ROW_H - text_height(font_ui, FS)) / 2;
        text_draw(s, font_ui, FS, x + U(40), tty, e->name, tc);
        if (!e->is_dir) {
            char sz[24];
            u64 b = e->size;
            if (b < 1024) snprintf(sz, sizeof(sz), "%llu Byte", (unsigned long long)b);
            else if (b < 1024 * 1024) snprintf(sz, sizeof(sz), "%llu KB", (unsigned long long)(b / 1024));
            else snprintf(sz, sizeof(sz), "%llu MB", (unsigned long long)(b >> 20));
            text_draw(s, font_ui, FS, x + cw - U(80), tty, sz, selected && active ? 0xFFFFFF : C_TEXT2);
        }
    }
}

void draw_text(Win *w, int x, int y, int cw, int ch)
{
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, C_WINDOW);
    int vis = (ch - U(16)) / CELL_H;
    for (int i = 0; i < vis && w->top + i < w->nlines; i++)
        text_draw(s, font_mono, FS_MONO, x + U(12), y + U(8) + i * CELL_H, w->lines[w->top + i], C_TEXT);
    if (w->nlines > vis) { /* Bildlaufleiste (schmal, abgerundet) */
        int bh = (ch - U(8)) * vis / w->nlines;
        if (bh < U(24))
            bh = U(24);
        int by = y + U(4) + (ch - U(8) - bh) * w->top / (w->nlines - vis > 0 ? w->nlines - vis : 1);
        gfx_round_rect(s, x + cw - U(10), by, U(6), bh, U(3), 0x000000, 70);
    }
}

void draw_image(Win *w, int x, int y, int cw, int ch)
{
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, 0x1C1C1E);
    if (!w->img.px) {
        text_draw(s, font_ui, FS, x + U(16), y + U(16), w->err, 0xFF6961);
        return;
    }
    int dw = cw, dh = (int)((s64)w->img.h * cw / w->img.w);
    if (dh > ch) {
        dh = ch;
        dw = (int)((s64)w->img.w * ch / w->img.h);
    }
    if (dw > w->img.w && dh > w->img.h) { /* nicht vergroessern */
        dw = w->img.w;
        dh = w->img.h;
    }
    gfx_draw_scaled(s, &w->img, x + (cw - dw) / 2, y + (ch - dh) / 2, dw, dh);
}

/* ======================================================================================================================
 * Rechner (Festkomma mit 6 Nachkommastellen)
 * ==================================================================================================================== */

#define FX 1000000LL

const char *const calc_keys[20] = {"C", "\xC2\xB1", "%", "/", "7", "8", "9", "*", "4", "5", "6", "-",
                                          "1", "2", "3", "+", "0", ".", "\xE2\x86\x90", "="};

static void calc_show(Win *w, s64 v)
{
    int neg = v < 0;
    u64 a = neg ? (u64)-v : (u64)v;
    char frac[8];
    snprintf(frac, sizeof(frac), "%06llu", (unsigned long long)(a % FX));
    int fl = 6;
    while (fl > 0 && frac[fl - 1] == '0')
        frac[--fl] = 0;
    snprintf(w->disp, sizeof(w->disp), "%s%llu%s%s", neg ? "-" : "", (unsigned long long)(a / FX), fl ? "," : "", frac);
}

static s64 calc_apply(Win *w, s64 a, s64 b)
{
    switch (w->op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a / 1000 * b / 1000;
    case '/':
        if (!b) {
            snprintf(w->disp, sizeof(w->disp), "Fehler");
            return 0;
        }
        return a * 1000 / b * 1000;
    default: return b;
    }
}

void calc_key(Win *w, const char *k)
{
    char c = k[0];
    if (c >= '0' && c <= '9') {
        if (w->fresh) {
            w->cur = 0;
            w->dec = 0;
            w->fresh = 0;
        }
        if (w->dec) {
            if (w->dec < FX) {
                w->dec *= 10;
                w->cur += (w->cur < 0 ? -1 : 1) * (c - '0') * (FX / w->dec);
            }
        } else if (w->cur < 100000000000LL * FX / 1000) {
            w->cur = w->cur * 10 + (w->cur < 0 ? -1 : 1) * (c - '0') * FX;
        }
        calc_show(w, w->cur);
        if (w->dec) { /* Nullen hinter dem Komma beim Tippen zeigen */
            if (!strchr(w->disp, ','))
                strcat(w->disp, ",");
        }
    } else if (c == '.' || c == ',') {
        if (w->fresh) {
            w->cur = 0;
            w->fresh = 0;
        }
        if (!w->dec)
            w->dec = 1;
        calc_show(w, w->cur);
        if (!strchr(w->disp, ','))
            strcat(w->disp, ",");
    } else if (c == 'C') {
        w->cur = w->acc = 0;
        w->op = 0;
        w->dec = 0;
        w->fresh = 1;
        calc_show(w, 0);
    } else if (strcmp(k, "\xC2\xB1") == 0) {
        w->cur = -w->cur;
        calc_show(w, w->cur);
    } else if (c == '%') {
        w->cur = w->cur / 100;
        calc_show(w, w->cur);
    } else if (strcmp(k, "\xE2\x86\x90") == 0 || c == '\b') {
        w->cur = w->cur / FX / 10 * FX;
        w->dec = 0;
        calc_show(w, w->cur);
    } else if (c == '=' || c == '\n') {
        if (w->op) {
            w->cur = calc_apply(w, w->acc, w->cur);
            w->op = 0;
            if (strcmp(w->disp, "Fehler") != 0)
                calc_show(w, w->cur);
        }
        w->fresh = 1;
        w->dec = 0;
    } else if (strchr("+-*/", c)) {
        if (w->op && !w->fresh)
            w->cur = calc_apply(w, w->acc, w->cur);
        w->acc = w->cur;
        w->op = c;
        w->fresh = 1;
        w->dec = 0;
        calc_show(w, w->cur);
    }
    win_dirty_all(w);
}

void open_calc(void)
{
    Win *w = new_window(W_CALC, "Rechner", 4 * U(58) + 5 * U(10), TITLE_H + U(96) + 5 * U(58) + 6 * U(10));
    if (!w)
        return;
    w->fresh = 1;
    calc_show(w, 0);
}

/* Lage der Tasten: Anzeige oben, darunter 4 x 5 runde Tasten */
static void calc_geom(Win *w, int *x, int *y, int *d, int *gap)
{
    int cx, cy, cw, ch;
    content_rect(w, &cx, &cy, &cw, &ch);
    *gap = U(10);
    int dw = (cw - 5 * *gap) / 4, dh = (ch - U(96) - 6 * *gap) / 5;
    *d = dw < dh ? dw : dh;
    *x = cx + (cw - 4 * *d - 3 * *gap) / 2;
    *y = cy + U(96) + *gap;
}

int calc_btn_at(Win *w, int px, int py)
{
    int x, y, d, gap;
    calc_geom(w, &x, &y, &d, &gap);
    for (int i = 0; i < 20; i++) {
        int bx = x + (i % 4) * (d + gap), by = y + (i / 4) * (d + gap);
        if (px >= bx && px < bx + d && py >= by && py < by + d)
            return i;
    }
    return -1;
}

void draw_calc(Win *w, int x, int y, int cw, int ch)
{
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, 0x2C2C2E);
    int dsz = U(44), tw = text_width(font_ui, dsz, w->disp);
    while (tw > cw - U(28) && dsz > U(18)) { /* lange Zahlen kleiner */
        dsz -= U(4);
        tw = text_width(font_ui, dsz, w->disp);
    }
    text_draw(s, font_ui, dsz, x + cw - U(16) - tw, y + U(88) - text_height(font_ui, dsz), w->disp, 0xFFFFFF);
    int bx0, by0, d, gap;
    calc_geom(w, &bx0, &by0, &d, &gap);
    for (int i = 0; i < 20; i++) {
        int bx = bx0 + (i % 4) * (d + gap), by = by0 + (i / 4) * (d + gap);
        int op = i % 4 == 3 || i == 19, top = i < 3;
        u32 c = op ? 0xFF9F0A : top ? 0xA5A5A5 : 0x505050, tc = top ? 0x000000 : 0xFFFFFF;
        if (op && w->op && calc_keys[i][0] == w->op && w->fresh) { /* gewaehlter Operator: invertiert */
            c = 0xFFFFFF;
            tc = 0xFF9F0A;
        }
        gfx_disc(s, bx + d * 0.5f, by + d * 0.5f, d * 0.5f, c, 255);
        const char *k = calc_keys[i];
        if (k[0] == '/' && !k[1]) k = "\xC3\xB7";
        else if (k[0] == '*' && !k[1]) k = "\xC3\x97";
        else if (k[0] == '-' && !k[1]) k = "\xE2\x88\x92";
        else if (k[0] == '.' && !k[1]) k = ",";
        int fs = top ? U(20) : U(24), kw = text_width(font_ui, fs, k);
        text_draw(s, font_ui, fs, bx + (d - kw) / 2, by + (d - text_height(font_ui, fs)) / 2, k, tc);
    }
}

/* ======================================================================================================================
 * Uhr, Info
 * ==================================================================================================================== */

/* sin(6 Grad * i) * 1000 */
static const short sin60[60] = {0,    105,  208,  309,  407,  500,  588,  669,  743,  809,  866,  914,  951,  978,  995,
                                1000, 995,  978,  951,  914,  866,  809,  743,  669,  588,  500,  407,  309,  208,  105,
                                0,    -105, -208, -309, -407, -500, -588, -669, -743, -809, -866, -914, -951, -978, -995,
                                -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588, -500, -407, -309, -208, -105};

static void hand(float cx, float cy, int pos60, float len, float back, float width, u32 c)
{
    int i = ((pos60 % 60) + 60) % 60;
    float dx = sin60[i] / 1000.0f, dy = -sin60[(i + 15) % 60] / 1000.0f;
    gfx_capsule(tgt, cx - dx * back, cy - dy * back, cx + dx * len, cy + dy * len, width, c, 255);
}

void open_clock(void)
{
    new_window(W_CLOCK, "Uhr", U(300), U(360));
}

void open_about(void)
{
    new_window(W_ABOUT, "Ü" "ber MiniKernel", U(480), U(260));
}

void draw_clock(Win *w, int x, int y, int cw, int ch)
{
    (void)w;
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, 0xF5F5F7);
    float r = (float)((cw < ch - U(70) ? cw : ch - U(70)) / 2 - U(16)), cx = x + cw * 0.5f, cy = y + U(16) + r;
    if (r < 20)
        return;
    gfx_shadow(s, (int)(cx - r), (int)(cy - r + U(3)), (int)(2 * r), (int)(2 * r), (int)r, U(14), 50);
    gfx_disc(s, cx, cy, r, 0xFFFFFF, 255);
    gfx_ring(s, cx, cy, r, U(1) * 1.5f, 0xD1D1D6, 255);
    for (int i = 0; i < 60; i++) {
        float dx = sin60[i] / 1000.0f, dy = -sin60[(i + 15) % 60] / 1000.0f, in = i % 5 ? r * 0.90f : r * 0.82f;
        gfx_capsule(s, cx + dx * in, cy + dy * in, cx + dx * r * 0.94f, cy + dy * r * 0.94f, i % 5 ? U(1) * 1.0f : U(1) * 2.5f,
                    i % 5 ? 0xAEAEB2 : 0x1D1D1F, 255);
    }
    static const char *num[4] = {"12", "3", "6", "9"};
    for (int k = 0; k < 4; k++) {
        int i = k * 15, fs = (int)(r * 0.20f);
        float dx = sin60[i] / 1000.0f, dy = -sin60[(i + 15) % 60] / 1000.0f;
        int tw = text_width(font_bold, fs, num[k]);
        text_draw(s, font_bold, fs, (int)(cx + dx * r * 0.66f) - tw / 2, (int)(cy + dy * r * 0.66f) - text_height(font_bold, fs) / 2,
                  num[k], 0x1D1D1F);
    }
    s64 now = sys_time();
    DateTime dt = {0, 0, 0, 0, 0, 0, 0};
    if (now > 0)
        time_to_date((u64)now, &dt);
    hand(cx, cy, dt.hour % 12 * 5 + dt.min / 12, r * 0.50f, 0, U(1) * 5.0f, 0x1D1D1F);
    hand(cx, cy, dt.min, r * 0.78f, 0, U(1) * 3.5f, 0x1D1D1F);
    hand(cx, cy, dt.sec, r * 0.86f, r * 0.15f, U(1) * 1.5f, 0xFF9500);
    gfx_disc(s, cx, cy, U(1) * 4.0f, 0xFF9500, 255);
    gfx_disc(s, cx, cy, U(1) * 1.5f, 0xFFFFFF, 255);
    char t[40];
    snprintf(t, sizeof(t), "%02d:%02d:%02d", dt.hour, dt.min, dt.sec);
    int fs = U(22), tw = text_width(font_bold, fs, t);
    text_draw(s, font_bold, fs, x + (cw - tw) / 2, (int)(cy + r + U(12)), t, C_TEXT);
    snprintf(t, sizeof(t), "%02d.%02d.%04d", dt.day, dt.month, dt.year);
    tw = text_width(font_ui, FS, t);
    text_draw(s, font_ui, FS, x + (cw - tw) / 2, (int)(cy + r + U(12)) + text_height(font_bold, fs), t, C_TEXT2);
}

void draw_about(Win *w, int x, int y, int cw, int ch)
{
    (void)w;
    (void)ch;
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, C_WINDOW);
    int isz = U(96);
    draw_app_icon(s, A_ABOUT, x + U(28), y + U(34), isz);
    int tx = x + U(28) + isz + U(28), ty = y + U(24);
    text_draw(s, font_bold, U(26), tx, ty, "MiniKernel", C_TEXT);
    ty += text_height(font_bold, U(26));
    text_draw(s, font_ui, FS, tx, ty, "Version 1.0", C_TEXT2);
    ty += text_height(font_ui, FS) + U(14);
    char t[128];
    int cpus = 0;
    CpuInfo ci;
    while (sys_cpuinfo((u64)cpus, &ci) == 0)
        cpus++;
    u64 up = (u64)sys_ticks() / 100;
    int procs = 0;
    ProcInfo pi;
    for (u64 i = 0; sys_procinfo(i, &pi) == 0; i++)
        if (!pi.state)
            procs++;
    const char *lab[4] = {"Prozessor", "Bildschirm", "L\xC3\xA4uft seit", "Prozesse"};
    char val[4][64];
    snprintf(val[0], sizeof(val[0]), "%d Kern%s (x86-64)", cpus, cpus == 1 ? "" : "e");
    snprintf(val[1], sizeof(val[1]), "%d \xC3\x97 %d", W, H);
    snprintf(val[2], sizeof(val[2]), "%llu:%02llu:%02llu", (unsigned long long)(up / 3600), (unsigned long long)(up / 60 % 60),
             (unsigned long long)(up % 60));
    snprintf(val[3], sizeof(val[3]), "%d", procs);
    for (int i = 0; i < 4; i++) {
        text_draw(s, font_bold, FS, tx, ty, lab[i], C_TEXT);
        text_draw(s, font_ui, FS, tx + U(100), ty, val[i], C_TEXT2);
        ty += text_height(font_ui, FS) + U(3);
    }
    snprintf(t, sizeof(t), "Eigener 64-Bit-Kernel mit UEFI-Bootloader");
    text_draw(s, font_ui, FS_SMALL, tx, ty + U(10), t, C_TEXT2);
}
