#include "gfx.h"
#include "malloc.h"
#include "ui.h"

/* files [ordner]: Dateien und Ordner ansehen. Doppelklick oder Enter oeffnet: Ordner im selben Fenster, Dateien mit dem
 * passenden Programm (das waehlt der Desktop: Bilder in der Bildansicht, sonst die Textansicht). Pfeile waehlen aus,
 * Ruecktaste geht einen Ordner hoeher. Laeuft nur sinnvoll unter dem Desktop (Fenster mit aenderbarer Groesse). */

typedef struct {
    char name[128];
    int  is_dir;
    u64  size;
} Ent;

static char dir[256] = "/";
static Ent *ents;
static int  nent, scroll, sel, focus = 1;
static char status[160];

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static void load(void)
{
    u_free(ents);
    ents = 0;
    nent = 0;
    int cap = 64;
    Ent *e = u_malloc(sizeof(Ent) * (u64)cap);
    if (!e)
        return;
    int n = 0;
    if (strcmp(dir, "/") != 0) {
        snprintf(e[0].name, sizeof(e[0].name), "..");
        e[0].is_dir = 1;
        e[0].size = 0;
        n = 1;
    }
    DirEnt de;
    for (u64 i = 0; sys_readdir(dir, i, &de) == 0; i++) {
        if (n == cap) {
            Ent *ne = u_malloc(sizeof(Ent) * (u64)cap * 2);
            if (!ne)
                break;
            memcpy(ne, e, sizeof(Ent) * (u64)n);
            u_free(e);
            e = ne;
            cap *= 2;
        }
        snprintf(e[n].name, sizeof(e[n].name), "%s", de.name);
        e[n].is_dir = (int)de.is_dir;
        e[n].size = de.size;
        n++;
    }
    int first = strcmp(dir, "/") != 0; /* ".." bleibt oben; sonst Ordner zuerst, alphabetisch */
    for (int i = first + 1; i < n; i++) {
        Ent x = e[i];
        int j = i - 1;
        while (j >= first && (e[j].is_dir < x.is_dir || (e[j].is_dir == x.is_dir && strcasecmp(e[j].name, x.name) > 0))) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = x;
    }
    ents = e;
    nent = n;
    scroll = 0;
    sel = 0;
    status[0] = 0;
    const char *base = strrchr(dir, '/');
    gfx_set_title(strcmp(dir, "/") == 0 ? "System" : base ? base + 1 : dir);
}

static int head(void) { return U(28); }
static int foot(void) { return U(24); }
static int visible(void) { return (gfx_screen.h - head() - foot()) / ROW_H; }

