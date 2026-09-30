#include "libc.h"

/* cat [datei...]: gibt Dateien aus, ohne Argument stdin (bis Dateiende) */
static int cat_fd(int fd)
{
    char buf[512];
    s64 n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
        if (write_all(1, buf, (size_t)n) < 0)
            return 1;
    return n < 0 ? 1 : 0;
}

void _start(int argc, char **argv)
{
    if (argc < 2)
        sys_exit(cat_fd(0));

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        s64 fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "cat: '%s': nicht gefunden (Fehler %d)\n", argv[i], (int)fd);
            rc = 1;
            continue;
        }
        rc |= cat_fd((int)fd);
        sys_close((int)fd);
    }
    sys_exit(rc);
}
