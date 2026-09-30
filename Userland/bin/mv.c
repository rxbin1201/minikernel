#include "libc.h"

/* mv alt neu: benennt um bzw. verschiebt (nur unter /disk; Verzeichnisse nur in derselben Ebene) */
void _start(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(2, "Aufruf: mv alt neu\n");
        sys_exit(2);
    }
    s64 r = sys_rename(argv[1], argv[2]);
    if (r < 0) {
        fprintf(2, "mv: '%s' -> '%s' fehlgeschlagen (Fehler %d)\n", argv[1], argv[2], (int)r);
        sys_exit(1);
    }
    sys_exit(0);
}
