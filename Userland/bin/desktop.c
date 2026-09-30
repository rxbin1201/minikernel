#include "gfx.h"
#include "malloc.h"

/* desktop: grafische Oberflaeche mit Fenstern.
 *   Fenster: Terminal (eine echte Shell), Dateien, Textansicht, Bildansicht, Rechner, Uhr, Info.
 *   Fenster verschieben (Titelleiste ziehen), Groesse aendern (Ecke unten rechts), minimieren (_), schliessen (x).
 *   Taskleiste mit Startmenue, Fensterknoepfen und Uhr. Vollbild-Programme (Malen, Snake, Tetris) startet das Startmenue.
 *   "Zur Konsole" im Startmenue beendet den Desktop. */

#define TITLE_H   22
#define BORDER    3
#define TASKBAR_H 30
#define MAXW      16
#define ROW_H     18

enum { W_TERM, W_FILES, W_TEXT, W_IMAGE, W_CALC, W_CLOCK, W_ABOUT };

typedef struct {
    unsigned short ch;
    unsigned char  fg, bg;
} TCell;

typedef struct {
    char name[128];
    int  is_dir;
    u64  size;
} FileEnt;

typedef struct {
    int  used, kind, x, y, w, h, minimized;
    char title[80];
    /* Das Fenster wird in ein eigenes Bild gezeichnet (nur wenn sich sein Inhalt aendert); auf den Bildschirm wird es
     * nur kopiert. Neu zu zeichnen ist das Rechteck [rx0, rx1) x [ry0, ry1) in Fensterkoordinaten. */
    Surface buf;
    int     rx0, ry0, rx1, ry1;
    /* Terminal */
    int    pid, to_child, from_child, cols, rows, cx, cy, fg, bg, esc, escn, escp[8], u8need, exited;
    int    tr0, tr1; /* geaenderte Zeilen (tr0 > tr1: keine) */
    unsigned u8cp;
    TCell *cells;
    /* Dateien */
    char     dir[256];
    FileEnt *ents;
    int      nent, scroll, sel;
    /* Text */
    char  *text;
    char **lines;
    int    nlines, top;
    /* Bild */
    Surface img;
    char    err[96];
    /* Rechner (Festkomma, 6 Nachkommastellen) */
    char disp[40];
    s64  acc, cur;
    char op;
    int  fresh, dec;
} Win;

static Win     wins[MAXW];
static Win    *order[MAXW]; /* Stapel: order[nord-1] liegt oben und hat den Fokus */
static int     nord;
static Surface bg;
static Surface *tgt = &gfx_screen; /* Ziel der draw_*-Funktionen */
static int     W, H, menu_open, menu_hover = -1;
static int     drag_mode; /* 0 = nichts, 1 = verschieben, 2 = Groesse */
static Win    *drag_win;
static int     drag_dx, drag_dy;
static s64     last_click_tick;
static int     last_click_x, last_click_y;
static int     quit;

