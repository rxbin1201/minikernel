#include "libc.h"

/* wc [datei...]: Zeilen, Woerter und Bytes (ohne Argument von stdin) */
static u64 total_lines, total_words, total_bytes;

static int count(int fd, const char *name)
{
    char buf[512];
    u64 lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    s64 n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (s64 i = 0; i < n; i++) {
            char c = buf[i];
            bytes++;
            if (c == '\n')
                lines++;
            if (c == ' ' || c == '\n' || c == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    printf("%llu %llu %llu%s%s\n", (unsigned long long)lines, (unsigned long long)words, (unsigned long long)bytes,
           name ? " " : "", name ? name : "");
    total_lines += lines;
    total_words += words;
    total_bytes += bytes;
    return n < 0 ? 1 : 0;
}

void _start(int argc, char **argv)
{
    if (argc < 2)
        sys_exit(count(0, NULL));

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        s64 fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "wc: '%s': nicht gefunden\n", argv[i]);
            rc = 1;
            continue;
        }
        rc |= count((int)fd, argv[i]);
        sys_close((int)fd);
    }
    if (argc > 2)
        printf("%llu %llu %llu gesamt\n", (unsigned long long)total_lines, (unsigned long long)total_words,
               (unsigned long long)total_bytes);
    sys_exit(rc);
}
