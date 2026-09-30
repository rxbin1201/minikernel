#include "libc.h"

/* grep [-v] [-n] muster [datei...]: gibt Zeilen aus, die das Muster (als Text, kein Regex) enthalten */
static int invert, numbered, matched, tty;

static int scan(int fd, const char *pattern, const char *prefix)
{
    char line[1024];
    int len, number = 0;
    while ((len = read_line(fd, line, sizeof(line))) >= 0) {
        number++;
        int hit = strstr(line, pattern) != NULL;
        if (hit == invert)
            continue;
        matched = 1;
        if (prefix)
            printf(tty ? C_MAGENTA "%s" C_RESET C_DIM ":" C_RESET : "%s:", prefix);
        if (numbered)
            printf(tty ? C_GREEN "%d" C_RESET C_DIM ":" C_RESET : "%d:", number);
        const char *p = line, *at;
        size_t plen = strlen(pattern);
        if (tty && !invert && plen) { /* jedes Vorkommen des Musters rot hervorheben */
            while ((at = strstr(p, pattern))) {
                write_all(1, p, (size_t)(at - p));
                write_all(1, C_RED, strlen(C_RED));
                write_all(1, at, plen);
                write_all(1, C_RESET, 4);
                p = at + plen;
            }
        }
        printf("%s\n", p);
    }
    return 0;
}

void _start(int argc, char **argv)
{
    tty = sys_isatty(1) != 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'v')
                invert = 1;
            else if (*o == 'n')
                numbered = 1;
        }
    }
    if (i >= argc) {
        fprintf(2, "Aufruf: grep [-v] [-n] muster [datei...]\n");
        sys_exit(2);
    }
    const char *pattern = argv[i++];

    if (i >= argc) {
        scan(0, pattern, NULL);
    } else {
        for (int many = argc - i > 1; i < argc; i++) {
            s64 fd = sys_open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(2, "grep: '%s': nicht gefunden\n", argv[i]);
                continue;
            }
            scan((int)fd, pattern, many ? argv[i] : NULL);
            sys_close((int)fd);
        }
    }
    sys_exit(matched ? 0 : 1);
}
