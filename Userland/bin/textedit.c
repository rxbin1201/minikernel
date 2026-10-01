#include "gfx.h"
#include "malloc.h"
#include "ui.h"

/* textedit [datei]: Texteditor fuer den Desktop.
 *   Markieren mit der Maus (Doppelklick: Wort, Dreifachklick: Zeile) oder mit Shift + Pfeilen/Pos1/Ende/Bild,
 *   Strg + Pfeil springt wortweise, Strg + Pos1/Ende an Anfang/Ende. Strg+A alles, Strg+C/X/V kopieren, ausschneiden,
 *   einfuegen (Zwischenablage des Systems), Strg+Z/Y rueckgaengig/wiederholen, Strg+S sichern.
 *   Werkzeugleiste: Neu (neues Fenster), Oeffnen (Pfad eingeben; der Desktop oeffnet die Datei im passenden
 *   Programm), Sichern, Sichern unter. Beim Schliessen mit ungesicherten Aenderungen kommt eine Rueckfrage.
 *   Dateien auf /disk brauchen kurze Namen (8.3, z.B. /disk/NOTIZ.TXT); die initrd ("/") ist nur lesbar. */

#define TABW     4   /* Tabulator: Spalten */
#define MAXUNDO  100

typedef struct {
    char *s;
    int   len, cap;
} Line;

static Line *L;
static int   nl, capl;
static int   cy, cx;          /* Cursor: Zeile, Byte in der Zeile */
static int   ay, ax, sel;     /* Anker der Markierung; sel = 1: von (ay, ax) bis zum Cursor ist markiert */
static int   top, left;       /* erste sichtbare Zeile und Spalte */
static int   want_col = -1;   /* Spalte, die Hoch/Runter halten moechte */
static char  path[256];
static int   modified, focus = 1;
static u64   saved_hash;      /* Pruefwert des zuletzt gesicherten/geladenen Textes: Rueckgaengig bis dorthin = unveraendert */
static char  status[160];
static s64   blink_t0;        /* Cursor blinkt ab hier */

/* Rueckfragen und Eingabefeld */
enum { P_NONE, P_OPEN, P_SAVEAS, P_CLOSE };
static int  prompt;
static char field[256];
static int  flen;
static int  close_after_save;

/* ---------------------------------------------------------------------------------------------------------------------
 * Zeilen
 * ------------------------------------------------------------------------------------------------------------------- */

static void line_reserve(Line *l, int n)
{
    if (n <= l->cap)
        return;
    int c = l->cap ? l->cap : 16;
    while (c < n)
        c *= 2;
    char *ns = u_malloc((u64)c);
    if (l->len)
        memcpy(ns, l->s, (size_t)l->len);
    u_free(l->s);
    l->s = ns;
    l->cap = c;
}

static void lines_reserve(int n)
{
    if (n <= capl)
        return;
    int c = capl ? capl : 64;
    while (c < n)
        c *= 2;
    Line *nlp = u_malloc(sizeof(Line) * (u64)c);
    memset(nlp, 0, sizeof(Line) * (u64)c);
    if (nl)
        memcpy(nlp, L, sizeof(Line) * (u64)nl);
    u_free(L);
    L = nlp;
    capl = c;
}

static void insert_line(int y) /* leere Zeile vor y */
{
    lines_reserve(nl + 1);
    memmove(&L[y + 1], &L[y], sizeof(Line) * (u64)(nl - y));
    memset(&L[y], 0, sizeof(Line));
    nl++;
}

static void remove_line(int y)
{
    u_free(L[y].s);
    memmove(&L[y], &L[y + 1], sizeof(Line) * (u64)(nl - y - 1));
    nl--;
}

static void clear_doc(void)
{
    for (int i = 0; i < nl; i++)
        u_free(L[i].s);
    nl = 0;
    insert_line(0);
    cy = cx = 0;
    sel = 0;
    top = left = 0;
}

/* ganzen Text setzen ('\n' trennt Zeilen, '\r' wird verworfen) */
static void set_text(const char *t, u64 n)
{
    for (int i = 0; i < nl; i++)
        u_free(L[i].s);
    nl = 0;
    insert_line(0);
    int y = 0;
    for (u64 i = 0; i < n; i++) {
        if (t[i] == '\n') {
            insert_line(++y);
        } else if (t[i] != '\r') {
            line_reserve(&L[y], L[y].len + 1);
            L[y].s[L[y].len++] = t[i];
        }
    }
}

/* ganzer Text als ein Puffer (u_free durch den Aufrufer) */
static char *get_text(u64 *n)
{
    u64 total = 0;
    for (int i = 0; i < nl; i++)
        total += (u64)L[i].len + 1;
    char *t = u_malloc(total + 1);
    u64 p = 0;
    for (int i = 0; i < nl; i++) {
        memcpy(t + p, L[i].s, (size_t)L[i].len);
        p += (u64)L[i].len;
        if (i + 1 < nl)
            t[p++] = '\n';
    }
    t[p] = 0;
    *n = p;
    return t;
}

