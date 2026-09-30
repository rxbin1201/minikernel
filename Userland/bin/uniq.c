#include "util.h"

/* uniq [-c] [-d] [-u] [-i] [datei]: aufeinanderfolgende gleiche Zeilen zusammenfassen (meist nach sort).
 * -c mit Anzahl, -d nur mehrfache, -u nur einmalige Zeilen, -i ohne Gross-/Kleinschreibung. */
void _start(int argc, char **argv)
{
    int count = 0, dups = 0, uniques = 0, fold = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'c') count = 1;
            else if (*o == 'd') dups = 1;
            else if (*o == 'u') uniques = 1;
            else if (*o == 'i') fold = 1;
            else {
                fprintf(2, "Aufruf: uniq [-c] [-d] [-u] [-i] [datei]\n");
                sys_exit(2);
            }
        }
    }
    u64 len;
    char *buf = read_file_all("uniq", i < argc ? argv[i] : 0, &len);
    if (!buf)
        sys_exit(1);
    int n;
    char **lines = split_lines(buf, len, &n);
    for (int k = 0; k < n;) {
        int j = k + 1;
        while (j < n && (fold ? strcasecmp(lines[j], lines[k]) : strcmp(lines[j], lines[k])) == 0)
            j++;
        int c = j - k;
        if ((!dups || c > 1) && (!uniques || c == 1)) {
            if (count)
                out_printf("%7d ", c);
            out_str(lines[k]);
            out_write("\n", 1);
        }
        k = j;
    }
    out_flush();
    sys_exit(0);
}