static const u32 term_pal[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xC0C0C0,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

/* ======================================================================================================================
 * Geaenderte Bereiche
 *   damage():   Bildschirmrechteck, das neu zusammengesetzt und angezeigt werden muss
 *   win_dirty(): Teil eines Fensters (Fensterkoordinaten), der in dessen Bild neu gezeichnet werden muss
 * ==================================================================================================================== */

#define MAXD 24
static Clip dmg[MAXD];
static int  ndmg;

static void damage(int x, int y, int w, int h)
{
    Clip r = {x < 0 ? 0 : x, y < 0 ? 0 : y, x + w > W ? W : x + w, y + h > H ? H : y + h};
    if (r.x0 >= r.x1 || r.y0 >= r.y1)
        return;
    for (int i = 0; i < ndmg;) { /* ueberlappende Rechtecke zusammenfassen */
        Clip *d = &dmg[i];
        if (d->x0 <= r.x1 && r.x0 <= d->x1 && d->y0 <= r.y1 && r.y0 <= d->y1) {
            if (d->x0 < r.x0) r.x0 = d->x0;
            if (d->y0 < r.y0) r.y0 = d->y0;
            if (d->x1 > r.x1) r.x1 = d->x1;
            if (d->y1 > r.y1) r.y1 = d->y1;
            dmg[i] = dmg[--ndmg];
            i = 0;
        } else {
            i++;
        }
    }
    if (ndmg == MAXD) { /* zu viele: alles zu einem Rechteck */
        for (int i = 0; i < ndmg; i++) {
            if (dmg[i].x0 < r.x0) r.x0 = dmg[i].x0;
            if (dmg[i].y0 < r.y0) r.y0 = dmg[i].y0;
            if (dmg[i].x1 > r.x1) r.x1 = dmg[i].x1;
            if (dmg[i].y1 > r.y1) r.y1 = dmg[i].y1;
        }
        ndmg = 0;
    }
    dmg[ndmg++] = r;
}

static void damage_all(void) { damage(0, 0, W, H); }
static void damage_taskbar(void) { damage(0, H - TASKBAR_H, W, TASKBAR_H); }

/* Fenster samt Schatten auf dem Bildschirm */
static void damage_win(const Win *w)
{
    if (!w->minimized)
        damage(w->x, w->y, w->w + 4, w->h + 4);
}

static void win_dirty(Win *w, int x, int y, int ww, int hh)
{
    if (w->rx0 >= w->rx1) {
        w->rx0 = x; w->ry0 = y; w->rx1 = x + ww; w->ry1 = y + hh;
        return;
    }
    if (x < w->rx0) w->rx0 = x;
    if (y < w->ry0) w->ry0 = y;
    if (x + ww > w->rx1) w->rx1 = x + ww;
    if (y + hh > w->ry1) w->ry1 = y + hh;
}

static void win_dirty_all(Win *w) { win_dirty(w, 0, 0, w->w, w->h); }

/* ======================================================================================================================
 * Fensterverwaltung
 * ==================================================================================================================== */

static void content_rect(const Win *w, int *x, int *y, int *cw, int *ch)
{
    *x = w->x + BORDER;
    *y = w->y + TITLE_H;
    *cw = w->w - 2 * BORDER;
    *ch = w->h - TITLE_H - BORDER;
}

static Win *new_window(int kind, const char *title, int w, int h)
{
    for (int i = 0; i < MAXW; i++) {
        if (!wins[i].used) {
            Win *n = &wins[i];
            memset(n, 0, sizeof(*n));
            n->used = 1;
            n->kind = kind;
            snprintf(n->title, sizeof(n->title), "%s", title);
            if (w > W - 20) w = W - 20;
            if (h > H - TASKBAR_H - 20) h = H - TASKBAR_H - 20;
            n->w = w;
            n->h = h;
            static int cascade;
            n->x = 40 + (cascade % 8) * 26;
            n->y = 30 + (cascade % 8) * 22;
            cascade++;
            if (n->x + w > W) n->x = W - w;
            if (n->y + h > H - TASKBAR_H) n->y = H - TASKBAR_H - h;
            n->tr0 = 1;
            order[nord++] = n;
            win_dirty_all(n);
            damage_win(n);
            damage_taskbar();
            return n;
        }
    }
    return 0;
}

static void raise_win(Win *w)
{
    int i = 0;
    while (i < nord && order[i] != w)
        i++;
    if (i == nord)
        return;
    for (; i < nord - 1; i++)
        order[i] = order[i + 1];
    order[nord - 1] = w;
    w->minimized = 0;
    damage_win(w);
    damage_taskbar();
}

static void minimize(Win *w)
{
    damage_win(w);
    damage_taskbar();
    w->minimized = 1;
}

static Win *focused(void)
{
    for (int i = nord - 1; i >= 0; i--)
        if (!order[i]->minimized)
            return order[i];
    return 0;
}

static void close_win(Win *w)
{
    if (w->kind == W_TERM) {
        if (w->to_child >= 0)
            sys_close(w->to_child);
        if (w->from_child >= 0)
            sys_close(w->from_child);
        if (w->pid > 0) {
            sys_kill(w->pid);
            int code;
            sys_wait(w->pid, &code);
        }
        u_free(w->cells);
    }
    if (w->kind == W_FILES)
        u_free(w->ents);
    if (w->kind == W_TEXT) {
        u_free(w->text);
        u_free(w->lines);
    }
    if (w->kind == W_IMAGE && w->img.px)
        surface_free(&w->img);
    damage_win(w);
    damage_taskbar();
    if (w->buf.px)
        surface_free(&w->buf);
    int i = 0;
    while (i < nord && order[i] != w)
        i++;
    for (; i < nord - 1; i++)
        order[i] = order[i + 1];
    if (nord)
        nord--;
    w->used = 0;
    if (drag_win == w)
        drag_mode = 0;
}

/* ======================================================================================================================
 * Terminal
 * ==================================================================================================================== */

static void term_alloc(Win *w)
{
    int x, y, cw, ch;
    content_rect(w, &x, &y, &cw, &ch);
    int cols = (cw - 4) / 8, rows = (ch - 4) / 16;
    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;
    TCell *n = u_malloc(sizeof(TCell) * (u64)(cols * rows));
    if (!n)
        return;
    for (int i = 0; i < cols * rows; i++) {
        n[i].ch = ' ';
        n[i].fg = 7;
        n[i].bg = 0;
    }
    if (w->cells) { /* alten Inhalt uebernehmen (unten ausgerichtet) */
        int shift = w->cy >= rows ? w->cy - rows + 1 : 0;
        for (int r = 0; r < rows && r + shift < w->rows; r++)
            for (int c = 0; c < cols && c < w->cols; c++)
                n[r * cols + c] = w->cells[(r + shift) * w->cols + c];
        w->cy -= shift;
        u_free(w->cells);
    }
    w->cells = n;
    w->cols = cols;
    w->rows = rows;
    if (w->cx >= cols) w->cx = cols - 1;
    if (w->cy >= rows) w->cy = rows - 1;
}

static void term_touch(Win *w, int r0, int r1)
{
    if (w->tr0 > w->tr1) {
        w->tr0 = r0;
        w->tr1 = r1;
        return;
    }
    if (r0 < w->tr0) w->tr0 = r0;
    if (r1 > w->tr1) w->tr1 = r1;
}

static void term_scroll(Win *w)
{
    term_touch(w, 0, w->rows - 1);
    memmove(w->cells, w->cells + w->cols, sizeof(TCell) * (u64)(w->cols * (w->rows - 1)));
    for (int c = 0; c < w->cols; c++) {
        TCell *t = &w->cells[(w->rows - 1) * w->cols + c];
        t->ch = ' ';
        t->fg = 7;
        t->bg = 0;
    }
}

static void term_newline(Win *w)
{
    w->cx = 0;
    if (++w->cy >= w->rows) {
        term_scroll(w);
        w->cy = w->rows - 1;
    }
}

static void term_char(Win *w, unsigned cp)
{
    if (w->cx >= w->cols)
        term_newline(w);
    TCell *t = &w->cells[w->cy * w->cols + w->cx];
    t->ch = cp > 0xFFFF ? '?' : (unsigned short)cp;
    t->fg = (unsigned char)w->fg;
    t->bg = (unsigned char)w->bg;
    w->cx++;
}

static void term_csi(Win *w, char f)
{
    int n = w->escn + 1;
    if (f == 'm') {
        for (int i = 0; i < n; i++) {
            int p = w->escp[i];
            if (p == 0) { w->fg = 7; w->bg = 0; }
            else if (p == 1 && w->fg < 8) w->fg += 8;
            else if (p == 7) { int t = w->fg; w->fg = w->bg; w->bg = t; }
            else if (p >= 30 && p <= 37) w->fg = p - 30;
            else if (p >= 90 && p <= 97) w->fg = p - 90 + 8;
            else if (p == 39) w->fg = 7;
            else if (p >= 40 && p <= 47) w->bg = p - 40;
            else if (p == 49) w->bg = 0;
        }
    } else if (f == 'K') {
        for (int c = w->cx; c < w->cols; c++) {
            TCell *t = &w->cells[w->cy * w->cols + c];
            t->ch = ' ';
            t->bg = (unsigned char)w->bg;
        }
    } else if (f == 'J') {
        term_touch(w, 0, w->rows - 1);
        for (int i = 0; i < w->cols * w->rows; i++) {
            w->cells[i].ch = ' ';
            w->cells[i].bg = 0;
        }
        w->cx = w->cy = 0;
    } else if (f == 'H') {
        int r = w->escp[0] > 0 ? w->escp[0] - 1 : 0, c = n > 1 && w->escp[1] > 0 ? w->escp[1] - 1 : 0;
        w->cy = r < w->rows ? r : w->rows - 1;
        w->cx = c < w->cols ? c : w->cols - 1;
    }
}

static void term_byte2(Win *w, unsigned char b);

/* Ein Byte der Ausgabe verarbeiten; die Zeile des Cursors vorher und nachher wird neu gezeichnet */
static void term_byte(Win *w, unsigned char b)
{
    term_touch(w, w->cy, w->cy);
    term_byte2(w, b);
    term_touch(w, w->cy, w->cy);
}

static void term_byte2(Win *w, unsigned char b)
{
    if (w->esc == 1) {
        if (b == '[') {
            w->esc = 2;
            w->escn = 0;
            memset(w->escp, 0, sizeof(w->escp));
        } else {
            w->esc = 0;
        }
        return;
    }
    if (w->esc == 2) {
        if (b >= '0' && b <= '9') {
            if (w->escp[w->escn] < 10000)
                w->escp[w->escn] = w->escp[w->escn] * 10 + (b - '0');
        } else if (b == ';') {
            if (w->escn < 7)
                w->escn++;
        } else {
            w->esc = 0;
            term_csi(w, (char)b);
        }
        return;
    }
    if (w->u8need) {
        if ((b & 0xC0) == 0x80) {
            w->u8cp = (w->u8cp << 6) | (b & 0x3F);
            if (--w->u8need == 0)
                term_char(w, w->u8cp);
            return;
        }
        w->u8need = 0;
        term_char(w, 0xFFFD);
    }
    if (b >= 0x80) {
        if ((b & 0xE0) == 0xC0) { w->u8cp = b & 0x1F; w->u8need = 1; }
        else if ((b & 0xF0) == 0xE0) { w->u8cp = b & 0x0F; w->u8need = 2; }
        else if ((b & 0xF8) == 0xF0) { w->u8cp = b & 0x07; w->u8need = 3; }
        else term_char(w, 0xFFFD);
        return;
    }
    switch (b) {
    case 0x1B: w->esc = 1; break;
    case '\n': term_newline(w); break;
    case '\r': w->cx = 0; break;
    case '\b': if (w->cx > 0) w->cx--; break;
    case '\t': w->cx = (w->cx + 8) & ~7; if (w->cx >= w->cols) term_newline(w); break;
    case 7: break;
    default:
        if (b >= 32)
            term_char(w, b);
    }
}

static void open_terminal(void)
{
    Win *w = new_window(W_TERM, "Terminal", 8 * 82 + 2 * BORDER + 4, 16 * 26 + TITLE_H + BORDER + 4);
    if (!w)
        return;
    w->to_child = w->from_child = -1;
    w->fg = 7;
    term_alloc(w);
    int in[2], out[2];
    if (sys_pipe(in) < 0 || sys_pipe(out) < 0) {
        close_win(w);
        return;
    }
    s64 pid = sys_fork();
    if (pid == 0) {
        sys_dup2(in[0], 0);
        sys_dup2(out[1], 1);
        sys_dup2(out[1], 2);
        for (int fd = 3; fd < 64; fd++) /* keine Pipes anderer Fenster erben */
            sys_close(fd);
        sys_exec("/bin/sh", "sh");
        sys_exit(127);
    }
    sys_close(in[0]);
    sys_close(out[1]);
    if (pid < 0) {
        sys_close(in[1]);
        sys_close(out[0]);
        close_win(w);
        return;
    }
    w->pid = (int)pid;
    w->to_child = in[1];
    w->from_child = out[0];
    const char *hello = "\x1b[36mTerminal im Desktop\x1b[0m (Programme mit Vollbild wie edit laufen hier nicht)\r\n";
    for (const char *p = hello; *p; p++)
        term_byte(w, (unsigned char)*p);
}

static void term_poll(Win *w)
{
    if (w->from_child < 0)
        return;
    unsigned char buf[2048];
    for (int round = 0; round < 8; round++) {
        s64 n = sys_fdavail(w->from_child);
        if (n < 0) { /* Shell beendet */
            w->exited = 1;
            return;
        }
        if (n == 0)
            return;
        s64 r = sys_read(w->from_child, buf, n < (s64)sizeof(buf) ? (u64)n : sizeof(buf));
        if (r <= 0)
            return;
        for (s64 i = 0; i < r; i++)
            term_byte(w, buf[i]);
    }
}

/* Geaenderte Zeilen des Terminals zum Neuzeichnen vormerken */
static void term_flush(Win *w)
{
    if (w->tr0 > w->tr1)
        return;
    win_dirty(w, BORDER, TITLE_H + 2 + w->tr0 * 16, w->w - 2 * BORDER, (w->tr1 - w->tr0 + 1) * 16);
    w->tr0 = 1;
    w->tr1 = 0;
}

static void draw_terminal(Win *w, int x, int y, int cw, int ch)
{
    gfx_fill(tgt, x, y, cw, ch, 0);
    int ox = x + 2, oy = y + 2, active = w == focused();
    for (int r = 0; r < w->rows; r++) {
        if (oy + r * 16 + 16 <= gfx_clip.y0 || oy + r * 16 >= gfx_clip.y1)
            continue; /* Zeile liegt nicht im neu zu zeichnenden Bereich */
        for (int c = 0; c < w->cols; c++) {
            TCell *t = &w->cells[r * w->cols + c];
            u32 fgc = term_pal[t->fg & 15], bgc = term_pal[t->bg & 15];
            if (active && r == w->cy && c == w->cx) { /* Cursor */
                u32 tmp = fgc;
                fgc = bgc;
                bgc = tmp == bgc ? 0xC0C0C0 : tmp;
            }
            if (t->ch == ' ' && bgc == 0)
                continue;
            gfx_char(tgt, ox + c * 8, oy + r * 16, t->ch, fgc, bgc, 1);
        }
    }
}

static void term_key(Win *w, int k)
{
    if (w->to_child < 0)
        return;
    unsigned char b = (unsigned char)k;
    sys_write(w->to_child, &b, 1);
}

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

static void open_files(const char *dir)
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

static void files_open_entry(Win *w, int i)
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

static void draw_files(Win *w, int x, int y, int cw, int ch)
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

static void draw_text(Win *w, int x, int y, int cw, int ch)
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

static void draw_image(Win *w, int x, int y, int cw, int ch)
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
static const char *const calc_keys[20] = {"C", "\xC2\xB1", "%", "/", "7", "8", "9", "*", "4", "5", "6", "-",
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

static void calc_key(Win *w, const char *k)
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

static void open_calc(void)
{
    Win *w = new_window(W_CALC, "Rechner", 4 * 56 + 16 + 2 * BORDER, 5 * 44 + 64 + TITLE_H + BORDER);
    if (!w)
        return;
    w->fresh = 1;
    calc_show(w, 0);
}

static int calc_btn_at(Win *w, int px, int py)
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

static void draw_calc(Win *w, int x, int y, int cw, int ch)
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

static void draw_clock(Win *w, int x, int y, int cw, int ch)
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

static void draw_about(Win *w, int x, int y, int cw, int ch)
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

/* ======================================================================================================================
 * Zeichnen der ganzen Oberflaeche
 * ==================================================================================================================== */

static void make_background(void)
{
    surface_new(&bg, W, H);
    for (int y = 0; y < H; y++) {
        int t = y * 256 / H;
        u32 c = RGB(40 - 25 * t / 256, 95 - 60 * t / 256, 165 - 90 * t / 256);
        for (int x = 0; x < W; x++)
            bg.px[(u64)y * (u64)W + (u64)x] = c;
    }
    gfx_no_clip();
    const char *logo = "MiniKernel";
    int lw = gfx_text_width(logo) * 4;
    gfx_text_scaled(&bg, (W - lw) / 2, (H - TASKBAR_H) / 2 - 32, logo, RGB(70, 120, 190), GFX_TRANSPARENT, 4);
}

static void draw_window(Win *w)
{
    Surface *s = tgt;
    int active = w == focused();
    gfx_no_clip();
    gfx_fill(s, w->x, w->y, w->w, w->h, RGB(205, 208, 218));
    gfx_bevel(s, w->x, w->y, w->w, w->h, 1);
    for (int i = 0; i < TITLE_H - 2; i++) { /* Titelleiste mit Verlauf */
        u32 c = active ? RGB(30 + i * 2, 80 + i * 3, 170 + i * 2) : RGB(120 + i, 125 + i, 140 + i);
        gfx_fill(s, w->x + 1, w->y + 1 + i, w->w - 2, 1, c);
    }
    gfx_set_clip(w->x, w->y, w->w - 48, TITLE_H);
    gfx_text(s, w->x + 8, w->y + 3, w->title, RGB(255, 255, 255), GFX_TRANSPARENT);
    gfx_no_clip();
    int bx = w->x + w->w - 22; /* Schliessen */
    gfx_fill(s, bx, w->y + 3, 18, 16, RGB(210, 60, 60));
    gfx_bevel(s, bx, w->y + 3, 18, 16, 1);
    gfx_text(s, bx + 5, w->y + 3, "x", RGB(255, 255, 255), GFX_TRANSPARENT);
    bx -= 22; /* Minimieren */
    gfx_fill(s, bx, w->y + 3, 18, 16, RGB(200, 204, 214));
    gfx_bevel(s, bx, w->y + 3, 18, 16, 1);
    gfx_text(s, bx + 5, w->y + 1, "_", RGB(20, 20, 30), GFX_TRANSPARENT);

    int x, y, cw, ch;
    content_rect(w, &x, &y, &cw, &ch);
    gfx_set_clip(x, y, cw, ch);
    switch (w->kind) {
    case W_TERM:  draw_terminal(w, x, y, cw, ch); break;
    case W_FILES: draw_files(w, x, y, cw, ch); break;
    case W_TEXT:  draw_text(w, x, y, cw, ch); break;
    case W_IMAGE: draw_image(w, x, y, cw, ch); break;
    case W_CALC:  draw_calc(w, x, y, cw, ch); break;
    case W_CLOCK: draw_clock(w, x, y, cw, ch); break;
    case W_ABOUT: draw_about(w, x, y, cw, ch); break;
    }
    gfx_no_clip();
    for (int i = 0; i < 3; i++) /* Griff zum Vergroessern */
        gfx_line(s, w->x + w->w - 4 - i * 4, w->y + w->h - 2, w->x + w->w - 2, w->y + w->h - 4 - i * 4, RGB(100, 100, 115));
}

/* Startmenue */
typedef struct {
    const char *label;
    int         action;
} MenuItem;
enum { A_TERM, A_FILES, A_CALC, A_CLOCK, A_ABOUT, A_PAINT, A_SNAKE, A_TETRIS, A_QUIT, A_SEP };
static const MenuItem menu[] = {
    {"Terminal", A_TERM}, {"Dateien", A_FILES}, {"Rechner", A_CALC}, {"Uhr", A_CLOCK}, {"Info", A_ABOUT},
    {"", A_SEP}, {"Malen (Vollbild)", A_PAINT}, {"Snake (Vollbild)", A_SNAKE}, {"Tetris (Vollbild)", A_TETRIS},
    {"", A_SEP}, {"Zur Konsole", A_QUIT},
};
#define NMENU ((int)(sizeof(menu) / sizeof(menu[0])))
#define MENU_W 200

static int menu_item_y(int i)
{
    int y = H - TASKBAR_H - 6;
    for (int k = NMENU - 1; k >= i; k--)
        y -= menu[k].action == A_SEP ? 8 : 24;
    return y;
}

static void draw_taskbar(void)
{
    Surface *s = tgt;
    int y = H - TASKBAR_H;
    gfx_no_clip();
    gfx_fill(s, 0, y, W, TASKBAR_H, RGB(30, 34, 46));
    gfx_fill(s, 0, y, W, 1, RGB(90, 100, 130));
    gfx_fill(s, 4, y + 4, 70, TASKBAR_H - 8, menu_open ? RGB(60, 140, 60) : RGB(50, 110, 50));
    gfx_bevel(s, 4, y + 4, 70, TASKBAR_H - 8, !menu_open);
    gfx_text(s, 19, y + 7, "Start", RGB(255, 255, 255), GFX_TRANSPARENT);
    int bx = 82;
    Win *f = focused();
    for (int i = 0; i < MAXW; i++) {
        Win *w = &wins[i];
        if (!w->used)
            continue;
        if (bx + 150 > W - 70)
            break;
        u32 c = w == f ? RGB(80, 95, 130) : w->minimized ? RGB(40, 44, 58) : RGB(55, 62, 82);
        gfx_fill(s, bx, y + 4, 146, TASKBAR_H - 8, c);
        gfx_bevel(s, bx, y + 4, 146, TASKBAR_H - 8, w != f);
        gfx_set_clip(bx, y, 140, TASKBAR_H);
        gfx_text(s, bx + 6, y + 7, w->title, RGB(230, 230, 240), GFX_TRANSPARENT);
        gfx_no_clip();
        bx += 150;
    }
    s64 now = sys_time();
    if (now > 0) {
        DateTime dt;
        time_to_date((u64)now, &dt);
        char t[16];
        snprintf(t, sizeof(t), "%02d:%02d", dt.hour, dt.min);
        gfx_text(s, W - 50, y + 7, t, RGB(230, 230, 240), GFX_TRANSPARENT);
    }
    if (menu_open) {
        int my = menu_item_y(0) - 4;
        gfx_fill(s, 4, my, MENU_W, y - my, RGB(235, 237, 245));
        gfx_bevel(s, 4, my, MENU_W, y - my, 1);
        for (int i = 0; i < NMENU; i++) {
            int iy = menu_item_y(i);
            if (menu[i].action == A_SEP) {
                gfx_fill(s, 10, iy + 3, MENU_W - 12, 1, RGB(160, 160, 175));
                continue;
            }
            int hover = i == menu_hover;
            if (hover)
                gfx_fill(s, 6, iy, MENU_W - 4, 24, RGB(60, 110, 200));
            gfx_text(s, 16, iy + 4, menu[i].label, hover ? RGB(255, 255, 255) : RGB(20, 20, 30), GFX_TRANSPARENT);
        }
    }
}

/* Menueeintrag unter (x, y), -1 = keiner */
static int menu_hit(int x, int y)
{
    for (int i = 0; i < NMENU; i++) {
        int iy = menu_item_y(i);
        if (menu[i].action != A_SEP && x >= 4 && x < 4 + MENU_W && y >= iy && y < iy + 24)
            return i;
    }
    return -1;
}

static void damage_menu(void)
{
    int my = menu_item_y(0) - 4;
    damage(0, my, MENU_W + 8, H - my);
}

/* Geaenderten Teil eines Fensters in sein eigenes Bild zeichnen und auf dem Bildschirm als geaendert melden */
static void render_window(Win *w)
{
    if (!w->buf.px || w->buf.w != w->w || w->buf.h != w->h) {
        if (w->buf.px)
            surface_free(&w->buf);
        if (surface_new(&w->buf, w->w, w->h) != 0) {
            w->buf.px = 0;
            return;
        }
        win_dirty_all(w);
    }
    if (w->rx0 >= w->rx1)
        return;
    int sx = w->x, sy = w->y;
    w->x = w->y = 0; /* draw_window zeichnet an (w->x, w->y): hier die linke obere Ecke des Fensterbildes */
    tgt = &w->buf;
    gfx_set_base_clip(w->rx0, w->ry0, w->rx1 - w->rx0, w->ry1 - w->ry0);
    draw_window(w);
    gfx_reset_base_clip();
    tgt = &gfx_screen;
    w->x = sx;
    w->y = sy;
    if (!w->minimized)
        damage(w->x + w->rx0, w->y + w->ry0, w->rx1 - w->rx0, w->ry1 - w->ry0);
    w->rx0 = w->rx1 = 0;
}

/* Rechteck des Bildschirms aus Hintergrund, Fensterbildern und Taskleiste zusammensetzen und anzeigen */
static void compose(const Clip *r)
{
    int x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
    for (int y = y0; y < y1; y++)
        memcpy(gfx_screen.px + (u64)y * (u64)W + (u64)x0, bg.px + (u64)y * (u64)W + (u64)x0, (u64)(x1 - x0) * 4);
    gfx_set_base_clip(x0, y0, x1 - x0, y1 - y0);
    for (int i = 0; i < nord; i++) {
        Win *w = order[i];
        if (w->minimized || !w->buf.px)
            continue;
        gfx_fill(&gfx_screen, w->x + 4, w->y + 4, w->w, w->h, RGB(10, 20, 40)); /* Schatten */
        int ax = w->x > x0 ? w->x : x0, ay = w->y > y0 ? w->y : y0;
        int bx = w->x + w->w < x1 ? w->x + w->w : x1, by = w->y + w->h < y1 ? w->y + w->h : y1;
        for (int y = ay; y < by; y++)
            memcpy(gfx_screen.px + (u64)y * (u64)W + (u64)ax, w->buf.px + (u64)(y - w->y) * (u64)w->w + (u64)(ax - w->x),
                   bx > ax ? (u64)(bx - ax) * 4 : 0);
    }
    if (y1 > H - TASKBAR_H || menu_open)
        draw_taskbar();
    gfx_reset_base_clip();
    gfx_present(x0, y0, x1 - x0, y1 - y0);
}

static void draw_all(void)
{
    for (int i = 0; i < nord; i++)
        render_window(order[i]);
    for (int i = 0; i < ndmg; i++)
        compose(&dmg[i]);
    ndmg = 0;
}

/* ======================================================================================================================
 * Eingaben
 * ==================================================================================================================== */

/* Startet ein Vollbild-Programm und kommt danach zurueck */
static void run_fullscreen(const char *path, const char *cmdline)
{
    gfx_suspend();
    s64 pid = sys_fork();
    if (pid == 0) {
        for (int fd = 3; fd < 64; fd++)
            sys_close(fd);
        sys_setpgid(0, 0);
        sys_exec(path, cmdline);
        sys_exit(127);
    }
    if (pid > 0) {
        int code;
        sys_wait((int)pid, &code);
    }
    gfx_resume();
    while (sys_getchar() >= 0) /* Tasten, die fuer das Programm gedacht waren, nicht weiterreichen */
        ;
    damage_all();
}

static void do_action(int a)
{
    damage_menu();
    damage_taskbar();
    menu_open = 0;
    switch (a) {
    case A_TERM: open_terminal(); break;
    case A_FILES: open_files("/"); break;
    case A_CALC: open_calc(); break;
    case A_CLOCK: new_window(W_CLOCK, "Uhr", 260, 300); break;
    case A_ABOUT: new_window(W_ABOUT, "Info", 400, 220); break;
    case A_PAINT: run_fullscreen("/bin/paint", "paint"); break;
    case A_SNAKE: run_fullscreen("/bin/snake", "snake"); break;
    case A_TETRIS: run_fullscreen("/bin/tetris", "tetris"); break;
    case A_QUIT: quit = 1; break;
    }
}

static Win *window_at(int x, int y)
{
    for (int i = nord - 1; i >= 0; i--) {
        Win *w = order[i];
        if (!w->minimized && x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h)
            return w;
    }
    return 0;
}

static void content_click(Win *w, int px, int py, int dbl)
{
    int x, y, cw, ch;
    content_rect(w, &x, &y, &cw, &ch);
    if (w->kind == W_FILES) {
        int i = w->scroll + (py - y) / ROW_H;
        if (i < w->nent) {
            w->sel = i;
            if (dbl)
                files_open_entry(w, i);
        }
    } else if (w->kind == W_CALC) {
        int b = calc_btn_at(w, px, py);
        if (b >= 0)
            calc_key(w, calc_keys[b]);
    }
    win_dirty_all(w);
}

static void mouse_down(Event *e)
{
    int y0 = H - TASKBAR_H;
    if (menu_open) {
        int i = menu_hit(e->x, e->y);
        if (i >= 0) {
            do_action(menu[i].action);
            return;
        }
        damage_menu();
        damage_taskbar();
        menu_open = 0;
        if (e->y < y0)
            return;
    }
    if (e->y >= y0) { /* Taskleiste */
        if (e->x < 78) {
            menu_open = !menu_open;
            menu_hover = -1;
            damage_menu();
            damage_taskbar();
            return;
        }
        int bx = 82;
        for (int i = 0; i < MAXW; i++) {
            Win *w = &wins[i];
            if (!w->used)
                continue;
            if (e->x >= bx && e->x < bx + 146) {
                if (w == focused())
                    minimize(w);
                else
                    raise_win(w);
                return;
            }
            bx += 150;
        }
        return;
    }
    Win *w = window_at(e->x, e->y);
    if (!w)
        return;
    if (w != focused())
        raise_win(w);
    if (e->button != 1)
        return;
    int dbl = sys_ticks() - last_click_tick < 40 && e->x - last_click_x < 5 && last_click_x - e->x < 5 &&
              e->y - last_click_y < 5 && last_click_y - e->y < 5;
    last_click_tick = dbl ? 0 : sys_ticks();
    last_click_x = e->x;
    last_click_y = e->y;
    if (e->y < w->y + TITLE_H) {
        if (e->x >= w->x + w->w - 22) {
            close_win(w);
        } else if (e->x >= w->x + w->w - 44) {
            minimize(w);
        } else {
            drag_mode = 1;
            drag_win = w;
            drag_dx = e->x - w->x;
            drag_dy = e->y - w->y;
        }
        return;
    }
    if (e->x >= w->x + w->w - 14 && e->y >= w->y + w->h - 14) {
        drag_mode = 2;
        drag_win = w;
        drag_dx = w->x + w->w - e->x;
        drag_dy = w->y + w->h - e->y;
        return;
    }
    content_click(w, e->x, e->y, dbl);
}

static void mouse_move(Event *e)
{
    if (menu_open) { /* Hervorhebung im Menue */
        int h = menu_hit(e->x, e->y);
        if (h != menu_hover) {
            menu_hover = h;
            damage_menu();
        }
    }
    if (!drag_mode)
        return;
    Win *w = drag_win;
    damage_win(w); /* alte Stelle */
    if (drag_mode == 1) {
        w->x = e->x - drag_dx;
        w->y = e->y - drag_dy;
        if (w->y < 0) w->y = 0;
        if (w->y > H - TASKBAR_H - TITLE_H) w->y = H - TASKBAR_H - TITLE_H;
        if (w->x > W - 60) w->x = W - 60;
        if (w->x + w->w < 60) w->x = 60 - w->w;
    } else {
        int nw = e->x + drag_dx - w->x, nh = e->y + drag_dy - w->y;
        w->w = nw < 180 ? 180 : nw;
        w->h = nh < 120 ? 120 : nh;
        if (w->kind == W_TERM)
            term_alloc(w);
        win_dirty_all(w);
    }
    damage_win(w); /* neue Stelle */
}

static void key(int k)
{
    if (k == 0x1B && menu_open) {
        damage_menu();
        damage_taskbar();
        menu_open = 0;
        return;
    }
    Win *w = focused();
    if (!w)
        return;
    if (w->kind == W_TERM) {
        term_key(w, k);
    } else if (w->kind == W_CALC) {
        char s[2] = {(char)k, 0};
        if (k == '\b' || k == 0x7F)
            calc_key(w, "\xE2\x86\x90");
        else if (k == 'c' || k == 'C' || k == 0x1B)
            calc_key(w, "C");
        else if (k == ',')
            calc_key(w, ".");
        else
            calc_key(w, s);
    } else if (w->kind == W_FILES) {
        if (k == KEY_DOWN && w->sel + 1 < w->nent) w->sel++;
        else if (k == KEY_UP && w->sel > 0) w->sel--;
        else if (k == '\n') files_open_entry(w, w->sel);
        else if (k == '\b' || k == 0x7F) files_open_entry(w, 0 < w->nent && strcmp(w->ents[0].name, "..") == 0 ? 0 : -1);
        int x, y, cw, ch;
        content_rect(w, &x, &y, &cw, &ch);
        int vis = ch / ROW_H;
        if (w->sel < w->scroll) w->scroll = w->sel;
        if (w->sel >= w->scroll + vis) w->scroll = w->sel - vis + 1;
        win_dirty_all(w);
    } else if (w->kind == W_TEXT) {
        int x, y, cw, ch;
        content_rect(w, &x, &y, &cw, &ch);
        int vis = (ch - 4) / 16;
        if (k == KEY_DOWN) w->top++;
        else if (k == KEY_UP) w->top--;
        else if (k == KEY_PGDN || k == ' ') w->top += vis;
        else if (k == KEY_PGUP) w->top -= vis;
        else if (k == KEY_HOME) w->top = 0;
        else if (k == KEY_END) w->top = w->nlines;
        if (w->top > w->nlines - vis) w->top = w->nlines - vis;
        if (w->top < 0) w->top = 0;
        win_dirty_all(w);
    }
}

static void wheel(Event *e)
{
    Win *w = window_at(e->x, e->y);
    if (!w)
        return;
    int x, y, cw, ch;
    content_rect(w, &x, &y, &cw, &ch);
    if (w->kind == W_FILES) {
        w->scroll -= e->wheel * 3;
        if (w->scroll > w->nent - ch / ROW_H) w->scroll = w->nent - ch / ROW_H;
        if (w->scroll < 0) w->scroll = 0;
    } else if (w->kind == W_TEXT) {
        int vis = (ch - 4) / 16;
        w->top -= e->wheel * 3;
        if (w->top > w->nlines - vis) w->top = w->nlines - vis;
        if (w->top < 0) w->top = 0;
    }
    win_dirty_all(w);
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (gfx_open() != 0)
        sys_exit(1);
    sys_tty_fg(0); /* Strg+C geht an die Fenster, nicht an den Desktop */
    W = gfx_screen.w;
    H = gfx_screen.h;
    make_background();
    open_terminal();
    damage_all();
    draw_all();

    s64 last_draw = 0, last_sec = -1, last_min = -1;
    Win *last_focus = focused();
    while (!quit) {
        Event e;
        int n = 0;
        while (n < 64 && gfx_poll(&e)) {
            n++;
            if (e.type == EV_KEY) key(e.key);
            else if (e.type == EV_DOWN) mouse_down(&e);
            else if (e.type == EV_UP) drag_mode = 0;
            else if (e.type == EV_MOVE) mouse_move(&e);
            else if (e.type == EV_WHEEL) wheel(&e);
        }
        for (int i = 0; i < MAXW; i++) {
            Win *w = &wins[i];
            if (w->used && w->kind == W_TERM) {
                term_poll(w);
                if (w->exited)
                    close_win(w);
                else
                    term_flush(w);
            }
        }
        Win *f = focused();
        if (f != last_focus) { /* Titelleiste (aktiv/inaktiv) und Terminal-Cursor beider Fenster */
            for (int i = 0; i < nord; i++)
                if (order[i] == last_focus)
                    win_dirty_all(last_focus);
            if (f)
                win_dirty_all(f);
            damage_taskbar();
            last_focus = f;
        }
        s64 now = sys_time();
        if (now != last_sec) { /* Uhren jede Sekunde */
            last_sec = now;
            for (int i = 0; i < nord; i++)
                if (order[i]->kind == W_CLOCK || order[i]->kind == W_ABOUT)
                    win_dirty_all(order[i]);
            if (now / 60 != last_min) { /* Uhrzeit in der Taskleiste */
                last_min = now / 60;
                damage(W - 60, H - TASKBAR_H, 60, TASKBAR_H);
            }
        }
        s64 t = sys_ticks();
        if (t - last_draw >= 2) { /* hoechstens etwa 50 Bilder pro Sekunde */
            draw_all();
            last_draw = t;
        }
        if (!n)
            sys_sleep_ms(10);
    }
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used)
            close_win(&wins[i]);
    gfx_close();
    sys_exit(0);
}