/* ---------------------------------------------------------------------------------------------------------------------
 * UTF-8, Spalten, Woerter
 * ------------------------------------------------------------------------------------------------------------------- */

static int next_pos(const Line *l, int x)
{
    if (x >= l->len)
        return l->len;
    x++;
    while (x < l->len && (l->s[x] & 0xC0) == 0x80)
        x++;
    return x;
}

static int prev_pos(const Line *l, int x)
{
    if (x <= 0)
        return 0;
    x--;
    while (x > 0 && (l->s[x] & 0xC0) == 0x80)
        x--;
    return x;
}

static int col_of(const Line *l, int x) /* sichtbare Spalte vor Byte x */
{
    int c = 0;
    for (int i = 0; i < x && i < l->len; i++) {
        unsigned char b = (unsigned char)l->s[i];
        if (b == '\t')
            c = (c / TABW + 1) * TABW;
        else if ((b & 0xC0) != 0x80)
            c++;
    }
    return c;
}

static int pos_of_col(const Line *l, int col) /* Byte, dessen Spalte col am naechsten kommt */
{
    int x = 0;
    while (x < l->len) {
        int nx = next_pos(l, x);
        if (col_of(l, nx) > col) {
            int a = col_of(l, x), b = col_of(l, nx);
            return col - a > b - col ? nx : x;
        }
        x = nx;
    }
    return l->len;
}

static int is_word(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Markierung
 * ------------------------------------------------------------------------------------------------------------------- */

static int before(int y1, int x1, int y2, int x2) { return y1 < y2 || (y1 == y2 && x1 < x2); }

static void sel_range(int *y0, int *x0, int *y1, int *x1)
{
    if (before(ay, ax, cy, cx)) {
        *y0 = ay; *x0 = ax; *y1 = cy; *x1 = cx;
    } else {
        *y0 = cy; *x0 = cx; *y1 = ay; *x1 = ax;
    }
}

static int has_sel(void) { return sel && (ay != cy || ax != cx); }

static char *sel_text(u64 *n)
{
    int y0, x0, y1, x1;
    sel_range(&y0, &x0, &y1, &x1);
    u64 total = 1;
    for (int y = y0; y <= y1; y++)
        total += (u64)L[y].len + 1;
    char *t = u_malloc(total);
    u64 p = 0;
    for (int y = y0; y <= y1; y++) {
        int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : L[y].len;
        memcpy(t + p, L[y].s + a, (size_t)(b - a));
        p += (u64)(b - a);
        if (y < y1)
            t[p++] = '\n';
    }
    t[p] = 0;
    *n = p;
    return t;
}

static void delete_range(int y0, int x0, int y1, int x1)
{
    if (y0 == y1) {
        Line *l = &L[y0];
        memmove(l->s + x0, l->s + x1, (size_t)(l->len - x1));
        l->len -= x1 - x0;
    } else {
        Line *a = &L[y0], *b = &L[y1];
        int tail = b->len - x1;
        line_reserve(a, x0 + tail);
        memcpy(a->s + x0, b->s + x1, (size_t)tail);
        a->len = x0 + tail;
        for (int y = y1; y > y0; y--)
            remove_line(y);
    }
    cy = y0;
    cx = x0;
}

static void delete_sel(void)
{
    if (!has_sel()) {
        sel = 0;
        return;
    }
    int y0, x0, y1, x1;
    sel_range(&y0, &x0, &y1, &x1);
    delete_range(y0, x0, y1, x1);
    sel = 0;
}

/* Text an der Cursorstelle einfuegen (ersetzt die Markierung) */
static void insert_text(const char *t, u64 n)
{
    delete_sel();
    for (u64 i = 0; i < n; i++) {
        char c = t[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            insert_line(cy + 1);
            Line *a = &L[cy], *b = &L[cy + 1];
            int tail = a->len - cx;
            line_reserve(b, tail);
            memcpy(b->s, a->s + cx, (size_t)tail);
            b->len = tail;
            a->len = cx;
            cy++;
            cx = 0;
            continue;
        }
        Line *l = &L[cy];
        line_reserve(l, l->len + 1);
        memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx));
        l->s[cx++] = c;
        l->len++;
    }
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Rueckgaengig: Schnappschuesse des ganzen Textes (Tippen wird zusammengefasst)
 * ------------------------------------------------------------------------------------------------------------------- */

typedef struct {
    char *t;
    u64   n;
    int   cy, cx;
} Snap;

static Snap undo_s[MAXUNDO], redo_s[MAXUNDO];
static int  nundo, nredo, last_kind;
static s64  last_edit;

static Snap snap_now(void)
{
    Snap s;
    s.t = get_text(&s.n);
    s.cy = cy;
    s.cx = cx;
    return s;
}

static void snap_restore(Snap *s)
{
    set_text(s->t, s->n);
    cy = s->cy < nl ? s->cy : nl - 1;
    cx = s->cx <= L[cy].len ? s->cx : L[cy].len;
    sel = 0;
}

