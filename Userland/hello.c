#include "user.h"

static char message[] = "Hallo aus /bin/hello!"; /* .data: beschreibbar */

void _start(int argc, char **argv)
{
    u_puts("[hello] ");
    u_puts(message);
    u_puts(" PID ");
    u_putdec(sys_getpid());
    u_puts(", argc=");
    u_putdec(argc);
    for (int i = 0; i < argc; i++) {
        u_puts(" '");
        u_puts(argv[i]);
        u_puts("'");
    }
    u_puts("\n");

    for (int i = 1; i <= 3; i++) {
        sys_sleep_ms(30);
        u_puts("[hello] tick ");
        u_putdec(i);
        u_puts("\n");
    }
    sys_exit(42);
}
