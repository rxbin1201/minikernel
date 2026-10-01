#include "gfx.h"
#include "malloc.h"
#include "ui.h"

/* term: Terminal fuer den Desktop - eine echte Shell (/bin/sh) ueber Pipes, ANSI-Farben (ESC [ ... m), Cursor setzen
 * (H), Zeile/Bild loeschen (K, J), UTF-8. Fenstergroesse aenderbar: die Zeichenflaeche passt sich an.
 * Grafikprogramme, die man hier startet, bekommen ein eigenes Fenster (sie melden sich beim Desktop). Programme fuer
 * die Textkonsole mit eigenem Vollbild (edit, less) laufen hier nicht. */

typedef struct {
    unsigned short ch;
    unsigned char  fg, bg;
} Cell;

/* ANSI-Farben fuer dunklen Grund (Farbe 0 als Hintergrund = Grund des Terminals) */
static const u32 pal[16] = {
    0x3A3A3C, 0xFF5F57, 0x32D74B, 0xFFD60A, 0x409CFF, 0xDA8FFF, 0x5AC8FA, 0xE5E5EA,
    0x8E8E93, 0xFF6961, 0x30DB5B, 0xFFE55C, 0x70B8FF, 0xE8A8FF, 0x8CDBFF, 0xFFFFFF,
};

static Cell    *cells;
static int      cols, rows, cx, cy, fg = 7, bg, esc, escn, escp[8], u8need, focus = 1;
static unsigned u8cp;
static int      tr0 = 1, tr1 = 0; /* geaenderte Zeilen (tr0 > tr1: keine) */
static int      pid, to_sh, from_sh;

static int pad(void) { return U(8); }

static void touch(int r0, int r1)
{
    if (tr0 > tr1) {
        tr0 = r0;
        tr1 = r1;
        return;
    }
    if (r0 < tr0) tr0 = r0;
    if (r1 > tr1) tr1 = r1;
}

/* Zeichenflaeche an die Fenstergroesse anpassen; alter Inhalt bleibt (unten ausgerichtet) */
static void alloc_cells(void)
{
    int nc = (gfx_screen.w - 2 * pad()) / CELL_W, nr = (gfx_screen.h - 2 * pad()) / CELL_H;
    if (nc < 10) nc = 10;
    if (nr < 3) nr = 3;
    Cell *n = u_malloc(sizeof(Cell) * (u64)(nc * nr));
    if (!n)
        return;
    for (int i = 0; i < nc * nr; i++) {
        n[i].ch = ' ';
        n[i].fg = 7;
        n[i].bg = 0;
    }
    if (cells) {
        int shift = cy >= nr ? cy - nr + 1 : 0;
        for (int r = 0; r < nr && r + shift < rows; r++)
            for (int c = 0; c < nc && c < cols; c++)
                n[r * nc + c] = cells[(r + shift) * cols + c];
        cy -= shift;
        u_free(cells);
    }
    cells = n;
    cols = nc;
    rows = nr;
    if (cx >= cols) cx = cols - 1;
    if (cy >= rows) cy = rows - 1;
    touch(0, rows - 1);
}

static void scroll_up(void)
{
    touch(0, rows - 1);
    memmove(cells, cells + cols, sizeof(Cell) * (u64)(cols * (rows - 1)));
    for (int c = 0; c < cols; c++) {
        Cell *t = &cells[(rows - 1) * cols + c];
        t->ch = ' ';
        t->fg = 7;
        t->bg = 0;
    }
}

static void newline(void)
{
    cx = 0;
    if (++cy >= rows) {
        scroll_up();
        cy = rows - 1;
    }
}

static void put(unsigned cp)
{
    if (cx >= cols)
        newline();
    Cell *t = &cells[cy * cols + cx];
    t->ch = cp > 0xFFFF ? '?' : (unsigned short)cp;
    t->fg = (unsigned char)fg;
    t->bg = (unsigned char)bg;
    cx++;
}

