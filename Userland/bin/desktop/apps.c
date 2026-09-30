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
    snprintf(w->title, sizeof(w->title), "Dateien - %s", w->dir);
    win_dirty_all(w);
    damage_taskbar();
}

void open_files(const char *dir)
{
    Win *w = new_window(W_FILES, "Dateien", 460, 380);
    if (!w)
        return;
    snprintf(w->dir, sizeof(w->dir), "%s", dir);
    files_load(w);
}

static void open_text(const char *path)
{
    const char *base = strrchr(path, '/');
    char t[80];
    snprintf(t, sizeof(t), "Text - %s", base ? base + 1 : path);
    Win *w = new_window(W_TEXT, t, 640, 420);
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
    char t[80];
    snprintf(t, sizeof(t), "Bild - %s", base ? base + 1 : path);
    Win *w = new_window(W_IMAGE, t, 520, 420);
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

void draw_files(Win *w, int x, int y, int cw, int ch)
{
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, RGB(250, 250, 252));
    int vis = ch / ROW_H;
    for (int i = 0; i < vis && w->scroll + i < w->nent; i++) {
        FileEnt *e = &w->ents[w->scroll + i];
        int ry = y + i * ROW_H;
        int selected = w->scroll + i == w->sel;
        if (selected)
            gfx_fill(s, x, ry, cw, ROW_H, RGB(190, 215, 255));
        u32 icon = e->is_dir ? RGB(240, 200, 60) : ends_with(e->name, ".bmp") ? RGB(200, 90, 200) :
                   ends_with(e->name, ".sh") ? RGB(60, 170, 80) : RGB(170, 175, 190);
        gfx_fill(s, x + 6, ry + 3, 14, 12, icon);
        if (e->is_dir)
            gfx_fill(s, x + 6, ry + 1, 7, 3, icon);
        gfx_text(s, x + 26, ry + 1, e->name, RGB(20, 20, 30), GFX_TRANSPARENT);
        if (!e->is_dir) {
            char sz[24];
            u64 b = e->size;
            if (b < 1024) snprintf(sz, sizeof(sz), "%llu B", (unsigned long long)b);
            else if (b < 1024 * 1024) snprintf(sz, sizeof(sz), "%llu KB", (unsigned long long)(b / 1024));
            else snprintf(sz, sizeof(sz), "%llu MB", (unsigned long long)(b >> 20));
            gfx_text(s, x + cw - gfx_text_width(sz) - 8, ry + 1, sz, RGB(110, 110, 120), GFX_TRANSPARENT);
        }
    }
}

void draw_text(Win *w, int x, int y, int cw, int ch)
{
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, RGB(255, 255, 250));
    int vis = (ch - 4) / 16;
    for (int i = 0; i < vis && w->top + i < w->nlines; i++)
        gfx_text(s, x + 4, y + 2 + i * 16, w->lines[w->top + i], RGB(20, 20, 30), GFX_TRANSPARENT);
    if (w->nlines > vis) { /* Bildlaufleiste */
        int bh = ch * vis / w->nlines;
        if (bh < 10)
            bh = 10;
        int by = y + (ch - bh) * w->top / (w->nlines - vis > 0 ? w->nlines - vis : 1);
        gfx_fill(s, x + cw - 6, y, 6, ch, RGB(225, 225, 230));
        gfx_fill(s, x + cw - 6, by, 6, bh, RGB(140, 140, 160));
    }
}

void draw_image(Win *w, int x, int y, int cw, int ch)
{
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, RGB(40, 40, 46));
    if (!w->img.px) {
        gfx_text(s, x + 8, y + 8, w->err, RGB(255, 120, 120), GFX_TRANSPARENT);
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
    Win *w = new_window(W_CALC, "Rechner", 4 * 56 + 16 + 2 * BORDER, 5 * 44 + 64 + TITLE_H + BORDER);
    if (!w)
        return;
    w->fresh = 1;
    calc_show(w, 0);
}

int calc_btn_at(Win *w, int px, int py)
{
    int x, y, cw, ch;
    content_rect(w, &x, &y, &cw, &ch);
    int bw = (cw - 16) / 4, bh = (ch - 64) / 5;
    for (int i = 0; i < 20; i++) {
        int bx = x + 8 + (i % 4) * bw, by = y + 56 + (i / 4) * bh;
        if (px >= bx && px < bx + bw - 4 && py >= by && py < by + bh - 4)
            return i;
    }
    return -1;
}

