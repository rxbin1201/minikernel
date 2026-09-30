#include "libc.h"

/* rm pfad...: loescht Dateien und leere Verzeichnisse (nur unter /disk) */
void _start(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        fprintf(2, "Aufruf: rm pfad...\n");
        sys_exit(2);
    }
    for (int i = 1; i < argc; i++) {
        s64 r = sys_unlink(argv[i]);
        if (r < 0) {
            fprintf(2, "rm: '%s' fehlgeschlagen (Fehler %d)\n", argv[i], (int)r);
            rc = 1;
        }
    }
    sys_exit(rc);
}
