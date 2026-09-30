#include "util.h"

/* sort [-r] [-n] [-f] [-u] [datei...]: Zeilen sortieren. -r umgekehrt, -n nach Zahlenwert, -f ohne Gross-/Kleinschreibung,
 * -u gleiche Zeilen nur einmal. Ohne Datei: stdin. */
static int reverse, numeric, fold, unique;

static s64 num(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    int neg = *s == '-';
    if (neg || *s == '+')
        s++;
    s64 v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static int cmp(const char *a, const char *b)
{
    int r;
    if (numeric) {
        s64 x = num(a), y = num(b);
        r = x < y ? -1 : x > y ? 1 : strcmp(a, b);
    } else {
        r = fold ? strcasecmp(a, b) : strcmp(a, b);
    }
    return reverse ? -r : r;
}

/* stabiles Mergesort */
static void msort(char **v, char **tmp, int n)
{
    if (n < 2)
        return;
    int h = n / 2;
    msort(v, tmp, h);
    msort(v + h, tmp, n - h);
    int i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = cmp(v[j], v[i]) < 0 ? v[j++] : v[i++];
    while (i < h)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, sizeof(char *) * (u64)n);
}

void _start(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r') reverse = 1;
            else if (*o == 'n') numeric = 1;
            else if (*o == 'f') fold = 1;
            else if (*o == 'u') unique = 1;
            else {
                fprintf(2, "Aufruf: sort [-r] [-n] [-f] [-u] [datei...]\n");
                sys_exit(2);
            }
        }
    }
    /* alle Eingaben aneinanderhaengen */
    char **all = 0;
    int total = 0;
    int nfiles = argc - i;
    for (int f = 0; f < (nfiles ? nfiles : 1); f++) {
        u64 len;
        char *buf = read_file_all("sort", nfiles ? argv[i + f] : 0, &len);
        if (!buf)
            sys_exit(1);
        int n;
        char **lines = split_lines(buf, len, &n);
        char **na = u_malloc(sizeof(char *) * (u64)(total + n + 1));
        if (!na || !lines) {
            fprintf(2, "sort: kein Speicher\n");
            sys_exit(1);
        }
        if (total)
            memcpy(na, all, sizeof(char *) * (u64)total);
        memcpy(na + total, lines, sizeof(char *) * (u64)n);
        u_free(all);
        all = na;
        total += n;
    }
    char **tmp = u_malloc(sizeof(char *) * (u64)(total + 1));
    if (!tmp && total) {
        fprintf(2, "sort: kein Speicher\n");
        sys_exit(1);
    }
    msort(all, tmp, total);
    for (int k = 0; k < total; k++) {
        if (unique && k > 0 && cmp(all[k], all[k - 1]) == 0)
            continue;
        out_str(all[k]);
        out_write("\n", 1);
    }
    out_flush();
    sys_exit(0);
}
