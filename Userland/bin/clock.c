#include "gfx.h"
#include "ui.h"

/* clock: Analoguhr mit Datum (Zeit aus der RTC). Unter dem Desktop im Fenster, sonst im Vollbild (Esc/q beendet). */

static void hand(float cx, float cy, float a, float len, float back, float width, u32 c)
{
    float dx = ui_sin(a), dy = -ui_cos(a);
    gfx_capsule(&gfx_screen, cx - dx * back, cy - dy * back, cx + dx * len, cy + dy * len, width, c, 255);
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    int cw = s->w, ch = s->h;
    gfx_fill(s, 0, 0, cw, ch, 0xF5F5F7);
    float r = (float)((cw < ch - U(70) ? cw : ch - U(70)) / 2 - U(16)), cx = cw * 0.5f, cy = U(16) + r;
    if (r < 20)
        return;
    gfx_shadow(s, (int)(cx - r), (int)(cy - r + U(3)), (int)(2 * r), (int)(2 * r), (int)r, U(14), 50);
    gfx_disc(s, cx, cy, r, 0xFFFFFF, 255);
    gfx_ring(s, cx, cy, r, U(1) * 1.5f, 0xD1D1D6, 255);
    const float step = 6.2831853f / 60;
    for (int i = 0; i < 60; i++) {
        float dx = ui_sin(i * step), dy = -ui_cos(i * step), in = i % 5 ? r * 0.90f : r * 0.82f;
        gfx_capsule(s, cx + dx * in, cy + dy * in, cx + dx * r * 0.94f, cy + dy * r * 0.94f, i % 5 ? U(1) * 1.0f : U(1) * 2.5f,
                    i % 5 ? 0xAEAEB2 : 0x1D1D1F, 255);
    }
    static const char *num[4] = {"12", "3", "6", "9"};
    for (int k = 0; k < 4; k++) {
        int fs = (int)(r * 0.20f);
        float dx = ui_sin(k * 15 * step), dy = -ui_cos(k * 15 * step);
        int tw = text_width(font_bold, fs, num[k]);
        text_draw(s, font_bold, fs, (int)(cx + dx * r * 0.66f) - tw / 2, (int)(cy + dy * r * 0.66f) - text_height(font_bold, fs) / 2,
                  num[k], 0x1D1D1F);
    }
    s64 now = sys_time();
    DateTime dt = {0, 0, 0, 0, 0, 0, 0};
    if (now > 0)
        time_to_date((u64)now, &dt);
    hand(cx, cy, (dt.hour % 12 * 60 + dt.min) * step / 12, r * 0.50f, 0, U(1) * 5.0f, 0x1D1D1F);
    hand(cx, cy, (dt.min * 60 + dt.sec) * step / 60, r * 0.78f, 0, U(1) * 3.5f, 0x1D1D1F);
    hand(cx, cy, dt.sec * step, r * 0.86f, r * 0.15f, U(1) * 1.5f, 0xFF9500);
    gfx_disc(s, cx, cy, U(1) * 4.0f, 0xFF9500, 255);
    gfx_disc(s, cx, cy, U(1) * 1.5f, 0xFFFFFF, 255);
    char t[40];
    snprintf(t, sizeof(t), "%02d:%02d:%02d", dt.hour, dt.min, dt.sec);
    int fs = U(22), tw = text_width(font_bold, fs, t);
    text_draw(s, font_bold, fs, (cw - tw) / 2, (int)(cy + r + U(12)), t, C_TEXT);
    snprintf(t, sizeof(t), "%02d.%02d.%04d", dt.day, dt.month, dt.year);
    tw = text_width(font_ui, FS, t);
    text_draw(s, font_ui, FS, (cw - tw) / 2, (int)(cy + r + U(12)) + text_height(font_bold, fs), t, C_TEXT2);
    gfx_present_all();
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ui_setup(0);
    if (gfx_open_window_ex(U(300), U(330), "Uhr", GFX_RESIZABLE) != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    draw();
    s64 shown = sys_time();
    for (;;) {
        Event e;
        int got = gfx_wait(&e, 100);
        if (got && (e.type == EV_CLOSE || (e.type == EV_KEY && (e.key == 0x1B || e.key == 'q'))))
            break;
        s64 now = sys_time();
        if (now != shown || (got && e.type == EV_RESIZE)) { /* jede Sekunde */
            shown = now;
            draw();
        }
    }
    gfx_close();
    sys_exit(0);
}
