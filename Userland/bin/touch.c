#include "libc.h"

/* touch datei...: legt leere Dateien an (bestehende bleiben unveraendert) */
void _start(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        s64 fd = sys_open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            fprintf(2, "touch: '%s' fehlgeschlagen (Fehler %d)\n", argv[i], (int)fd);
            rc = 1;
            continue;
        }
        sys_close((int)fd);
    }
    sys_exit(rc);
}
