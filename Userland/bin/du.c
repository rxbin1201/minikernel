#include "util.h"

/* du [-s] [-b] [-a] [pfad...]: Platzbedarf von Verzeichnissen (Summe der Dateigroessen).
 * -s nur die Summe je Argument, -a auch jede Datei, -b genaue Bytes statt 1,5K/3M. */
static int summary, bytes, all;

static void show(u64 size, const char *path)
{
    char s[32];
    if (bytes)
        snprintf(s, sizeof(s), "%llu", (unsigned long long)size);
    else
        fmt_size(size, s, sizeof(s));
    out_printf("%-8s %s\n", s, path);
}

static u64 walk(const char *path, int depth)
{
    u64 total = 0;
    DirEnt ent;
    for (u64 i = 0; sys_readdir(path, i, &ent) == 0; i++) {
        char child[PATH_MAX];
        join_path(child, sizeof(child), path, ent.name);
        if (ent.is_dir) {
            total += walk(child, depth + 1);
        } else {
            total += ent.size;
            if (all && !summary)
                show(ent.size, child);
        }
    }
    if (!summary || depth == 0)
        show(total, path);
    return total;
}

void _start(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 's') summary = 1;
            else if (*o == 'b') bytes = 1;
            else if (*o == 'a') all = 1;
            else if (*o == 'h') ;
            else {
                fprintf(2, "Aufruf: du [-s] [-a] [-b] [pfad...]\n");
                sys_exit(2);
            }
        }
    }
    const char *def[1] = {"."};
    const char **paths = i < argc ? (const char **)argv + i : def;
    int n = i < argc ? argc - i : 1, rc = 0;
    for (int k = 0; k < n; k++) {
        Stat st;
        if (sys_stat(paths[k], &st) != 0) {
            out_flush();
            fprintf(2, "du: '%s': nicht gefunden\n", paths[k]);
            rc = 1;
            continue;
        }
        if (st.is_dir)
            walk(paths[k], 0);
        else
            show(st.size, paths[k]);
    }
    out_flush();
    sys_exit(rc);
}
