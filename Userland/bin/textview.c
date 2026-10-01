#include "gfx.h"
#include "malloc.h"
#include "ui.h"

/* textview datei: Text ansehen (bis 4 MB). Pfeile, Bild hoch/runter, Pos1/Ende, Leertaste und Mausrad blaettern.
 * Unter dem Desktop im Fenster (Groesse aenderbar), sonst im Vollbild (Esc/q beendet). */

static char  *text;
static char **lines;
static int    nlines, top;

static void load(const char *path)
{
    Stat st;
    u64 size = 0;
    if (sys_stat(path, &st) != 0 || st.is_dir || st.size > (4u << 20)) {
        text = u_malloc(64);
        snprintf(text, 64, "(nicht lesbar oder gr\xC3\xB6\xC3\x9F" "er als 4 MB)");
        size = strlen(text);
    } else {
        text = u_malloc(st.size + 1);
        s64 fd = sys_open(path, O_RDONLY);
        s64 r;
        while (fd >= 0 && size < st.size && (r = sys_read((int)fd, text + size, st.size - size)) > 0)
            size += (u64)r;
        if (fd >= 0)
            sys_close((int)fd);
        text[size] = 0;
        int binary = 0;
        for (u64 i = 0; i < size && i < 4096; i++)
            if (text[i] == 0)
                binary = 1;
        if (binary) { /* Programme und andere Binaerdateien nicht als Text zeigen */
            int is_prog = size >= 4 && text[0] == 0x7F && text[1] == 'E' && text[2] == 'L' && text[3] == 'F';
            char *msg = u_malloc(512);
            snprintf(msg, 512, "%s (%llu Bytes) - kein Text.\n\n%s", is_prog ? "Programm" : "Bin\xC3\xA4rdatei",
                     (unsigned long long)size,
                     is_prog ? "Programme startet man im Terminal, z.B. mit ihrem Namen." : "Ansehen im Terminal mit: hexdump datei");
            u_free(text);
            text = msg;
            size = strlen(msg);
        }
    }
    int n = 1;
    for (u64 i = 0; i < size; i++)
        if (text[i] == '\n')
            n++;
    lines = u_malloc(sizeof(char *) * (u64)n);
    char *p = text;
    for (u64 i = 0; i <= size; i++) {
        if (i == size || text[i] == '\n') {
            text[i] = 0;
            if (i > 0 && text[i - 1] == '\r')
                text[i - 1] = 0;
            lines[nlines++] = p;
            p = text + i + 1;
        }
    }
}

static int visible(void) { return (gfx_screen.h - U(16)) / CELL_H; }

static void clamp(void)
{
    if (top > nlines - visible())
        top = nlines - visible();
    if (top < 0)
        top = 0;
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    gfx_fill(s, 0, 0, s->w, s->h, C_WINDOW);
    int vis = visible();
    for (int i = 0; i < vis && top + i < nlines; i++)
        text_draw(s, font_mono, FS_MONO, U(12), U(8) + i * CELL_H, lines[top + i], C_TEXT);
    ui_scrollbar(s, s->w, 0, s->h, nlines, vis, top);
    gfx_present_all();
}

void _start(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(2, "Aufruf: textview datei\n");
        sys_exit(2);
    }
    load(argv[1]);
    ui_setup(0);
    const char *base = strrchr(argv[1], '/');
    if (gfx_open_window_ex(82 * CELL_W + U(24), U(460), base ? base + 1 : argv[1], GFX_RESIZABLE) != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    draw();
    for (;;) {
        Event e;
        if (!gfx_wait(&e, -1))
            continue;
        int vis = visible(), old = top;
        if (e.type == EV_CLOSE)
            break;
        if (e.type == EV_KEY) {
            int k = e.key;
            if ((k == 0x1B || k == 'q') && !gfx_windowed())
                break;
            if (k == KEY_DOWN) top++;
            else if (k == KEY_UP) top--;
            else if (k == KEY_PGDN || k == ' ') top += vis;
            else if (k == KEY_PGUP) top -= vis;
            else if (k == KEY_HOME) top = 0;
            else if (k == KEY_END) top = nlines;
        } else if (e.type == EV_WHEEL) {
            top -= e.wheel * 3;
        } else if (e.type == EV_RESIZE) {
            clamp();
            draw();
            continue;
        }
        clamp();
        if (top != old)
            draw();
    }
    gfx_close();
    sys_exit(0);
}
