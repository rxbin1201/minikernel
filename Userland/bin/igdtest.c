#include "libc.h"

/* igdtest: testet den Intel-Grafiktreiber (Stufe 2, Page-Flipping) auf echter Hardware. Der Bildschirm zeigt fuer
 * etwa 2 Sekunden das Bild mit invertierten Farben, flackert dann kurz (10 Wechsel) und kommt zurueck.
 * Die Messwerte stehen im Kernel-Log: danach "dmesg > /disk/igd.txt" und die Datei schicken. */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("igdtest: startet in 1 s (Bildschirm wird kurz invertiert) ...\n");
    sys_sleep_ms(1000);
    s64 r = sys_gpu(1);
    printf("igdtest: %s (Ergebnis %lld). Details: dmesg | grep igd\n", r == 0 ? "OK" : "FEHLER", (long long)r);
    sys_exit(r == 0 ? 0 : 1);
}
