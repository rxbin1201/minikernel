#include "util.h"

/* hexdump [-n anzahl] [-s start] [datei]: Inhalt als Hex und Text, 16 Bytes pro Zeile (wie "hexdump -C").
 * Gleiche Zeilen hintereinander werden als "*" zusammengefasst. */
void _start(int argc, char **argv)
{
    u64 limit = ~0ULL, skip = 0;
    const char *path = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            limit = (u64)atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            skip = (u64)atoi(argv[++i]);
        else if (strcmp(argv[i], "-C") == 0)
            ;
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(2, "Aufruf: hexdump [-n anzahl] [-s start] [datei]\n");
            sys_exit(2);
        } else
            path = argv[i];
    }
    int fd = 0;
    if (path) {
        s64 f = sys_open(path, O_RDONLY);
        if (f < 0) {
            fprintf(2, "hexdump: '%s': nicht gefunden\n", path);
            sys_exit(1);
        }
        fd = (int)f;
    }
    int tty = sys_isatty(1) != 0;
    unsigned char line[16], prev[16];
    int have_prev = 0, starred = 0;
    u64 off = 0;
    unsigned char buf[4096];
    int blen = 0, bpos = 0, eof = 0;
    /* ueberspringen */
    while (skip > 0 && !eof) {
        s64 r = sys_read(fd, buf, skip < sizeof(buf) ? skip : sizeof(buf));
        if (r <= 0)
            eof = 1;
        else {
            skip -= (u64)r;
            off += (u64)r;
        }
    }
    for (;;) {
        int n = 0;
        while (n < 16 && limit > 0) {
            if (bpos == blen) {
                if (eof)
                    break;
                s64 r = sys_read(fd, buf, sizeof(buf));
                if (r <= 0) {
                    eof = 1;
                    break;
                }
                blen = (int)r;
                bpos = 0;
            }
            line[n++] = buf[bpos++];
            limit--;
        }
        if (!n)
            break;
        if (n == 16 && have_prev && memcmp(line, prev, 16) == 0) {
            if (!starred)
                out_str("*\n");
            starred = 1;
            off += 16;
            continue;
        }
        starred = 0;
        memcpy(prev, line, 16);
        have_prev = n == 16;
        if (tty)
            out_str(C_DIM);
        out_printf("%08llx", (unsigned long long)off);
        if (tty)
            out_str(C_RESET);
        out_str("  ");
        for (int k = 0; k < 16; k++) {
            if (k < n)
                out_printf("%02x ", line[k]);
            else
                out_str("   ");
            if (k == 7)
                out_str(" ");
        }
        out_str(" |");
        if (tty)
            out_str(C_CYAN);
        for (int k = 0; k < n; k++) {
            char c = line[k] >= 32 && line[k] < 127 ? (char)line[k] : '.';
            out_write(&c, 1);
        }
        if (tty)
            out_str(C_RESET);
        out_str("|\n");
        off += (u64)n;
    }
    out_printf("%08llx\n", (unsigned long long)off);
    out_flush();
    sys_exit(0);
}
