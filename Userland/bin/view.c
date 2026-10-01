#include "gfx.h"

/* view bild.bmp: Bildbetrachter (BMP, 24/32 Bit).
 *   Leertaste: einpassen / Originalgroesse, + - zoomen, Pfeile verschieben, Bild hoch/runter: vorheriges/naechstes Bild
 *   im selben Ordner, Mausrad zoomt, Ziehen mit der Maus verschiebt, Esc oder q beendet.
 *   Unter dem Desktop im Fenster (Groesse passend zum Bild, aenderbar), sonst im Vollbild. */

static char   dir[256], files[64][128];
static int    nfiles, cur;
static Surface img;
static char   err[96];
static int    zoom_pct, fit = 1, ox, oy; /* Zoom in %, Verschiebung in Bildpixeln */

static int ends_bmp(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".bmp") == 0;
}

static void load_list(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash) {
        int n = (int)(slash - path);
        memcpy(dir, path, (size_t)n);
        dir[n] = 0;
        if (!n)
            snprintf(dir, sizeof(dir), "/");
    } else {
        snprintf(dir, sizeof(dir), ".");
    }
    const char *base = slash ? slash + 1 : path;
    DirEnt e;
    nfiles = 0;
    for (u64 i = 0; nfiles < 64 && sys_readdir(dir, i, &e) == 0; i++) {
        if (!e.is_dir && ends_bmp(e.name)) {
            snprintf(files[nfiles], sizeof(files[0]), "%s", e.name);
            if (strcasecmp(e.name, base) == 0)
                cur = nfiles;
            nfiles++;
        }
    }
    if (!nfiles) { /* Datei ohne .bmp-Endung o.ae. */
        snprintf(files[0], sizeof(files[0]), "%s", base);
        nfiles = 1;
        cur = 0;
    }
}

static void open_current(void)
{
    if (img.px)
        surface_free(&img);
    char p[400];
    snprintf(p, sizeof(p), "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/", files[cur]);
    err[0] = 0;
    if (bmp_load(p, &img, err, sizeof(err)) != 0)
        img.px = 0;
    fit = 1;
    ox = oy = 0;
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    gfx_no_clip();
    gfx_fill(s, 0, 0, s->w, s->h, RGB(32, 32, 38));
    int bar = 20;
    if (img.px) {
        int aw = s->w, ah = s->h - bar;
        if (fit) {
            zoom_pct = 100;
            if (img.w * 100 / aw > 100 || img.h * 100 / ah > 100) { /* nur verkleinern, nie vergroessern */
                int zx = aw * 100 / img.w, zy = ah * 100 / img.h;
                zoom_pct = zx < zy ? zx : zy;
                if (zoom_pct < 1)
                    zoom_pct = 1;
            }
        }
        int dw = img.w * zoom_pct / 100, dh = img.h * zoom_pct / 100;
        int dx = (aw - dw) / 2 - ox * zoom_pct / 100, dy = (ah - dh) / 2 - oy * zoom_pct / 100;
        gfx_set_clip(0, 0, aw, ah);
        gfx_draw_scaled(s, &img, dx, dy, dw, dh);
        gfx_no_clip();
    } else {
        char m[200];
        snprintf(m, sizeof(m), "%s: %s", files[cur], err);
        gfx_text(s, (s->w - gfx_text_width(m)) / 2, s->h / 2, m, RGB(255, 120, 120), GFX_TRANSPARENT);
    }
    gfx_fill(s, 0, s->h - bar, s->w, bar, RGB(55, 55, 65));
    char info[300];
    if (img.px)
        snprintf(info, sizeof(info), " %s   %dx%d   %d%%   (%d/%d)   Leertaste: einpassen/100%%  +/-: Zoom  Bild hoch/runter: "
                 "anderes Bild  Esc: Ende", files[cur], img.w, img.h, zoom_pct, cur + 1, nfiles);
    else
        snprintf(info, sizeof(info), " %s   (%d/%d)   Esc: Ende", files[cur], cur + 1, nfiles);
    gfx_text(s, 2, s->h - bar + 2, info, RGB(230, 230, 240), GFX_TRANSPARENT);
    gfx_present_all();
}

void _start(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(2, "Aufruf: view bild.bmp\n");
        sys_exit(2);
    }
    load_list(argv[1]);
    open_current();
    int ww = 0, wh = 0;
    if (gfx_desktop()) { /* Fenster so gross wie das Bild, hoechstens 80 % des Bildschirms */
        int sw, sh;
        gfx_display_size(&sw, &sh);
        ww = img.px ? img.w : 480;
        wh = (img.px ? img.h : 200) + 20;
        if (ww > sw * 4 / 5) ww = sw * 4 / 5;
        if (wh > sh * 4 / 5) wh = sh * 4 / 5;
        if (ww < 420) ww = 420;
        if (wh < 160) wh = 160;
    }
    if (gfx_open_window_ex(ww, wh, files[cur], GFX_RESIZABLE) != 0)
        sys_exit(1);
    sys_tty_fg(0);
    draw();
    int dragging = 0, lx = 0, ly = 0;
    for (;;) {
        Event e;
        if (!gfx_wait(&e, -1))
            continue;
        int redraw = 1;
        if (e.type == EV_CLOSE)
            break;
        if (e.type == EV_RESIZE) {
            draw();
            continue;
        }
        if (e.type == EV_KEY) {
            int k = e.key;
            if (k == 0x1B || k == 'q')
                break;
            else if (k == ' ' || k == 'f') {
                fit = !fit;
                zoom_pct = 100;
                ox = oy = 0;
            } else if (k == '+' || k == '=') {
                fit = 0;
                zoom_pct = zoom_pct * 5 / 4 + 1;
            } else if (k == '-') {
                fit = 0;
                zoom_pct = zoom_pct * 4 / 5;
                if (zoom_pct < 2)
                    zoom_pct = 2;
            } else if (k == KEY_LEFT) {
                ox -= 40 * 100 / (zoom_pct ? zoom_pct : 100);
            } else if (k == KEY_RIGHT) {
                ox += 40 * 100 / (zoom_pct ? zoom_pct : 100);
            } else if (k == KEY_UP) {
                oy -= 40 * 100 / (zoom_pct ? zoom_pct : 100);
            } else if (k == KEY_DOWN) {
                oy += 40 * 100 / (zoom_pct ? zoom_pct : 100);
            } else if (k == KEY_PGDN || k == 'n') {
                cur = (cur + 1) % nfiles;
                open_current();
                gfx_set_title(files[cur]);
            } else if (k == KEY_PGUP || k == 'p') {
                cur = (cur + nfiles - 1) % nfiles;
                open_current();
                gfx_set_title(files[cur]);
            } else {
                redraw = 0;
            }
        } else if (e.type == EV_WHEEL) {
            fit = 0;
            zoom_pct = e.wheel > 0 ? zoom_pct * 5 / 4 + 1 : zoom_pct * 4 / 5;
            if (zoom_pct < 2)
                zoom_pct = 2;
        } else if (e.type == EV_DOWN && e.button == 1) {
            dragging = 1;
            lx = e.x;
            ly = e.y;
            redraw = 0;
        } else if (e.type == EV_UP) {
            dragging = 0;
            redraw = 0;
        } else if (e.type == EV_MOVE && dragging && zoom_pct) {
            ox -= (e.x - lx) * 100 / zoom_pct;
            oy -= (e.y - ly) * 100 / zoom_pct;
            lx = e.x;
            ly = e.y;
        } else {
            redraw = 0;
        }
        if (redraw)
            draw();
    }
    gfx_close();
    sys_exit(0);
}
