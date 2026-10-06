#include "libc.h"

/* grep [-v] [-n] [-e muster]... [muster] [datei...]: gibt Zeilen aus, die das Muster (als Text, kein Regex) enthalten;
 * mit mehreren -e eines davon */
#define MAX_PATTERNS 16
static int         invert, numbered, matched, tty, npat;
static const char *pats[MAX_PATTERNS];

static int scan(int fd, const char *pattern, const char *prefix)
{
    char line[1024];
    int len, number = 0;
    while ((len = read_line(fd, line, sizeof(line))) >= 0) {
        number++;
        int hit = 0;
        for (int k = 0; k < npat && !hit; k++)
            hit = strstr(line, pats[k]) != NULL;
        if (hit == invert)
            continue;
        matched = 1;
        if (prefix)
            printf(tty ? C_MAGENTA "%s" C_RESET C_DIM ":" C_RESET : "%s:", prefix);
        if (numbered)
            printf(tty ? C_GREEN "%d" C_RESET C_DIM ":" C_RESET : "%d:", number);
        const char *p = line, *at;
        size_t plen = strlen(pattern);
        if (tty && !invert && plen && npat == 1) { /* jedes Vorkommen des Musters rot hervorheben */
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
        if (strcmp(argv[i], "-e") == 0) { /* weiteres Muster */
            if (i + 1 < argc && npat < MAX_PATTERNS)
                pats[npat++] = argv[++i];
            continue;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'v')
                invert = 1;
            else if (*o == 'n')
                numbered = 1;
        }
    }
    if (!npat && i < argc) /* ohne -e: das erste Wort ist das Muster */
        pats[npat++] = argv[i++];
    if (!npat) {
        fprintf(2, "Aufruf: grep [-v] [-n] [-e muster]... [muster] [datei...]\n");
        sys_exit(2);
    }
    const char *pattern = pats[0];

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
