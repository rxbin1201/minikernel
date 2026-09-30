#include "libc.h"

/* seq [von] bis: gibt die Zahlen von..bis aus, eine pro Zeile */
void _start(int argc, char **argv)
{
    int from = 1, to;
    if (argc == 2) {
        to = atoi(argv[1]);
    } else if (argc == 3) {
        from = atoi(argv[1]);
        to = atoi(argv[2]);
    } else {
        fprintf(2, "Aufruf: seq [von] bis\n");
        sys_exit(2);
    }
    for (int i = from; i <= to; i++)
        printf("%d\n", i);
    sys_exit(0);
}
