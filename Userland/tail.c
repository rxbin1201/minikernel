#include "libc.h"
#include "malloc.h"

/* tail [-n N] [datei]: die letzten N Zeilen (Standard 10) */
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
            fprintf(2, "tail: '%s': nicht gefunden\n", argv[i]);
            sys_exit(1);
        }
        fd = (int)f;
    }

    u64 cap = 65536, n = 0;
    char *b = u_malloc(cap);
    while (b) {
        if (n == cap) {
            char *nb = u_malloc(cap * 2);
            if (!nb)
                break;
            memcpy(nb, b, n);
            u_free(b);
            b = nb;
            cap *= 2;
        }
        s64 r = sys_read(fd, b + n, cap - n);
        if (r <= 0)
            break;
        n += (u64)r;
    }
    if (!b) {
        fprintf(2, "tail: kein Speicher\n");
        sys_exit(1);
    }

    u64 start = n;
    int lines = 0;
    if (limit > 0) {
        if (n && b[n - 1] == '\n')
            lines = -1; /* das letzte Zeilenende gehoert zur letzten Zeile */
        while (start > 0) {
            if (b[start - 1] == '\n' && ++lines >= limit)
                break;
            start--;
        }
    }
    if (limit > 0)
        write_all(1, b + start, (size_t)(n - start));
    sys_exit(0);
}
