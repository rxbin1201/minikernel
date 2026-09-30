#include "user.h"

/* Schaltet den Rechner aus. */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    u_puts("poweroff: System wird ausgeschaltet...\n");
    sys_power(0);
    u_puts("poweroff: fehlgeschlagen\n");
    sys_exit(1);
}
