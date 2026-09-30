#include "user.h"

/* Wartet auf ein Zeichen von der Tastatur und liefert es als Exit-Code. */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (;;) {
        s64 c = sys_getchar();
        if (c >= 0)
            sys_exit((int)c);
        sys_sleep_ms(10);
    }
}
