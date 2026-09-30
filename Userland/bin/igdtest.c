#include "libc.h"

/* igdtest [cursor]: testet den Intel-Grafiktreiber (Stufe 2) auf echter Hardware.
 *   igdtest         Page-Flipping: das Bild ist etwa 2 Sekunden invertiert, flackert dann kurz und kommt zurueck
 *   igdtest cursor  Hardware-Mauszeiger: ein Pfeil kreist 3 Sekunden um die Bildmitte und verschwindet wieder
 * Die Messwerte stehen im Kernel-Log: danach "dmesg > /disk/igd.txt" und die Datei schicken. */
void _start(int argc, char **argv)
{
    int cursor = argc > 1 && strcmp(argv[1], "cursor") == 0;
    if (argc > 1 && !cursor) {
        fprintf(2, "Aufruf: igdtest [cursor]\n");
        sys_exit(2);
    }
    printf("igdtest: startet in 1 s (%s) ...\n",
           cursor ? "ein Pfeil kreist 3 s um die Bildmitte" : "Bildschirm wird kurz invertiert");
    sys_sleep_ms(1000);
    s64 r = sys_gpu(cursor ? 2 : 1);
    printf("igdtest: %s (Ergebnis %lld). Details: dmesg | grep igd\n", r == 0 ? "OK" : "FEHLER", (long long)r);
    sys_exit(r == 0 ? 0 : 1);
}
