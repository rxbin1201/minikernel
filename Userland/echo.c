#include "libc.h"

/* echo [-n] [-e] text...: gibt die Argumente mit Leerzeichen getrennt aus.
 * -e wertet \e (Escape, fuer Farben: echo -e "\e[31mrot\e[0m"), \n, \t und \\ aus. */
static void put_escaped(const char *s)
{
    for (; *s; s++) {
        char c = *s;
        if (c == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'e': c = 0x1B; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case '\\': c = '\\'; break;
            default:
                write_all(1, "\\", 1);
                c = *s;
            }
        }
        write_all(1, &c, 1);
    }
}

void _start(int argc, char **argv)
{
    int newline = 1, escapes = 0, first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        const char *o = argv[first] + 1;
        int known = 1;
        for (const char *q = o; *q; q++)
            if (*q != 'n' && *q != 'e')
                known = 0;
        if (!known)
            break;
        for (; *o; o++) {
            if (*o == 'n')
                newline = 0;
            else
                escapes = 1;
        }
    }
    for (int i = first; i < argc; i++) {
        if (i > first)
            write_all(1, " ", 1);
        if (escapes)
            put_escaped(argv[i]);
        else
            write_all(1, argv[i], strlen(argv[i]));
    }
    if (newline)
        write_all(1, "\n", 1);
    sys_exit(0);
}
