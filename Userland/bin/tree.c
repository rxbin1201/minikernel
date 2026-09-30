#include "util.h"

/* tree [-L tiefe] [-d] [pfad]: Verzeichnisbaum mit Linien. -d nur Verzeichnisse. */
static int maxdepth = 100, dirs_only, tty, ndirs, nfiles;

typedef struct {
    char name[256];
    int  is_dir;
} Ent;

static void walk(const char *path, const char *prefix, int depth)
{
    if (depth >= maxdepth)
        return;
    int n = 0;
    DirEnt de;
    while (sys_readdir(path, (u64)n, &de) == 0)
        n++;
    Ent *e = u_malloc(sizeof(Ent) * (u64)(n ? n : 1));
    if (!e)
        return;
    int k = 0;
    for (int i = 0; i < n && sys_readdir(path, (u64)i, &de) == 0; i++) {
        if (dirs_only && !de.is_dir)
            continue;
        memcpy(e[k].name, de.name, sizeof(e[k].name));
        e[k].is_dir = (int)de.is_dir;
        k++;
    }
    for (int i = 1; i < k; i++) { /* Verzeichnisse zuerst, dann alphabetisch */
        Ent x = e[i];
        int j = i - 1;
        while (j >= 0 && (e[j].is_dir < x.is_dir || (e[j].is_dir == x.is_dir && strcasecmp(e[j].name, x.name) > 0))) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = x;
    }
    for (int i = 0; i < k; i++) {
        int last = i == k - 1;
        out_str(prefix);
        out_str(last ? "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 "); /* └── ├── */
        if (e[i].is_dir && tty)
            out_str(C_BLUE);
        out_str(e[i].name);
        if (e[i].is_dir && tty)
            out_str(C_RESET);
        out_write("\n", 1);
        if (e[i].is_dir) {
            ndirs++;
            char child[512], pre[512];
            join_path(child, sizeof(child), path, e[i].name);
            snprintf(pre, sizeof(pre), "%s%s", prefix, last ? "    " : "\xE2\x94\x82   "); /* │ */
            walk(child, pre, depth + 1);
        } else {
            nfiles++;
        }
    }
    u_free(e);
}

void _start(int argc, char **argv)
{
    const char *path = ".";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0 && i + 1 < argc)
            maxdepth = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0)
            dirs_only = 1;
        else if (argv[i][0] == '-') {
            fprintf(2, "Aufruf: tree [-L tiefe] [-d] [pfad]\n");
            sys_exit(2);
        } else
            path = argv[i];
    }
    Stat st;
    if (sys_stat(path, &st) != 0 || !st.is_dir) {
        fprintf(2, "tree: '%s': kein Verzeichnis\n", path);
        sys_exit(1);
    }
    tty = sys_isatty(1) != 0;
    if (tty)
        out_str(C_BLUE);
    out_str(path);
    if (tty)
        out_str(C_RESET);
    out_write("\n", 1);
    walk(path, "", 0);
    out_printf("\n%d Verzeichnisse, %d Dateien\n", ndirs, nfiles);
    out_flush();
    sys_exit(0);
}