static void csi(char f)
{
    int n = escn + 1;
    if (f == 'm') {
        for (int i = 0; i < n; i++) {
            int p = escp[i];
            if (p == 0) { fg = 7; bg = 0; }
            else if (p == 1 && fg < 8) fg += 8;
            else if (p == 7) { int t = fg; fg = bg; bg = t; }
            else if (p >= 30 && p <= 37) fg = p - 30;
            else if (p >= 90 && p <= 97) fg = p - 90 + 8;
            else if (p == 39) fg = 7;
            else if (p >= 40 && p <= 47) bg = p - 40;
            else if (p == 49) bg = 0;
        }
    } else if (f == 'K') {
        for (int c = cx; c < cols; c++) {
            Cell *t = &cells[cy * cols + c];
            t->ch = ' ';
            t->bg = (unsigned char)bg;
        }
    } else if (f == 'J') {
        touch(0, rows - 1);
        for (int i = 0; i < cols * rows; i++) {
            cells[i].ch = ' ';
            cells[i].bg = 0;
        }
        cx = cy = 0;
    } else if (f == 'H') {
        int r = escp[0] > 0 ? escp[0] - 1 : 0, c = n > 1 && escp[1] > 0 ? escp[1] - 1 : 0;
        cy = r < rows ? r : rows - 1;
        cx = c < cols ? c : cols - 1;
    }
}

static void byte2(unsigned char b)
{
    if (esc == 1) {
        if (b == '[') {
            esc = 2;
            escn = 0;
            memset(escp, 0, sizeof(escp));
        } else {
            esc = 0;
        }
        return;
    }
    if (esc == 2) {
        if (b >= '0' && b <= '9') {
            if (escp[escn] < 10000)
                escp[escn] = escp[escn] * 10 + (b - '0');
        } else if (b == ';') {
            if (escn < 7)
                escn++;
        } else {
            esc = 0;
            csi((char)b);
        }
        return;
    }
    if (u8need) {
        if ((b & 0xC0) == 0x80) {
            u8cp = (u8cp << 6) | (b & 0x3F);
            if (--u8need == 0)
                put(u8cp);
            return;
        }
        u8need = 0;
        put(0xFFFD);
    }
    if (b >= 0x80) {
        if ((b & 0xE0) == 0xC0) { u8cp = b & 0x1F; u8need = 1; }
        else if ((b & 0xF0) == 0xE0) { u8cp = b & 0x0F; u8need = 2; }
        else if ((b & 0xF8) == 0xF0) { u8cp = b & 0x07; u8need = 3; }
        else put(0xFFFD);
        return;
    }
    switch (b) {
    case 0x1B: esc = 1; break;
    case '\n': newline(); break;
    case '\r': cx = 0; break;
    case '\b': if (cx > 0) cx--; break;
    case '\t': cx = (cx + 8) & ~7; if (cx >= cols) newline(); break;
    case 7: break;
    default:
        if (b >= 32)
            put(b);
    }
}

/* Ein Byte der Ausgabe; die Zeile des Cursors vorher und nachher wird neu gezeichnet */
static void byte(unsigned char b)
{
    touch(cy, cy);
    byte2(b);
    touch(cy, cy);
}

