#include "gfx.h"

/* anim: Bewegungstest fuer den Bildwechsel-Takt. Eine Kugel huepft, ein senkrechter Balken wandert; jedes Bild wird
 * ganz neu gezeichnet und mit gfx_vsync() im Takt des Monitors angezeigt (Doppelpufferung: kein Zerreissen).
 * Oben stehen die gezeigten Bilder pro Sekunde. Leertaste: Takt an/aus (zum Vergleich: dann 10-ms-Pausen). Esc: Ende. */

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (gfx_open() != 0)
        sys_exit(1);
    sys_tty_fg(0);
    gfx_show_cursor(0);
    Surface *s = &gfx_screen;
    int r = s->h / 12, x = r, y = r, dx = s->w / 250 + 1, dy = s->h / 180 + 1, bar = 0, sync = 1;
    int frames = 0, fps = 0, vsync_ok = 0;
    s64 sec = sys_ticks();
    for (;;) {
        Event e;
        while (gfx_poll(&e))
            if (e.type == EV_KEY) {
                if (e.key == 0x1B || e.key == 'q') {
                    gfx_close();
                    sys_exit(0);
                }
                if (e.key == ' ')
                    sync = !sync;
            }
        x += dx;
        y += dy;
        if (x < r || x > s->w - r)
            dx = -dx, x += 2 * dx;
        if (y < r + 40 || y > s->h - r)
            dy = -dy, y += 2 * dy;
        bar = (bar + s->w / 200 + 1) % s->w;

        gfx_fill(s, 0, 0, s->w, s->h, RGB(16, 20, 32));
        gfx_fill(s, bar, 40, s->w / 60 + 4, s->h - 40, RGB(60, 120, 200));
        gfx_fill_circle(s, x, y, r, RGB(240, 180, 40));
        char t[160];
        snprintf(t, sizeof(t), "anim: %d Bilder/s  %s  (Leertaste: Takt %s, Esc: Ende)", fps,
                 !sync ? "10-ms-Pausen" : vsync_ok ? "im Takt des Monitors" : "kein Bildwechsel-Interrupt, 10-ms-Pausen",
                 sync ? "aus" : "an");
        gfx_fill(s, 0, 0, s->w, 40, RGB(0, 0, 0));
        gfx_text_scaled(s, 12, 4, t, RGB(230, 230, 230), GFX_TRANSPARENT, 2);
        gfx_present_all();

        if (sync)
            vsync_ok = gfx_vsync();
        else
            sys_sleep_ms(10);
        frames++;
        s64 now = sys_ticks();
        if (now - sec >= 100) { /* Ticks zu 10 ms */
            fps = (int)(frames * 100 / (now - sec));
            frames = 0;
            sec = now;
        }
    }
}
