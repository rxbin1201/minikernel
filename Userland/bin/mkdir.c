#include "libc.h"

/* mkdir pfad...: legt Verzeichnisse an (auf beschreibbaren FAT-Datentraegern wie /disk, lange Namen gehen) */
void _start(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        fprintf(2, "Aufruf: mkdir pfad...\n");
        sys_exit(2);
    }
    for (int i = 1; i < argc; i++) {
        s64 r = sys_mkdir(argv[i]);
        if (r < 0) {
            fprintf(2, "mkdir: '%s' fehlgeschlagen (Fehler %d)\n", argv[i], (int)r);
            rc = 1;
        }
    }
    sys_exit(rc);
}
