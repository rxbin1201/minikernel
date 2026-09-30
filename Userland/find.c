#include "util.h"

/* find [pfad...] [-name muster] [-iname muster] [-type f|d] [-maxdepth n]: sucht rekursiv nach Dateien und Verzeichnissen.
 * Muster mit * ? [abc] (in Anfuehrungszeichen setzen, damit die Shell sie nicht selbst ersetzt: find / -name "*.txt"). */
static const char *name_pat;
static int         name_fold, want_type, maxdepth = 1000, found;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int match_ci(const char *p, const char *s)
{
    char lp[256], ls[256];
    int i;
    for (i = 0; p[i] && i < 255; i++)
        lp[i] = (char)lower((unsigned char)p[i]);
    lp[i] = 0;
    for (i = 0; s[i] && i < 255; i++)
        ls[i] = (char)lower((unsigned char)s[i]);
    ls[i] = 0;
    return glob_match(lp, ls);
}

static void visit(const char *path, const char *name, int is_dir, int depth)
{
    int ok = 1;
    if (name_pat)
        ok = name_fold ? match_ci(name_pat, name) : glob_match(name_pat, name);
    if (want_type == 'f' && is_dir)
        ok = 0;
    if (want_type == 'd' && !is_dir)
        ok = 0;
    if (ok) {
        out_str(path);
        out_write("\n", 1);
        found++;
    }
    if (!is_dir || depth >= maxdepth)
        return;
    DirEnt ent;
    for (u64 i = 0; sys_readdir(path, i, &ent) == 0; i++) {
        char child[512];
        join_path(child, sizeof(child), path, ent.name);
        visit(child, ent.name, (int)ent.is_dir, depth + 1);
    }
}

void _start(int argc, char **argv)
{
    const char *paths[16];
    int np = 0;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-name") == 0 || strcmp(argv[i], "-iname") == 0) && i + 1 < argc) {
            name_fold = argv[i][1] == 'i';
            name_pat = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            want_type = argv[++i][0];
        } else if (strcmp(argv[i], "-maxdepth") == 0 && i + 1 < argc) {
            maxdepth = atoi(argv[++i]);
        } else if (argv[i][0] == '-') {
            fprintf(2, "Aufruf: find [pfad...] [-name muster] [-iname muster] [-type f|d] [-maxdepth n]\n");
            sys_exit(2);
        } else if (np < 16) {
            paths[np++] = argv[i];
        }
    }
    if (!np)
        paths[np++] = ".";
    int rc = 0;
    for (int k = 0; k < np; k++) {
        Stat st;
        if (sys_stat(paths[k], &st) != 0) {
            out_flush();
            fprintf(2, "find: '%s': nicht gefunden\n", paths[k]);
            rc = 1;
            continue;
        }
        const char *base = strrchr(paths[k], '/');
        visit(paths[k], base && base[1] ? base + 1 : paths[k], (int)st.is_dir, 0);
    }
    out_flush();
    sys_exit(rc);
}
