#include "gfx.h"
#include "ui.h"

/* about: "Ueber MiniKernel" - Prozessor, Bildschirm, Laufzeit, Prozesse (jede Sekunde neu). Unter dem Desktop im
 * Fenster, sonst im Vollbild (Esc/q beendet). */

static int scr_w, scr_h;

static void draw(void)
{
    Surface *s = &gfx_screen;
    gfx_fill(s, 0, 0, s->w, s->h, C_WINDOW);
    int isz = U(96);
    ui_app_icon(s, ICON_ABOUT, U(28), U(34), isz);
    int tx = U(28) + isz + U(28), ty = U(24);
    text_draw(s, font_bold, U(26), tx, ty, "MiniKernel", C_TEXT);
    ty += text_height(font_bold, U(26));
    text_draw(s, font_ui, FS, tx, ty, "Version 1.0", C_TEXT2);
    ty += text_height(font_ui, FS) + U(14);
    int cpus = 0;
    CpuInfo ci;
    while (sys_cpuinfo((u64)cpus, &ci) == 0)
        cpus++;
    u64 up = (u64)sys_ticks() / 100;
    int procs = 0;
    ProcInfo pi;
    for (u64 i = 0; sys_procinfo(i, &pi) == 0; i++)
        if (!pi.state)
            procs++;
    const char *lab[4] = {"Prozessor", "Bildschirm", "L\xC3\xA4uft seit", "Prozesse"};
    char val[4][64];
    snprintf(val[0], sizeof(val[0]), "%d Kern%s (x86-64)", cpus, cpus == 1 ? "" : "e");
    snprintf(val[1], sizeof(val[1]), "%d \xC3\x97 %d", scr_w, scr_h);
    snprintf(val[2], sizeof(val[2]), "%llu:%02llu:%02llu", (unsigned long long)(up / 3600), (unsigned long long)(up / 60 % 60),
             (unsigned long long)(up % 60));
    snprintf(val[3], sizeof(val[3]), "%d", procs);
    for (int i = 0; i < 4; i++) {
        text_draw(s, font_bold, FS, tx, ty, lab[i], C_TEXT);
        text_draw(s, font_ui, FS, tx + U(100), ty, val[i], C_TEXT2);
        ty += text_height(font_ui, FS) + U(3);
    }
    text_draw(s, font_ui, FS_SMALL, tx, ty + U(10), "Eigener 64-Bit-Kernel mit UEFI-Bootloader", C_TEXT2);
    gfx_present_all();
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ui_setup(0);
    if (gfx_open_window(U(480), U(230), "\xC3\x9C" "ber MiniKernel") != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    gfx_display_size(&scr_w, &scr_h);
    draw();
    s64 shown = sys_time();
    for (;;) {
        Event e;
        int got = gfx_wait(&e, 200);
        if (got && (e.type == EV_CLOSE || (e.type == EV_KEY && (e.key == 0x1B || e.key == 'q'))))
            break;
        if (sys_time() != shown) {
            shown = sys_time();
            draw();
        }
    }
    gfx_close();
    sys_exit(0);
}
