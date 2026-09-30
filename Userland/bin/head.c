#include "libc.h"

/* head [-n N] [datei]: die ersten N Zeilen (Standard 10) */
void _start(int argc, char **argv)
{
    int limit = 10, i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        limit = atoi(argv[i + 1]);
        i += 2;
    }
    int fd = 0;
    if (i < argc) {
        s64 f = sys_open(argv[i], O_RDONLY);
        if (f < 0) {
            fprintf(2, "head: '%s': nicht gefunden\n", argv[i]);
            sys_exit(1);
        }
        fd = (int)f;
    }

    char line[1024];
    for (int n = 0; n < limit && read_line(fd, line, sizeof(line)) >= 0; n++)
        printf("%s\n", line);
    sys_exit(0);
}