static void push(Snap *st, int *n, Snap s)
{
    if (*n == MAXUNDO) { /* aeltesten vergessen */
        u_free(st[0].t);
        memmove(st, st + 1, sizeof(Snap) * (MAXUNDO - 1));
        (*n)--;
    }
    st[(*n)++] = s;
}

/* vor einer Aenderung: kind 1 = Tippen (wird zusammengefasst), 2 = Loeschen (ebenso), 3 = sonst */
static void before_edit(int kind)
{
    s64 now = sys_time_us();
    if (!(kind == last_kind && kind != 3 && now - last_edit < 1500000)) {
        push(undo_s, &nundo, snap_now());
        while (nredo)
            u_free(redo_s[--nredo].t);
    }
    last_kind = kind;
    last_edit = now;
    if (!modified) {
        modified = 1;
        status[0] = 0;
    }
}

static u64 text_hash(void)
{
    u64 n, h = 1469598103934665603ULL; /* FNV-1a */
    char *t = get_text(&n);
    for (u64 i = 0; i < n; i++)
        h = (h ^ (unsigned char)t[i]) * 1099511628211ULL;
    u_free(t);
    return h;
}

static void undo(int redo)
{
    Snap *from = redo ? redo_s : undo_s, *to = redo ? undo_s : redo_s;
    int *nf = redo ? &nredo : &nundo, *nt = redo ? &nundo : &nredo;
    if (!*nf)
        return;
    push(to, nt, snap_now());
    Snap s = from[--*nf];
    snap_restore(&s);
    u_free(s.t);
    last_kind = 0;
    modified = text_hash() != saved_hash;
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Datei
 * ------------------------------------------------------------------------------------------------------------------- */

static void update_title(void)
{
    const char *base = path[0] ? strrchr(path, '/') : 0;
    char t[80];
    snprintf(t, sizeof(t), "%s%s", path[0] ? (base ? base + 1 : path) : "Unbenannt", modified ? " \xE2\x80\x94 bearbeitet" : "");
    gfx_set_title(t);
}

static int load(const char *p)
{
    Stat st;
    if (sys_stat(p, &st) != 0 || st.is_dir)
        return -1;
    if (st.size > (8u << 20)) {
        snprintf(status, sizeof(status), "Datei ist zu gro\xC3\x9F (h\xC3\xB6" "chstens 8 MB)");
        return -1;
    }
    char *t = u_malloc(st.size + 1);
    s64 fd = sys_open(p, O_RDONLY);
    u64 got = 0;
    s64 r;
    while (fd >= 0 && got < st.size && (r = sys_read((int)fd, t + got, st.size - got)) > 0)
        got += (u64)r;
    if (fd >= 0)
        sys_close((int)fd);
    for (u64 i = 0; i < got && i < 4096; i++)
        if (!t[i]) {
            u_free(t);
            snprintf(status, sizeof(status), "Keine Textdatei (enth\xC3\xA4lt Bin\xC3\xA4rdaten)");
            return -1;
        }
    set_text(t, got);
    u_free(t);
    snprintf(path, sizeof(path), "%s", p);
    cy = cx = top = left = 0;
    sel = modified = 0;
    saved_hash = text_hash();
    return 0;
}

static int save_as(const char *p)
{
    u64 n;
    char *t = get_text(&n);
    s64 fd = sys_open(p, O_WRONLY | O_CREAT | O_TRUNC);
    int rc = fd < 0 ? (int)fd : write_all((int)fd, t, n);
    if (fd >= 0)
        sys_close((int)fd);
    u_free(t);
    if (rc != 0) {
        if (strncmp(p, "/disk/", 6) != 0)
            snprintf(status, sizeof(status), "Hier kann nicht gespeichert werden (nur auf /disk)");
        else
            snprintf(status, sizeof(status), "Speichern fehlgeschlagen (Fehler %d) - auf /disk gehen nur kurze Namen wie NOTIZ.TXT", rc);
        return -1;
    }
    if (p != path)
        snprintf(path, sizeof(path), "%s", p);
    modified = 0;
    saved_hash = text_hash();
    last_kind = 0; /* naechstes Tippen ist ein neuer Schritt */
    snprintf(status, sizeof(status), "Gesichert: %s", path);
    return 0;
}

static void open_prompt(int kind)
{
    prompt = kind;
    if (kind == P_SAVEAS)
        snprintf(field, sizeof(field), "%s", path[0] && strncmp(path, "/disk/", 6) == 0 ? path : "/disk/NOTIZ.TXT");
    else if (kind == P_OPEN)
        snprintf(field, sizeof(field), "/disk/");
    flen = (int)strlen(field);
}

static void save(void)
{
    if (!path[0] || strncmp(path, "/disk/", 6) != 0) /* neu oder nur lesbar: wohin? */
        open_prompt(P_SAVEAS);
    else
        save_as(path);
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Aussehen
 * ------------------------------------------------------------------------------------------------------------------- */

static int bar_h(void) { return U(38); }
static int status_h(void) { return U(24); }
static int gutter(void)
{
    int d = 1;
    for (int n = nl; n >= 10; n /= 10)
        d++;
    if (d < 3)
        d = 3;
    return d * CELL_W + U(20);
}
static int text_x(void) { return gutter() + U(8); }
static int text_y(void) { return bar_h() + U(6); }
static int rows(void) { return (gfx_screen.h - text_y() - status_h()) / CELL_H; }
static int cols(void) { return (gfx_screen.w - text_x() - U(14)) / CELL_W; }

static const char *const buttons[4] = {"Neu", "\xC3\x96" "ffnen \xE2\x80\xA6", "Sichern", "Sichern unter \xE2\x80\xA6"};
static int hover_btn = -1, prompt_hover = -1;

static void button_rect(int i, int *x, int *y, int *w, int *h)
{
    int bx = U(10);
    for (int k = 0; k < i; k++)
        bx += text_width(font_ui, FS, buttons[k]) + U(22) + U(6);
    *x = bx;
    *y = U(6);
    *w = text_width(font_ui, FS, buttons[i]) + U(22);
    *h = bar_h() - U(12);
}

static void keep_cursor_visible(void)
{
    int r = rows(), c = cols(), col = col_of(&L[cy], cx);
    if (cy < top) top = cy;
    if (cy >= top + r) top = cy - r + 1;
    if (top > nl - 1) top = nl - 1;
    if (top < 0) top = 0;
    if (col < left) left = col;
    if (col >= left + c) left = col - c + 1;
    if (left < 0) left = 0;
}

/* Rueckfrage/Eingabefeld: Kasten und Knoepfe */
static void prompt_box(int *x, int *y, int *w, int *h)
{
    *w = U(460);
    *h = prompt == P_CLOSE ? U(150) : U(168);
    if (*w > gfx_screen.w - U(20))
        *w = gfx_screen.w - U(20);
    *x = (gfx_screen.w - *w) / 2;
    *y = bar_h() + U(30);
}

static int prompt_nbtn(void) { return prompt == P_CLOSE ? 3 : 2; }

static const char *prompt_btn(int i)
{
    if (prompt == P_CLOSE)
        return i == 0 ? "Nicht sichern" : i == 1 ? "Abbrechen" : "Sichern";
    return i == 0 ? "Abbrechen" : prompt == P_OPEN ? "\xC3\x96" "ffnen" : "Sichern";
}

static void prompt_btn_rect(int i, int *bx, int *by, int *bw, int *bh)
{
    int x, y, w, h;
    prompt_box(&x, &y, &w, &h);
    int n = prompt_nbtn(), pad = U(16);
    *bw = U(120);
    *bh = U(30);
    *by = y + h - pad - *bh;
    *bx = x + w - pad - (n - i) * *bw - (n - 1 - i) * U(10);
}

static void draw_prompt(Surface *s)
{
    int x, y, w, h;
    prompt_box(&x, &y, &w, &h);
    gfx_blend_fill(s, 0, 0, s->w, s->h, 0x000000, 40);
    gfx_shadow(s, x, y + U(4), w, h, U(12), U(24), 80);
    gfx_round_rect(s, x, y, w, h, U(12), 0xF6F6F8, 255);
    gfx_round_frame(s, x, y, w, h, U(12), 0x000000, 30);
    int tx = x + U(20), ty = y + U(16);
    if (prompt == P_CLOSE) {
        const char *base = path[0] ? strrchr(path, '/') : 0;
        char t[200];
        snprintf(t, sizeof(t), "\xC3\x84nderungen an \xE2\x80\x9E%s\xE2\x80\x9C sichern?", path[0] ? (base ? base + 1 : path) : "Unbenannt");
        text_draw(s, font_bold, U(15), tx, ty, t, C_TEXT);
        ty += text_height(font_bold, U(15)) + U(6);
        text_draw(s, font_ui, FS, tx, ty, "Ohne Sichern gehen die \xC3\x84nderungen verloren.", C_TEXT2);
    } else {
        text_draw(s, font_bold, U(15), tx, ty, prompt == P_OPEN ? "Datei \xC3\xB6" "ffnen" : "Sichern unter", C_TEXT);
        ty += text_height(font_bold, U(15)) + U(8);
        int fh = U(28), fw = w - U(40);
        gfx_round_rect(s, tx, ty, fw, fh, U(6), 0xFFFFFF, 255);
        gfx_round_frame(s, tx, ty, fw, fh, U(6), C_ACCENT, 200);
        int fy = ty + (fh - text_height(font_ui, FS)) / 2;
        gfx_set_clip(tx + U(4), ty, fw - U(8), fh);
        int tw = text_width(font_ui, FS, field), sx = tx + U(10);
        if (tw > fw - U(24)) /* langer Pfad: das Ende zeigen */
            sx -= tw - (fw - U(24));
        text_draw(s, font_ui, FS, sx, fy, field, C_TEXT);
        gfx_fill(s, sx + tw + 1, fy, U(2) > 1 ? U(2) : 2, text_height(font_ui, FS), C_ACCENT);
        gfx_no_clip();
        ty += fh + U(6);
        text_draw(s, font_ui, FS_SMALL, tx, ty, "Auf /disk gehen nur kurze Namen (8.3), z.B. /disk/NOTIZ.TXT", C_TEXT2);
    }
    for (int i = 0; i < prompt_nbtn(); i++) {
        int bx, by, bw, bh;
        prompt_btn_rect(i, &bx, &by, &bw, &bh);
        int main_btn = i == prompt_nbtn() - 1;
        u32 bg = main_btn ? C_ACCENT : 0xE3E3E8, fg = main_btn ? 0xFFFFFF : C_TEXT;
        if (i == prompt_hover)
            bg = gfx_mix(bg, 0x000000, 30);
        gfx_round_rect(s, bx, by, bw, bh, U(7), bg, 255);
        const char *t = prompt_btn(i);
        text_draw(s, font_bold, FS, bx + (bw - text_width(font_bold, FS, t)) / 2, by + (bh - text_height(font_bold, FS)) / 2, t, fg);
    }
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    int W = s->w, H = s->h, g = gutter(), tx = text_x(), ty = text_y(), r = rows();
    gfx_fill(s, 0, 0, W, H, C_WINDOW);

    /* Werkzeugleiste */
    gfx_gradient(s, 0, 0, W, bar_h(), 0xF7F7F8, 0xEDEDF0);
    gfx_fill(s, 0, bar_h() - 1, W, 1, 0xDDDDE0);
    for (int i = 0; i < 4; i++) {
        int bx, by, bw, bh;
        button_rect(i, &bx, &by, &bw, &bh);
        if (bx + bw > W - U(6))
            break;
        gfx_round_rect(s, bx, by, bw, bh, U(6), i == hover_btn ? 0xDCDCE2 : 0xFFFFFF, 255);
        gfx_round_frame(s, bx, by, bw, bh, U(6), 0x000000, 28);
        text_draw(s, font_ui, FS, bx + U(11), by + (bh - text_height(font_ui, FS)) / 2, buttons[i], C_TEXT);
    }

    /* Zeilennummern */
    gfx_fill(s, 0, bar_h(), g, H - bar_h() - status_h(), 0xF7F7F9);
    gfx_fill(s, g, bar_h(), 1, H - bar_h() - status_h(), 0xE6E6EA);

    int y0 = 0, x0 = 0, y1 = 0, x1 = 0, hs = has_sel();
    if (hs)
        sel_range(&y0, &x0, &y1, &x1);
    u32 selc = focus ? 0xB4D5FE : 0xDCDCE0;
    gfx_set_clip(0, bar_h(), W, H - bar_h() - status_h());
    for (int i = 0; i < r + 1 && top + i < nl; i++) {
        int y = top + i, ry = ty + i * CELL_H;
        Line *l = &L[y];
        if (y == cy && !hs) /* aktuelle Zeile */
            gfx_fill(s, g + 1, ry, W - g - 1, CELL_H, 0xF3F7FE);
        char num[16];
        snprintf(num, sizeof(num), "%d", y + 1);
        text_draw(s, font_mono, FS_MONO, g - U(10) - text_width(font_mono, FS_MONO, num), ry, num, y == cy ? 0x6E6E73 : 0xB0B0B5);
        if (hs && y >= y0 && y <= y1) { /* Markierung */
            int a = y == y0 ? col_of(l, x0) : 0, b = y == y1 ? col_of(l, x1) : col_of(l, l->len) + 1;
            int sx = tx + (a - left) * CELL_W, ex = tx + (b - left) * CELL_W;
            if (sx < g + 1) sx = g + 1;
            if (ex > sx)
                gfx_fill(s, sx, ry, ex - sx, CELL_H, selc);
        }
        gfx_set_clip(g + 1, bar_h(), W - g - 1, H - bar_h() - status_h());
        int col = 0;
        for (int x = 0; x < l->len;) {
            unsigned char b = (unsigned char)l->s[x];
            if (b == '\t') {
                col = (col / TABW + 1) * TABW;
                x++;
                continue;
            }
            const char *p = l->s + x;
            int nx = next_pos(l, x);
            unsigned cp = b;
            if (b >= 0x80) { /* UTF-8 dekodieren */
                int n = nx - x;
                cp = n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
                for (int k = 1; k < n; k++)
                    cp = (cp << 6) | ((unsigned char)p[k] & 0x3F);
            }
            if (col >= left && col < left + cols() + 1 && cp != ' ')
                text_glyph(s, font_mono, FS_MONO, tx + (col - left) * CELL_W, ry, cp, C_TEXT);
            col++;
            x = nx;
        }
        gfx_set_clip(0, bar_h(), W, H - bar_h() - status_h());
    }
    /* Cursor (blinkt, nur im aktiven Fenster) */
    if (focus && !prompt && ((sys_time_us() - blink_t0) / 530000) % 2 == 0 && cy >= top && cy < top + r + 1) {
        int c = col_of(&L[cy], cx) - left;
        if (c >= 0)
            gfx_fill(s, tx + c * CELL_W - 1, ty + (cy - top) * CELL_H, 2, CELL_H, C_ACCENT);
    }
    gfx_no_clip();
    ui_scrollbar(s, W, bar_h(), H - bar_h() - status_h(), nl, r, top);

    /* Statuszeile */
    int sy = H - status_h();
    gfx_fill(s, 0, sy, W, status_h(), 0xF7F7F8);
    gfx_fill(s, 0, sy, W, 1, 0xE0E0E4);
    char t[220];
    if (status[0])
        snprintf(t, sizeof(t), "%s", status);
    else
        snprintf(t, sizeof(t), "Zeile %d, Spalte %d  \xC2\xB7  %d Zeilen%s", cy + 1, col_of(&L[cy], cx) + 1, nl,
                 modified ? "  \xC2\xB7  ge\xC3\xA4ndert" : "");
    text_draw(s, font_ui, FS_SMALL, U(12), sy + (status_h() - text_height(font_ui, FS_SMALL)) / 2, t, C_TEXT2);

    if (prompt)
        draw_prompt(s);
    gfx_present_all();
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Eingaben
 * ------------------------------------------------------------------------------------------------------------------- */

static void move_to(int y, int x, int extend)
{
    if (extend && !sel) {
        sel = 1;
        ay = cy;
        ax = cx;
    } else if (!extend) {
        sel = 0;
    }
    cy = y;
    cx = x;
}

static void copy_sel(void)
{
    if (!has_sel())
        return;
    u64 n;
    char *t = sel_text(&n);
    sys_clipboard_set(t, n);
    u_free(t);
}

static void paste(void)
{
    s64 n = sys_clipboard_get(0, 0);
    if (n <= 0)
        return;
    char *t = u_malloc((u64)n + 1);
    n = sys_clipboard_get(t, (u64)n);
    before_edit(3);
    insert_text(t, (u64)n);
    u_free(t);
}

static void select_word(void)
{
    Line *l = &L[cy];
    int a = cx, b = cx;
    while (a > 0 && is_word((unsigned char)l->s[a - 1]))
        a--;
    while (b < l->len && is_word((unsigned char)l->s[b]))
        b++;
    while (a > 0 && (l->s[a] & 0xC0) == 0x80) /* nicht mitten in ein Zeichen */
        a--;
    sel = 1;
    ay = cy;
    ax = a;
    cx = b;
}

static void word_left(int *y, int *x)
{
    if (*x == 0) {
        if (*y > 0) {
            (*y)--;
            *x = L[*y].len;
        }
        return;
    }
    Line *l = &L[*y];
    while (*x > 0 && !is_word((unsigned char)l->s[*x - 1]))
        *x = prev_pos(l, *x);
    while (*x > 0 && is_word((unsigned char)l->s[*x - 1]))
        *x = prev_pos(l, *x);
}

static void word_right(int *y, int *x)
{
    Line *l = &L[*y];
    if (*x >= l->len) {
        if (*y + 1 < nl) {
            (*y)++;
            *x = 0;
        }
        return;
    }
    while (*x < l->len && !is_word((unsigned char)l->s[*x]))
        *x = next_pos(l, *x);
    while (*x < l->len && is_word((unsigned char)l->s[*x]))
        *x = next_pos(l, *x);
}

static void key(int k)
{
    int base = KEY_BASE(k), shift = (k & KEY_MOD_SHIFT) != 0, ctrl = (k & KEY_MOD_CTRL) != 0;
    if (k & KEY_MOD_ALT)
        return;
    int keep_col = 0, y = cy, x = cx;
    if (base >= KEY_UP && base <= KEY_PGDN && base != KEY_DEL) {
        switch (base) {
        case KEY_LEFT:
            if (has_sel() && !shift) {
                int y0, x0, y1, x1;
                sel_range(&y0, &x0, &y1, &x1);
                y = y0;
                x = x0;
            } else if (ctrl) {
                word_left(&y, &x);
            } else if (x > 0) {
                x = prev_pos(&L[y], x);
            } else if (y > 0) {
                y--;
                x = L[y].len;
            }
            break;
        case KEY_RIGHT:
            if (has_sel() && !shift) {
                int y0, x0, y1, x1;
                sel_range(&y0, &x0, &y1, &x1);
                y = y1;
                x = x1;
            } else if (ctrl) {
                word_right(&y, &x);
            } else if (x < L[y].len) {
                x = next_pos(&L[y], x);
            } else if (y + 1 < nl) {
                y++;
                x = 0;
            }
            break;
        case KEY_UP:
        case KEY_DOWN:
        case KEY_PGUP:
        case KEY_PGDN: {
            int step = base == KEY_UP ? -1 : base == KEY_DOWN ? 1 : base == KEY_PGUP ? -rows() : rows();
            if (want_col < 0)
                want_col = col_of(&L[y], x);
            y += step;
            if (y < 0) { y = 0; x = 0; want_col = 0; }
            else if (y >= nl) { y = nl - 1; x = L[y].len; want_col = col_of(&L[y], x); }
            else x = pos_of_col(&L[y], want_col);
            if (base == KEY_PGUP || base == KEY_PGDN)
                top += step;
            keep_col = 1;
            break;
        }
        case KEY_HOME:
            if (ctrl) {
                y = 0;
                x = 0;
            } else { /* erst an den Textanfang (nach der Einrueckung), dann an den Zeilenanfang */
                int ind = 0;
                while (ind < L[y].len && (L[y].s[ind] == ' ' || L[y].s[ind] == '\t'))
                    ind++;
                x = x == ind ? 0 : ind;
            }
            break;
        case KEY_END:
            if (ctrl)
                y = nl - 1;
            x = L[y].len;
            break;
        }
        move_to(y, x, shift);
        if (!keep_col)
            want_col = -1;
        return;
    }
    want_col = -1;
    if (ctrl)
        return;
    switch (k) {
    case 1: /* Strg+A */
        sel = 1;
        ay = 0;
        ax = 0;
        cy = nl - 1;
        cx = L[cy].len;
        return;
    case 3: copy_sel(); return;                                         /* Strg+C */
    case 24: if (has_sel()) { copy_sel(); before_edit(3); delete_sel(); } return; /* Strg+X */
    case 22: paste(); return;                                           /* Strg+V */
    case 26: undo(0); return;                                           /* Strg+Z */
    case 25: undo(1); return;                                           /* Strg+Y */
    case 19: save(); return;                                            /* Strg+S */
    case '\b':
    case 0x7F:
        if (has_sel()) {
            before_edit(2);
            delete_sel();
        } else if (cx > 0 || cy > 0) {
            before_edit(2);
            if (cx > 0)
                delete_range(cy, prev_pos(&L[cy], cx), cy, cx);
            else
                delete_range(cy - 1, L[cy - 1].len, cy, 0);
        }
        return;
    case KEY_DEL:
        if (has_sel()) {
            before_edit(2);
            delete_sel();
        } else if (cx < L[cy].len || cy + 1 < nl) {
            before_edit(2);
            if (cx < L[cy].len)
                delete_range(cy, cx, cy, next_pos(&L[cy], cx));
            else
                delete_range(cy, cx, cy + 1, 0);
        }
        return;
    case '\n': { /* neue Zeile mit derselben Einrueckung */
        before_edit(3);
        char ind[128];
        int n = 0;
        while (n < (int)sizeof(ind) - 1 && n < L[cy].len && (L[cy].s[n] == ' ' || L[cy].s[n] == '\t') && n < cx)
            ind[n] = L[cy].s[n], n++;
        insert_text("\n", 1);
        insert_text(ind, (u64)n);
        return;
    }
    }
    if (k == '\t' || (k >= 32 && k < 0xF5 && k != 0x7F)) {
        before_edit(1);
        char c = (char)k;
        insert_text(&c, 1);
    }
}

/* Mausposition -> Zeile und Byte */
static void hit(int px, int py, int *y, int *x)
{
    int r = (py - text_y()) / CELL_H;
    if (py < text_y())
        r = -1;
    *y = top + r;
    if (*y < 0) *y = 0;
    if (*y >= nl) *y = nl - 1;
    int col = (px - text_x() + CELL_W / 2) / CELL_W + left;
    if (col < 0)
        col = 0;
    *x = pos_of_col(&L[*y], col);
}

static void prompt_ok(void)
{
    int kind = prompt;
    field[flen] = 0;
    prompt = P_NONE;
    if (kind == P_OPEN) {
        Stat st;
        if (sys_stat(field, &st) != 0)
            snprintf(status, sizeof(status), "Nicht gefunden: %s", field);
        else if (gfx_desktop_open(field) != 0 && !st.is_dir && !modified)
            load(field); /* ohne Desktop: hier oeffnen */
    } else if (kind == P_SAVEAS) {
        if (save_as(field) == 0 && close_after_save) {
            gfx_close();
            sys_exit(0);
        }
    }
    close_after_save = 0;
}

static void prompt_click(int i)
{
    if (prompt == P_CLOSE) {
        if (i == 0) { /* nicht sichern */
            gfx_close();
            sys_exit(0);
        }
        prompt = P_NONE;
        if (i == 2) { /* sichern */
            close_after_save = 1;
            if (path[0] && strncmp(path, "/disk/", 6) == 0) {
                if (save_as(path) == 0) {
                    gfx_close();
                    sys_exit(0);
                }
                close_after_save = 0;
            } else {
                open_prompt(P_SAVEAS);
            }
        }
        return;
    }
    if (i == 0)
        prompt = P_NONE;
    else
        prompt_ok();
}

static void prompt_key(int k)
{
    if (k == 0x1B) {
        prompt = P_NONE;
        close_after_save = 0;
    } else if (k == '\n') {
        if (prompt == P_CLOSE)
            prompt_click(2);
        else
            prompt_ok();
    } else if (prompt != P_CLOSE && (k == '\b' || k == 0x7F)) {
        while (flen > 0 && (field[flen - 1] & 0xC0) == 0x80)
            flen--;
        if (flen > 0)
            flen--;
        field[flen] = 0;
    } else if (prompt != P_CLOSE && k >= 32 && k < 0xF5 && flen < (int)sizeof(field) - 1) {
        field[flen++] = (char)k;
        field[flen] = 0;
    }
}

static int prompt_btn_at(int px, int py)
{
    for (int i = 0; i < prompt_nbtn(); i++) {
        int bx, by, bw, bh;
        prompt_btn_rect(i, &bx, &by, &bw, &bh);
        if (px >= bx && px < bx + bw && py >= by && py < by + bh)
            return i;
    }
    return -1;
}

static int toolbar_btn_at(int px, int py)
{
    for (int i = 0; i < 4; i++) {
        int bx, by, bw, bh;
        button_rect(i, &bx, &by, &bw, &bh);
        if (px >= bx && px < bx + bw && py >= by && py < by + bh)
            return i;
    }
    return -1;
}

void _start(int argc, char **argv)
{
    ui_setup(0);
    clear_doc();
    saved_hash = text_hash();
    if (argc > 1 && load(argv[1]) != 0 && !status[0])
        snprintf(path, sizeof(path), "%s", argv[1]); /* gibt es noch nicht: wird beim Sichern angelegt */
    if (gfx_open_window_ex(90 * CELL_W + U(80), U(560), "Texteditor", GFX_RESIZABLE) != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    update_title();
    blink_t0 = sys_time_us();
    draw();
    int dragging = 0, last_mod = -1, clicks = 0, shown_blink = 0;
    s64 last_click = 0;
    for (;;) {
        Event e;
        int got = gfx_wait(&e, 50), changed = 0;
        if (got) {
            changed = 1;
            if (e.type == EV_CLOSE) {
                if (!modified) {
                    gfx_close();
                    sys_exit(0);
                }
                prompt = P_CLOSE; /* erst fragen */
                prompt_hover = -1;
            } else if (e.type == EV_FOCUS) {
                focus = e.key;
            } else if (e.type == EV_KEY) {
                if (prompt) {
                    prompt_key(e.key);
                } else {
                    key(e.key);
                    keep_cursor_visible();
                    blink_t0 = sys_time_us();
                }
            } else if (e.type == EV_DOWN && e.button == 1) {
                if (prompt) {
                    int b = prompt_btn_at(e.x, e.y);
                    if (b >= 0)
                        prompt_click(b);
                } else if (e.y < bar_h()) {
                    int b = toolbar_btn_at(e.x, e.y);
                    if (b == 0 && gfx_desktop_open("") != 0) { /* Neu: neues Fenster (ohne Desktop: leeren) */
                        if (!modified) {
                            clear_doc();
                            path[0] = 0;
                        }
                    } else if (b == 1) {
                        open_prompt(P_OPEN);
                    } else if (b == 2) {
                        save();
                    } else if (b == 3) {
                        open_prompt(P_SAVEAS);
                    }
                } else if (e.y < gfx_screen.h - status_h()) {
                    int y, x;
                    hit(e.x, e.y, &y, &x);
                    s64 now = sys_ticks();
                    clicks = now - last_click < 40 && y == cy ? clicks + 1 : 1;
                    last_click = now;
                    move_to(y, x, 0);
                    if (clicks == 2) {
                        select_word();
                    } else if (clicks >= 3) { /* ganze Zeile */
                        sel = 1;
                        ay = cy;
                        ax = 0;
                        cx = L[cy].len;
                    }
                    want_col = -1;
                    dragging = clicks == 1;
                    blink_t0 = sys_time_us();
                }
            } else if (e.type == EV_MOVE) {
                if (dragging) {
                    int y, x;
                    hit(e.x, e.y, &y, &x);
                    if (!sel) {
                        sel = 1;
                        ay = cy;
                        ax = cx;
                    }
                    cy = y;
                    cx = x;
                    if (e.y < text_y() && top > 0) /* ueber den Rand: mitscrollen */
                        top--;
                    else if (e.y > gfx_screen.h - status_h() && top + rows() < nl)
                        top++;
                    keep_cursor_visible();
                } else {
                    int hb = prompt ? -1 : e.y < bar_h() ? toolbar_btn_at(e.x, e.y) : -1;
                    int ph = prompt ? prompt_btn_at(e.x, e.y) : -1;
                    changed = hb != hover_btn || ph != prompt_hover;
                    hover_btn = hb;
                    prompt_hover = ph;
                }
            } else if (e.type == EV_UP) {
                dragging = 0;
            } else if (e.type == EV_WHEEL && !prompt) {
                top -= e.wheel * 3;
                if (top > nl - rows()) top = nl - rows();
                if (top < 0) top = 0;
            }
        }
        if (modified != last_mod) {
            last_mod = modified;
            update_title();
        }
        int b = (int)(((sys_time_us() - blink_t0) / 530000) % 2);
        if (b != shown_blink) { /* Cursor blinkt */
            shown_blink = b;
            changed = 1;
        }
        if (changed)
            draw();
    }
}
