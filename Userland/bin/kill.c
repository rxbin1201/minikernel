#include "libc.h"

/* kill pid...: beendet Prozesse (sie sterben beim naechsten Eintritt in den Kernel) */
void _start(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        fprintf(2, "Aufruf: kill pid...\n");
        sys_exit(2);
    }
    for (int i = 1; i < argc; i++) {
        s64 r = sys_kill(atoi(argv[i]));
        if (r < 0) {
            fprintf(2, "kill: %s: kein solcher Prozess\n", argv[i]);
            rc = 1;
        }
    }
    sys_exit(rc);
}