static void open_entry(int i)
{
    if (i < 0 || i >= nent)
        return;
    Ent *e = &ents[i];
    if (strcmp(e->name, "..") == 0) {
        char *s = strrchr(dir, '/');
        if (s && s != dir)
            *s = 0;
        else
            snprintf(dir, sizeof(dir), "/");
        load();
        return;
    }
    char path[400];
    snprintf(path, sizeof(path), "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/", e->name);
    if (e->is_dir) {
        if (strlen(path) < sizeof(dir)) {
            snprintf(dir, sizeof(dir), "%s", path);
            load();
        }
    } else if (gfx_desktop_open(path) == 0) {
        snprintf(status, sizeof(status), "%s wird ge\xC3\xB6" "ffnet", e->name);
    } else {
        snprintf(status, sizeof(status), "%s kann nicht ge\xC3\xB6" "ffnet werden", e->name);
    }
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    int cw = s->w, ch = s->h, hd = head();
    gfx_fill(s, 0, 0, cw, ch, C_WINDOW);
    gfx_fill(s, 0, 0, cw, hd, 0xFAFAFA);
    gfx_fill(s, 0, hd - 1, cw, 1, 0xE5E5E5);
    int ty = (hd - text_height(font_bold, FS_SMALL)) / 2;
    text_draw(s, font_bold, FS_SMALL, U(40), ty, "Name", C_TEXT2);
    text_draw(s, font_bold, FS_SMALL, cw - U(80), ty, "Gr\xC3\xB6\xC3\x9F" "e", C_TEXT2);
    int vis = visible();
    for (int i = 0; i < vis && scroll + i < nent; i++) {
        Ent *e = &ents[scroll + i];
        int ry = hd + i * ROW_H, selected = scroll + i == sel;
        if (selected)
            gfx_round_rect(s, U(6), ry + 1, cw - U(12), ROW_H - 2, U(5), focus ? C_ACCENT : 0xDCDCE0, 255);
        else if (i % 2)
            gfx_fill(s, 0, ry, cw, ROW_H, 0xF5F5F7);
        int isz = ROW_H - U(8), iy = ry + U(4);
        if (e->is_dir)
            ui_folder_icon(s, U(14), iy, isz);
        else
            ui_doc_icon(s, U(14), iy, isz, ends_with(e->name, ".bmp") ? 0x34C759 : ends_with(e->name, ".sh") ? 0x0A84FF :
                        ends_with(e->name, ".wav") || ends_with(e->name, ".mp3") ? 0xFF2D55 : 0);
        u32 tc = selected && focus ? 0xFFFFFF : C_TEXT;
        int tty = ry + (ROW_H - text_height(font_ui, FS)) / 2;
        gfx_set_clip(0, ry, cw - U(90), ROW_H); /* lange Namen nicht in die Groesse schreiben */
        text_draw(s, font_ui, FS, U(40), tty, e->name, tc);
        gfx_no_clip();
        if (!e->is_dir) {
            char sz[24];
            u64 b = e->size;
            if (b < 1024) snprintf(sz, sizeof(sz), "%llu Byte", (unsigned long long)b);
            else if (b < 1024 * 1024) snprintf(sz, sizeof(sz), "%llu KB", (unsigned long long)(b / 1024));
            else snprintf(sz, sizeof(sz), "%llu MB", (unsigned long long)(b >> 20));
            text_draw(s, font_ui, FS, cw - U(80), tty, sz, selected && focus ? 0xFFFFFF : C_TEXT2);
        }
    }
    ui_scrollbar(s, cw, hd, ch - hd - foot(), nent, vis, scroll);
    /* Fusszeile: Pfad bzw. Meldung */
    int fy = ch - foot();
    gfx_fill(s, 0, fy, cw, foot(), 0xFAFAFA);
    gfx_fill(s, 0, fy, cw, 1, 0xE5E5E5);
    char t[300];
    if (status[0])
        snprintf(t, sizeof(t), "%s", status);
    else
        snprintf(t, sizeof(t), "%s  \xC2\xB7  %d Eintr\xC3\xA4ge", dir, nent - (strcmp(dir, "/") != 0));
    text_draw(s, font_ui, FS_SMALL, U(12), fy + (foot() - text_height(font_ui, FS_SMALL)) / 2, t, C_TEXT2);
    gfx_present_all();
}

static void keep_visible(void)
{
    int vis = visible();
    if (sel < scroll)
        scroll = sel;
    if (sel >= scroll + vis)
        scroll = sel - vis + 1;
    if (scroll > nent - vis)
        scroll = nent - vis;
    if (scroll < 0)
        scroll = 0;
}

void _start(int argc, char **argv)
{
    if (argc > 1)
        snprintf(dir, sizeof(dir), "%s", argv[1]);
    ui_setup(0);
    if (gfx_open_window_ex(U(560), U(420), "Dateien", GFX_RESIZABLE) != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    load();
    draw();
    s64 last_click = 0;
    int last_i = -1;
    for (;;) {
        Event e;
        if (!gfx_wait(&e, -1))
            continue;
        if (e.type == EV_CLOSE)
            break;
        if (e.type == EV_KEY) {
            int k = e.key;
            if ((k == 0x1B || k == 'q') && !gfx_windowed())
                break;
            if (k == KEY_DOWN && sel + 1 < nent) sel++;
            else if (k == KEY_UP && sel > 0) sel--;
            else if (k == KEY_HOME) sel = 0;
            else if (k == KEY_END) sel = nent - 1;
            else if (k == KEY_PGDN) sel = sel + visible() < nent ? sel + visible() : nent - 1;
            else if (k == KEY_PGUP) sel = sel > visible() ? sel - visible() : 0;
            else if (k == '\n') open_entry(sel);
            else if ((k == '\b' || k == 0x7F) && nent && strcmp(ents[0].name, "..") == 0) open_entry(0);
            else continue;
            keep_visible();
            draw();
        } else if (e.type == EV_DOWN && e.button == 1) {
            if (e.y < head() || e.y >= gfx_screen.h - foot())
                continue;
            int i = scroll + (e.y - head()) / ROW_H;
            if (i >= nent)
                continue;
            s64 now = sys_ticks();
            int dbl = i == last_i && now - last_click < 40;
            last_click = dbl ? 0 : now;
            last_i = i;
            sel = i;
            if (dbl)
                open_entry(i);
            draw();
        } else if (e.type == EV_WHEEL) {
            scroll -= e.wheel * 3;
            if (scroll > nent - visible()) scroll = nent - visible();
            if (scroll < 0) scroll = 0;
            draw();
        } else if (e.type == EV_RESIZE) {
            keep_visible();
            draw();
        } else if (e.type == EV_FOCUS) {
            focus = e.key;
            draw();
        }
    }
    gfx_close();
    sys_exit(0);
}
