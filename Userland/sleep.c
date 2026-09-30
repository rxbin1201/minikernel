#include "libc.h"

/* sleep sekunden: wartet (Ctrl-C beendet es) */
void _start(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(2, "Aufruf: sleep sekunden\n");
        sys_exit(2);
    }
    sys_sleep_ms((u64)atoi(argv[1]) * 1000);
    sys_exit(0);
}
