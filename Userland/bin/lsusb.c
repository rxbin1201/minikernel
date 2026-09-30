#include "libc.h"

/* lsusb: listet die erkannten USB-Geraete (Port bzw. Hub-Pfad, ID, Geschwindigkeit, verwendeter Treiber).
 * Der Pfad zeigt die Baumstruktur: "1" = Root-Port 1, "1.3" = Port 3 des Hubs an Root-Port 1. */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char *speeds[] = {"?", "Full (12 Mbit/s)", "Low (1,5 Mbit/s)", "High (480 Mbit/s)", "Super (5 Gbit/s)", "Super+ (10 Gbit/s)"};
    static const char *drivers[] = {"kein Treiber", "Tastatur", "Massenspeicher", "Hub", "Maus"};

    int tty = sys_isatty(1) != 0;
    UsbInfo info;
    int n = 0;
    for (u64 i = 0; sys_usbinfo(i, &info) == 0; i++, n++) {
        int depth = 0;
        for (const char *c = info.path; *c; c++)
            depth += *c == '.';
        printf("Port %-7s %04x:%04x  %-18s  Klasse %02x  %s%s%s\n", info.path, info.vid, info.pid,
               info.speed < 6 ? speeds[info.speed] : "?", info.cls, tty && info.driver == 3 ? C_CYAN : "",
               info.driver < 5 ? drivers[info.driver] : "?", tty && info.driver == 3 ? C_RESET : "");
        (void)depth;
    }
    if (!n)
        printf("keine USB-Geraete\n");
    sys_exit(0);
}
