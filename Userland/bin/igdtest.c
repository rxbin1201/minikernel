#include "libc.h"

/* igdtest [cursor|blit]: testet den Intel-Grafiktreiber auf echter Hardware.
 *   igdtest         Page-Flipping: das Bild ist etwa 2 Sekunden invertiert, flackert dann kurz und kommt zurueck
 *   igdtest cursor  Hardware-Mauszeiger: ein Pfeil kreist 3 Sekunden um die Bildmitte und verschwindet wieder
 *   igdtest blit    Blitter: farbige Rechtecke erscheinen nacheinander, dann scrollt das Bild fuenfmal nach oben;
 *                   danach wird die Konsole neu gezeichnet
 * Die Messwerte stehen im Kernel-Log: danach "dmesg > /disk/igd.txt" und die Datei schicken. */
void _start(int argc, char **argv)
{
    int op = 1;
    const char *what = "Bildschirm wird kurz invertiert";
    if (argc > 1 && strcmp(argv[1], "cursor") == 0) {
        op = 2;
        what = "ein Pfeil kreist 3 s um die Bildmitte";
    } else if (argc > 1 && strcmp(argv[1], "blit") == 0) {
        op = 3;
        what = "farbige Rechtecke, dann scrollt das Bild";
    } else if (argc > 1) {
        fprintf(2, "Aufruf: igdtest [cursor|blit]\n");
        sys_exit(2);
    }
    printf("igdtest: startet in 1 s (%s) ...\n", what);
    sys_sleep_ms(1000);
    s64 r = sys_gpu((u64)op);
    printf("igdtest: %s (Ergebnis %lld). Details: dmesg | grep igd\n", r == 0 ? "OK" : "FEHLER", (long long)r);
    sys_exit(r == 0 ? 0 : 1);
}
