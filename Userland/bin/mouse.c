#include "libc.h"

/* mouse [-n N]: zeigt Position, Tasten und Mausrad der USB-Maus, solange man 'q' drueckt (oder nach N Meldungen mit -n).
 * Der Mauszeiger selbst wird vom Kernel gezeichnet, sobald eine USB-Maus angeschlossen ist; das Mausrad blaettert im
 * Verlauf der Konsole. Zeile: "M x y tasten rad" (Tasten: L = links, R = rechts, M = Mitte). */
void _start(int argc, char **argv)
{
    int limit = 0;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0)
        limit = atoi(argv[2]);

    MouseInfo mi;
    if (sys_mouse(&mi) != 0) {
        fprintf(2, "mouse: Systemaufruf fehlgeschlagen\n");
        sys_exit(1);
    }
    if (!mi.attached && !limit)
        printf("Keine USB-Maus gefunden (angeschlossen? 'lsusb' zeigt Geraete mit dem Treiber \"Maus\").\n");
    else if (!limit)
        printf("Bildschirm %ux%u. Maus bewegen/klicken, 'q' beendet.\n", mi.width, mi.height);

    unsigned last_events = mi.events;
    int shown = 0;
    for (;;) {
        s64 c = sys_getchar();
        if (c == 'q' || c == 'Q' || c == 3)
            break;
        if (sys_mouse(&mi) == 0 && mi.events != last_events) {
            last_events = mi.events;
            printf("M %d %d %s%s%s %d\n", mi.x, mi.y, mi.buttons & 1 ? "L" : "-", mi.buttons & 2 ? "R" : "-",
                   mi.buttons & 4 ? "M" : "-", mi.wheel);
            if (limit && ++shown >= limit)
                break;
        }
        sys_sleep_ms(10);
    }
    sys_exit(0);
}
