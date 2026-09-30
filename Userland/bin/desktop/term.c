/* Desktop: Terminalfenster (eine echte Shell ueber Pipes, ANSI-Farben) */

#include "desktop.h"

static void term_byte2(Win *w, unsigned char b);

static const u32 term_pal[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xC0C0C0,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

/* ======================================================================================================================
 * Terminal
 * ==================================================================================================================== */

void term_alloc(Win *w)
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

void open_terminal(void)
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

void term_poll(Win *w)
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
void term_flush(Win *w)
{
    if (w->tr0 > w->tr1)
        return;
    win_dirty(w, BORDER, TITLE_H + 2 + w->tr0 * 16, w->w - 2 * BORDER, (w->tr1 - w->tr0 + 1) * 16);
    w->tr0 = 1;
    w->tr1 = 0;
}

void draw_terminal(Win *w, int x, int y, int cw, int ch)
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

void term_key(Win *w, int k)
{
    if (w->to_child < 0)
        return;
    unsigned char b = (unsigned char)k;
    sys_write(w->to_child, &b, 1);
}
