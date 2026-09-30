#include "util.h"

/* df [-b]: Groesse, belegter und freier Platz aller Datentraeger (-b: genaue Bytes) */
void _start(int argc, char **argv)
{
    int bytes = argc > 1 && strcmp(argv[1], "-b") == 0;
    int tty = sys_isatty(1) != 0;
    out_printf("%s%-14s %-8s %9s %9s %9s %5s  %s%s\n", tty ? C_BOLD : "", "Verzeichnis", "Typ", "Groesse", "Belegt", "Frei",
               "Bel%", "Name", tty ? C_RESET : "");
    MountInfo m;
    for (u64 i = 0; sys_mountinfo(i, &m) == 0; i++) {
        if (m.flags & 2) /* nicht unterstuetzt */
            continue;
        u64 st[2];
        if (sys_statfs(m.point, st) != 0)
            continue;
        u64 used = st[0] - st[1];
        int pct = st[0] ? (int)(used * 100 / st[0]) : 0;
        char a[24], b[24], c[24];
        if (bytes) {
            snprintf(a, sizeof(a), "%llu", (unsigned long long)st[0]);
            snprintf(b, sizeof(b), "%llu", (unsigned long long)used);
            snprintf(c, sizeof(c), "%llu", (unsigned long long)st[1]);
        } else {
            fmt_size(st[0], a, sizeof(a));
            fmt_size(used, b, sizeof(b));
            fmt_size(st[1], c, sizeof(c));
        }
        const char *col = !tty ? "" : pct >= 90 ? C_RED : pct >= 70 ? C_YELLOW : C_GREEN;
        out_printf("%s%-14s%s %-8s %9s %9s %9s %s%4d%%%s  %s\n", tty ? C_BLUE : "", m.point, tty ? C_RESET : "", m.fstype, a,
                   b, c, col, pct, tty ? C_RESET : "", m.label[0] ? m.label : "-");
    }
    out_flush();
    sys_exit(0);
}