static void draw_rows(int r0, int r1)
{
    Surface *s = &gfx_screen;
    int ox = pad(), oy = pad();
    int y0 = r0 == 0 ? 0 : oy + r0 * CELL_H, y1 = r1 == rows - 1 ? s->h : oy + (r1 + 1) * CELL_H;
    gfx_fill(s, 0, y0, s->w, y1 - y0, C_TERM_BG);
    for (int r = r0; r <= r1; r++) {
        int ry = oy + r * CELL_H;
        for (int c = 0; c < cols; c++) {
            Cell *t = &cells[r * cols + c];
            u32 fgc = pal[t->fg & 15];
            int x = ox + c * CELL_W;
            if (t->bg & 15)
                gfx_fill(s, x, ry, CELL_W, CELL_H, pal[t->bg & 15]);
            if (r == cy && c == cx) { /* Cursor: Block (aktiv) bzw. Rahmen */
                if (focus) {
                    gfx_fill(s, x, ry, CELL_W, CELL_H, 0xE5E5EA);
                    fgc = C_TERM_BG;
                } else {
                    gfx_rect(s, x, ry, CELL_W, CELL_H, 0x8E8E93);
                }
            }
            if (t->ch != ' ')
                text_glyph(s, font_mono, FS_MONO, x, ry, t->ch, fgc);
        }
    }
    gfx_present(0, y0, s->w, y1 - y0);
}

static void flush(void)
{
    if (tr0 > tr1)
        return;
    if (tr0 < 0) tr0 = 0;
    if (tr1 >= rows) tr1 = rows - 1;
    draw_rows(tr0, tr1);
    tr0 = 1;
    tr1 = 0;
}

static int start_shell(void)
{
    int in[2], out[2];
    if (sys_pipe(in) < 0 || sys_pipe(out) < 0)
        return -1;
    s64 p = sys_fork();
    if (p == 0) {
        sys_dup2(in[0], 0);
        sys_dup2(out[1], 1);
        sys_dup2(out[1], 2);
        for (int fd = 3; fd < 32; fd++) /* auch die Pipes zum Desktop nicht vererben */
            sys_close(fd);
        sys_exec("/bin/sh", "sh");
        sys_exit(127);
    }
    sys_close(in[0]);
    sys_close(out[1]);
    if (p < 0)
        return -1;
    pid = (int)p;
    to_sh = in[1];
    from_sh = out[0];
    return 0;
}

static void quit(void)
{
    sys_close(to_sh);
    sys_close(from_sh);
    sys_kill(pid);
    int code;
    sys_wait(pid, &code);
    gfx_close();
    sys_exit(0);
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ui_setup(0);
    if (gfx_open_window_ex(90 * CELL_W + 2 * pad(), 28 * CELL_H + 2 * pad(), "Terminal", GFX_RESIZABLE) != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    alloc_cells();
    if (start_shell() != 0) {
        gfx_close();
        sys_exit(1);
    }
    const char *hello = "\x1b[36mTerminal\x1b[0m \xE2\x80\x93 Grafikprogramme (snake, view, \xE2\x80\xA6) \xC3\xB6" "ffnen ein eigenes "
                        "Fenster; mit & dahinter l\xC3\xA4" "uft das Terminal weiter\r\n";
    for (const char *p = hello; *p; p++)
        byte((unsigned char)*p);
    for (;;) {
        int busy = 0;
        Event e;
        while (gfx_poll(&e)) {
            if (e.type == EV_CLOSE) {
                quit();
            } else if (e.type == EV_KEY) {
                unsigned char b = (unsigned char)e.key;
                sys_write(to_sh, &b, 1);
            } else if (e.type == EV_RESIZE) {
                alloc_cells();
            } else if (e.type == EV_FOCUS) {
                focus = e.key;
                touch(cy, cy);
            }
        }
        unsigned char buf[2048];
        for (int round = 0; round < 8; round++) {
            s64 n = sys_fdavail(from_sh);
            if (n < 0) /* Shell beendet (exit): Fenster zu */
                quit();
            if (n == 0)
                break;
            s64 r = sys_read(from_sh, buf, n < (s64)sizeof(buf) ? (u64)n : sizeof(buf));
            if (r <= 0)
                break;
            for (s64 i = 0; i < r; i++)
                byte(buf[i]);
            busy = 1;
        }
        if (tr0 <= tr1) {
            flush();
            busy = 1;
        }
        if (busy)
            gfx_vsync(); /* im Takt des Desktops */
        else
            sys_sleep_ms(10);
    }
}
