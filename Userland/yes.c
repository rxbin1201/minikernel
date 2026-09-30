#include "libc.h"

/* yes [text]: gibt endlos "y" (oder den Text) aus, bis Ctrl-C oder das Ende der Pipe */
void _start(int argc, char **argv)
{
    const char *text = argc > 1 ? argv[1] : "y";
    char line[256];
    int n = snprintf(line, sizeof(line), "%s\n", text);
    while (write_all(1, line, (size_t)n) == 0)
        ;
    sys_exit(0);
}
