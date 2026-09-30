#include "user.h"

/* Startet den Rechner neu. */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    u_puts("reboot: System wird neu gestartet...\n");
    sys_power(1);
    u_puts("reboot: fehlgeschlagen\n");
    sys_exit(1);
}