void draw_calc(Win *w, int x, int y, int cw, int ch)
{
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, RGB(210, 214, 224));
    gfx_fill(s, x + 8, y + 8, cw - 16, 40, RGB(235, 245, 235));
    gfx_bevel(s, x + 8, y + 8, cw - 16, 40, 0);
    int tw = gfx_text_width(w->disp) * 2;
    gfx_text_scaled(s, x + cw - 16 - tw, y + 12, w->disp, RGB(20, 40, 20), GFX_TRANSPARENT, 2);
    int bw = (cw - 16) / 4, bh = (ch - 64) / 5;
    for (int i = 0; i < 20; i++) {
        int bx = x + 8 + (i % 4) * bw, by = y + 56 + (i / 4) * bh;
        int op = i % 4 == 3 || i == 19;
        gfx_fill(s, bx, by, bw - 4, bh - 4, op ? RGB(250, 170, 70) : i < 3 ? RGB(190, 195, 205) : RGB(245, 245, 248));
        gfx_bevel(s, bx, by, bw - 4, bh - 4, 1);
        const char *k = calc_keys[i];
        int kw = gfx_text_width(k) * 2;
        gfx_text_scaled(s, bx + (bw - 4 - kw) / 2, by + (bh - 4 - 32) / 2, k, RGB(20, 20, 30), GFX_TRANSPARENT, 2);
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

static void hand(int cx, int cy, int pos60, int len, int width, u32 c)
{
    int i = ((pos60 % 60) + 60) % 60;
    int dx = sin60[i] * len / 1000, dy = -sin60[(i + 15) % 60] * len / 1000;
    gfx_thick_line(tgt,cx, cy, cx + dx, cy + dy, width, c);
}

void draw_clock(Win *w, int x, int y, int cw, int ch)
{
    (void)w;
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, RGB(235, 238, 245));
    int r = (cw < ch - 40 ? cw : ch - 40) / 2 - 10, cx = x + cw / 2, cy = y + r + 10;
    if (r < 20)
        return;
    gfx_fill_circle(s, cx, cy, r, RGB(255, 255, 255));
    gfx_circle(s, cx, cy, r, RGB(60, 60, 80));
    gfx_circle(s, cx, cy, r - 1, RGB(60, 60, 80));
    for (int i = 0; i < 60; i++) {
        int in = i % 5 ? r - 5 : r - 12;
        int dx = sin60[i], dy = -sin60[(i + 15) % 60];
        gfx_thick_line(s, cx + dx * in / 1000, cy + dy * in / 1000, cx + dx * (r - 3) / 1000, cy + dy * (r - 3) / 1000,
                       i % 5 ? 1 : 3, RGB(60, 60, 80));
    }
    s64 now = sys_time();
    DateTime dt = {0, 0, 0, 0, 0, 0, 0};
    if (now > 0)
        time_to_date((u64)now, &dt);
    hand(cx, cy, dt.hour % 12 * 5 + dt.min / 12, r * 5 / 10, 5, RGB(30, 30, 40));
    hand(cx, cy, dt.min, r * 8 / 10, 3, RGB(30, 30, 40));
    hand(cx, cy, dt.sec, r * 9 / 10, 1, RGB(210, 40, 40));
    gfx_fill_circle(s, cx, cy, 4, RGB(210, 40, 40));
    char t[40];
    snprintf(t, sizeof(t), "%02d:%02d:%02d  %02d.%02d.%04d", dt.hour, dt.min, dt.sec, dt.day, dt.month, dt.year);
    gfx_text(s, x + (cw - gfx_text_width(t)) / 2, cy + r + 8, t, RGB(30, 30, 40), GFX_TRANSPARENT);
}

void draw_about(Win *w, int x, int y, int cw, int ch)
{
    (void)w;
    Surface *s = tgt;
    gfx_fill(s, x, y, cw, ch, RGB(245, 246, 250));
    gfx_text_scaled(s, x + 16, y + 12, "MiniKernel", RGB(40, 90, 180), GFX_TRANSPARENT, 2);
    char t[128];
    u64 up = (u64)sys_ticks() / 100;
    int procs = 0;
    ProcInfo pi;
    for (u64 i = 0; sys_procinfo(i, &pi) == 0; i++)
        if (!pi.state)
            procs++;
    int ly = y + 52;
    snprintf(t, sizeof(t), "Bildschirm: %dx%d", W, H);
    gfx_text(s, x + 16, ly, t, RGB(30, 30, 40), GFX_TRANSPARENT);
    snprintf(t, sizeof(t), "L\xC3\xA4uft seit: %llu:%02llu:%02llu", (unsigned long long)(up / 3600), (unsigned long long)(up / 60 % 60),
             (unsigned long long)(up % 60));
    gfx_text(s, x + 16, ly + 20, t, RGB(30, 30, 40), GFX_TRANSPARENT);
    snprintf(t, sizeof(t), "Prozesse: %d", procs);
    gfx_text(s, x + 16, ly + 40, t, RGB(30, 30, 40), GFX_TRANSPARENT);
    gfx_text(s, x + 16, ly + 70, "Eigener 64-Bit-Kernel mit UEFI-Bootloader,", RGB(80, 80, 90), GFX_TRANSPARENT);
    gfx_text(s, x + 16, ly + 88, "Shell, Editor, USB, FAT/exFAT und Grafik.", RGB(80, 80, 90), GFX_TRANSPARENT);
}
