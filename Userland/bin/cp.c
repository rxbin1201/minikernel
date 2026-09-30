#include "libc.h"

/* cp quelle ziel: kopiert eine Datei (ist ziel ein Verzeichnis, wird der Name der Quelle uebernommen) */
static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1])
            b = p + 1;
    return b;
}

void _start(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(2, "Aufruf: cp quelle ziel\n");
        sys_exit(2);
    }
    char target[192];
    Stat st;
    if (sys_stat(argv[2], &st) == 0 && st.is_dir)
        snprintf(target, sizeof(target), "%s/%s", argv[2], base_name(argv[1]));
    else
        snprintf(target, sizeof(target), "%s", argv[2]);

    s64 in = sys_open(argv[1], O_RDONLY);
    if (in < 0) {
        fprintf(2, "cp: '%s': nicht gefunden\n", argv[1]);
        sys_exit(1);
    }
    s64 out = sys_open(target, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        fprintf(2, "cp: '%s': nicht schreibbar (Fehler %d)\n", target, (int)out);
        sys_exit(1);
    }

    char buf[512];
    s64 n, total = 0;
    while ((n = sys_read((int)in, buf, sizeof(buf))) > 0) {
        if (write_all((int)out, buf, (size_t)n) < 0) {
            fprintf(2, "cp: Schreibfehler\n");
            sys_exit(1);
        }
        total += n;
    }
    printf("%d Bytes kopiert\n", (int)total);
    sys_exit(0);
}
