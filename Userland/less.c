#include "libc.h"
#include "malloc.h"

/* less [datei]: zeigt eine Datei (oder stdin) seitenweise an.
 *   Leertaste/Bild runter = eine Seite vor, b/Bild hoch = zurueck, Enter/j/Pfeil runter = eine Zeile vor,
 *   k/Pfeil hoch = zurueck, g/Pos1 = Anfang, G/Ende = Ende, q = beenden.
 * Ist die Ausgabe kein Terminal (Pipe, Datei), verhaelt sich less wie cat. */
#define MAX_BYTES (4ULL << 20)

static char *read_all(int fd, u64 *len)
{
    u64 cap = 65536, n = 0;
    char *b = u_malloc(cap);
    while (b) {
        if (n == cap) {
            if (cap >= MAX_BYTES)
                break; /* zu gross: der Rest wird nicht angezeigt */
            char *nb = u_malloc(cap * 2);
            if (!nb)
                break;
            memcpy(nb, b, n);
            u_free(b);
            b = nb;
            cap *= 2;
        }
        s64 r = sys_read(fd, b + n, cap - n);
        if (r <= 0)
            break;
        n += (u64)r;
    }
    *len = n;
    return b;
}

static char *disp;   /* aufbereiteter Text: Tabs ausgeschrieben, Steuerzeichen ersetzt, bei Breite umbrochen */
static int  *row_start, *row_len, nrows;

/* Zerlegt den Text in Bildschirmzeilen der Breite w. Zeichen ausserhalb von ASCII 32..126 werden zu '.' bzw. '?'. */
static int build_rows(const char *text, u64 len, int w)
{
    disp = u_malloc(len * 8 + 16);
    row_start = u_malloc((len + 2) * sizeof(int));
    row_len = u_malloc((len + 2) * sizeof(int));
    if (!disp || !row_start || !row_len)
        return -1;
    int d = 0, col = 0;
    nrows = 0;
    row_start[0] = 0;
    for (u64 i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        int reps = 1;
        char out = (char)c;
        if (c == '\n') {
            row_len[nrows] = d - row_start[nrows];
            row_start[++nrows] = d;
            col = 0;
            continue;
        }
        if (c == '\r')
            continue;
        if (c >= 0xC2 && c <= 0xF4) { /* UTF-8: ein Zeichen aus mehreren Bytes belegt eine Spalte */
            int l = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4, ok = i + (u64)l <= len;
            for (int k = 1; ok && k < l; k++)
                ok = ((unsigned char)text[i + (u64)k] & 0xC0) == 0x80;
            if (ok) {
                if (col == w) {
                    row_len[nrows] = d - row_start[nrows];
                    row_start[++nrows] = d;
                    col = 0;
                }
                for (int k = 0; k < l; k++)
                    disp[d++] = text[i + (u64)k];
                col++;
                i += (u64)l - 1;
                continue;
            }
        }
        if (c == '\t') {
            reps = 8 - col % 8;
            out = ' ';
        } else if (c < 32) {
            out = '.';
        } else if (c >= 127) {
            out = '?';
        }
        while (reps--) {
            if (col == w) { /* Zeile voll: umbrechen */
                row_len[nrows] = d - row_start[nrows];
                row_start[++nrows] = d;
                col = 0;
            }
            disp[d++] = out;
            col++;
        }
    }
    if (d > row_start[nrows] || nrows == 0) { /* letzte Zeile ohne abschliessendes '\n' */
        row_len[nrows] = d - row_start[nrows];
        nrows++;
    }
    return 0;
}

static void draw(int top, int page, int cols, const char *name)
{
    write_all(1, "\x1b[H", 3);
    for (int r = 0; r < page; r++) {
        int i = top + r;
        if (i < nrows) {
            write_all(1, disp + row_start[i], (size_t)row_len[i]);
        } else {
            write_all(1, C_DIM "~" C_RESET, 1 + 4 + 4);
        }
        write_all(1, "\x1b[K\n", 4);
    }
    char status[160];
    int last = top + page < nrows ? top + page : nrows;
    int pct = nrows ? last * 100 / nrows : 100;
    int n = snprintf(status, sizeof(status), " %s  Zeilen %d-%d von %d (%d%%)   Leertaste/b = Seite, j/k = Zeile, g/G, q = Ende", name,
                     nrows ? top + 1 : 0, last, nrows, pct);
    if (n > cols - 1)
        n = cols - 1;
    write_all(1, "\x1b[7m", 4);
    write_all(1, status, (size_t)n);
    for (int k = n; k < cols - 1; k++)
        write_all(1, " ", 1);
    write_all(1, "\x1b[0m", 4);
}

void _start(int argc, char **argv)
{
    int fd = 0;
    const char *name = "(stdin)";
    if (argc > 1) {
        s64 f = sys_open(argv[1], O_RDONLY);
        if (f < 0) {
            fprintf(2, "less: '%s': nicht gefunden\n", argv[1]);
            sys_exit(1);
        }
        fd = (int)f;
        name = argv[1];
    }

    s64 t = sys_isatty(1);
    if (!t) { /* keine Anzeige: wie cat */
        char buf[512];
        s64 n;
        while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
            write_all(1, buf, (size_t)n);
        sys_exit(0);
    }
    int cols = (int)(t >> 16), rows = (int)(t & 0xFFFF);
    if (cols < 20)
        cols = 80;
    if (rows < 4)
        rows = 25;

    u64 len = 0;
    char *text = read_all(fd, &len);
    if (!text || build_rows(text, len, cols - 1) != 0) {
        fprintf(2, "less: kein Speicher\n");
        sys_exit(1);
    }

    int page = rows - 1, top = 0;
    int maxtop = nrows > page ? nrows - page : 0;
    write_all(1, "\x1b[2J", 4);
    draw(top, page, cols, name);
    for (;;) {
        s64 c = sys_getchar();
        if (c < 0) {
            sys_sleep_ms(10);
            continue;
        }
        int ntop = top;
        if (c == 'q' || c == 'Q' || c == 3)
            break;
        else if (c == ' ' || c == 'f' || c == KEY_PGDN)
            ntop = top + page;
        else if (c == 'b' || c == KEY_PGUP)
            ntop = top - page;
        else if (c == '\n' || c == 'j' || c == KEY_DOWN)
            ntop = top + 1;
        else if (c == 'k' || c == KEY_UP)
            ntop = top - 1;
        else if (c == 'g' || c == KEY_HOME)
            ntop = 0;
        else if (c == 'G' || c == KEY_END)
            ntop = maxtop;
        else
            continue;
        if (ntop > maxtop)
            ntop = maxtop;
        if (ntop < 0)
            ntop = 0;
        if (ntop != top) {
            top = ntop;
            draw(top, page, cols, name);
        }
    }
    write_all(1, "\x1b[2J\x1b[H", 7);
    sys_exit(0);
}
