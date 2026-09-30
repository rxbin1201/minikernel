#include "libc.h"

/* resolution                 listet die Grafikmodi, die der Bildschirm bzw. die Firmware anbietet (* = aktuell)
 * resolution 1920x1080       speichert den Wunsch in \cmdline.txt auf dem Boot-Volume: gilt ab dem naechsten Start
 * resolution max | auto      hoechste Aufloesung / die der Firmware (Standard)
 * resolution -s N            Schriftvergroesserung 1..4 (auto: ab ca. 2400 Pixel Breite 2)
 * Optionen: -r sofort neu starten, -d PFAD Boot-Volume selbst angeben (z.B. /mnt/usb0p1)
 * Der Modus kann nur der Bootloader einstellen (vor dem Kernel), deshalb wirkt eine Aenderung erst nach einem Neustart. */
#define MAX_MODES 48

static VideoInfo modes[MAX_MODES];
static int       nmodes;

static void list_modes(void)
{
    int tty = sys_isatty(1) != 0;
    VideoInfo cur = {0};
    int order[MAX_MODES];
    for (int i = 0; i < nmodes; i++)
        order[i] = i;
    for (int i = 1; i < nmodes; i++) { /* nach Flaeche sortieren */
        int x = order[i], j = i - 1;
        while (j >= 0 && (u64)modes[order[j]].width * modes[order[j]].height > (u64)modes[x].width * modes[x].height) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = x;
    }
    printf("%sVerfuegbare Grafikmodi:%s\n", tty ? C_BOLD : "", tty ? C_RESET : "");
    for (int k = 0; k < nmodes; k++) {
        VideoInfo *m = &modes[order[k]];
        if (m->current)
            cur = *m;
        printf("  %s%4ux%-4u%s%s\n", m->current && tty ? C_GREEN : "", m->width, m->height, m->current && tty ? C_RESET : "",
               m->current ? "  <- aktuell" : "");
    }
    if (cur.width)
        printf("Konsole: %u x %u Zeichen, Schrift x%u\n", cur.cols, cur.rows, cur.scale);
    printf("%sAendern: resolution 1920x1080 (oder max, auto), danach neu starten (reboot).%s\n", tty ? C_DIM : "", tty ? C_RESET : "");
}

void _start(int argc, char **argv)
{
    for (VideoInfo vi; nmodes < MAX_MODES && sys_videoinfo((u64)nmodes, &vi) == 0;)
        modes[nmodes++] = vi;

    const char *mode = NULL, *scale = NULL, *dir = NULL;
    int reboot = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0)
            reboot = 1;
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            scale = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            dir = argv[++i];
        else if (argv[i][0] != '-')
            mode = argv[i];
        else {
            fprintf(2, "resolution: unbekannte Option '%s'\n", argv[i]);
            sys_exit(2);
        }
    }
    if (!mode && !scale) {
        if (!nmodes)
            printf("Die Firmware hat keine Grafikmodi gemeldet (nur der aktuelle Modus ist nutzbar).\n");
        else
            list_modes();
        sys_exit(0);
    }

    /* Wert pruefen */
    char valbuf[24];
    const char *mode_value = NULL;
    if (mode && strcmp(mode, "auto") != 0) {
        if (strcmp(mode, "max") == 0) {
            mode_value = "max";
        } else {
            unsigned w = 0, h = 0;
            const char *p = mode;
            while (*p >= '0' && *p <= '9')
                w = w * 10 + (unsigned)(*p++ - '0');
            if (*p == 'x')
                p++;
            while (*p >= '0' && *p <= '9')
                h = h * 10 + (unsigned)(*p++ - '0');
            if (!w || !h || *p) {
                fprintf(2, "resolution: '%s' ist keine Aufloesung (Beispiel: 1920x1080, max oder auto)\n", mode);
                sys_exit(2);
            }
            int exact = 0;
            for (int i = 0; i < nmodes; i++)
                exact |= modes[i].width == w && modes[i].height == h;
            if (nmodes && !exact) {
                fprintf(2, "resolution: %ux%u wird nicht angeboten; der Bootloader nimmt die naechstliegende Aufloesung.\n", w, h);
                list_modes();
            }
            snprintf(valbuf, sizeof(valbuf), "%ux%u", w, h);
            mode_value = valbuf;
        }
    }
    const char *scale_value = NULL;
    if (scale && strcmp(scale, "auto") != 0) {
        if (scale[0] < '1' || scale[0] > '4' || scale[1]) {
            fprintf(2, "resolution: die Schriftvergroesserung ist 1..4 oder auto\n");
            sys_exit(2);
        }
        scale_value = scale;
    }

    char dirs[4][40];
    int n = 0;
    if (dir) {
        snprintf(dirs[0], 40, "%s", dir);
        n = 1;
    } else {
        n = find_boot_volumes(dirs, 4);
    }
    if (!n) {
        fprintf(2, "resolution: kein Boot-Volume gefunden (enthaelt \\kernel.elf und \\EFI). Mit -d PFAD angeben, z.B. -d /mnt/usb0p1\n");
        sys_exit(1);
    }

    int rc = 0;
    for (int i = 0; i < n; i++) {
        int r = 0;
        if (mode)
            r = boot_cmdline_set(dirs[i], "mode=", mode_value);
        if (r == 0 && scale)
            r = boot_cmdline_set(dirs[i], "scale=", scale_value);
        if (r == 0)
            printf("%s/cmdline.txt aktualisiert (%s%s%s%s%s).\n", dirs[i], mode ? "mode=" : "", mode ? (mode_value ? mode_value : "auto") : "",
                   mode && scale ? ", " : "", scale ? "scale=" : "", scale ? (scale_value ? scale_value : "auto") : "");
        else {
            fprintf(2, "resolution: %s/cmdline.txt konnte nicht geschrieben werden (Fehler %d%s)\n", dirs[i], r,
                    r == -30 ? ": Volume ist nur lesbar" : "");
            rc = 1;
        }
    }
    if (rc == 0) {
        if (reboot) {
            printf("Starte neu ...\n");
            sys_power(1);
        }
        printf("Gilt ab dem naechsten Start (reboot).\n");
    }
    sys_exit(rc);
}
