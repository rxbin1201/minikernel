#include "libc.h"
#include "malloc.h"

/* ls [-l] [-1] [pfad...]: Verzeichnisse auflisten. Auf dem Terminal farbig und in Spalten (Verzeichnisse zuerst),
 * in Pipes und Dateien schlicht: ein Eintrag pro Zeile. -l zeigt die Groesse, -1 erzwingt eine Zeile pro Eintrag. */
static int long_format, one_per_line, tty, term_width = 80;

typedef struct {
    char       *name; /* bei Verzeichnissen mit '/' am Ende */
    u64         size;
    u64         mtime;
    int         is_dir;
    const char *color;
} Entry;

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

static int is_elf(const char *path)
{
    s64 fd = sys_open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    unsigned char m[4];
    int ok = sys_read((int)fd, m, 4) == 4 && m[0] == 0x7F && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
    sys_close((int)fd);
    return ok;
}

/* Farbe nach Art: Verzeichnisse blau, Programme/Skripte gruen, Archive rot, Bilder magenta, Quelltext cyan */
static const char *color_for(const char *dir, const char *name, int is_dir)
{
    if (!tty)
        return "";
    if (is_dir)
        return C_BLUE;
    static const char *const images[] = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", 0};
    static const char *const archives[] = {".tar", ".zip", ".gz", ".xz", ".7z", ".img", ".iso", 0};
    static const char *const sources[] = {".c", ".h", ".py", ".s", ".asm", ".json", ".md", 0};
    for (int i = 0; images[i]; i++)
        if (ends_with(name, images[i]))
            return C_MAGENTA;
    for (int i = 0; archives[i]; i++)
        if (ends_with(name, archives[i]))
            return C_RED;
    for (int i = 0; sources[i]; i++)
        if (ends_with(name, sources[i]))
            return C_CYAN;
    if (ends_with(name, ".sh"))
        return C_GREEN;
    if (ends_with(name, ".txt"))
        return "";
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return is_elf(path) ? C_GREEN : "";
}

static int entry_cmp(const Entry *a, const Entry *b)
{
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1; /* Verzeichnisse zuerst */
    return strcasecmp(a->name, b->name);
}

static void sort_entries(Entry *e, int n)
{
    for (int i = 1; i < n; i++) { /* Einfuegesortieren: Verzeichnisse sind klein */
        Entry x = e[i];
        int j = i - 1;
        while (j >= 0 && entry_cmp(&e[j], &x) > 0) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = x;
    }
}

/* "2026-09-29 15:04" bzw. Striche, wenn die Zeit unbekannt ist (z.B. Dateien der initrd) */
static void print_time(u64 mtime)
{
    if (!mtime) {
        printf("       -        ");
        return;
    }
    DateTime dt;
    time_to_date(mtime, &dt);
    printf("%s%04d-%02d-%02d %02d:%02d%s", tty ? C_DIM : "", dt.year, dt.month, dt.day, dt.hour, dt.min, tty ? C_RESET : "");
}

static void print_size(u64 size)
{
    if (tty)
        printf(C_YELLOW "%10llu" C_RESET, (unsigned long long)size);
    else
        printf("%10llu", (unsigned long long)size);
}

static int list(const char *path, int header)
{
    Stat st;
    s64 r = sys_stat(path, &st);
    if (r < 0) {
        fprintf(2, "%sls: '%s': nicht gefunden (Fehler %d)%s\n", tty ? C_RED : "", path, (int)r, tty ? C_RESET : "");
        return 1;
    }
    if (!st.is_dir) {
        const char *c = color_for(".", path, 0);
        if (long_format) {
            printf("- ");
            print_time(st.mtime);
            printf(" ");
            print_size(st.size);
            printf("  %s%s%s\n", c, path, c[0] ? C_RESET : "");
        } else {
            printf("%s%s%s\n", c, path, c[0] ? C_RESET : "");
        }
        return 0;
    }
    if (header)
        printf("%s%s:%s\n", tty ? C_BOLD : "", path, tty ? C_RESET : "");

    int n = 0;
    DirEnt ent;
    while (sys_readdir(path, (u64)n, &ent) == 0)
        n++;
    if (!n)
        return 0;
    Entry *e = u_malloc(sizeof(Entry) * (u64)n);
    if (!e) {
        fprintf(2, "ls: kein Speicher\n");
        return 1;
    }
    int got = 0;
    for (int i = 0; i < n && sys_readdir(path, (u64)i, &ent) == 0; i++) {
        size_t l = strlen(ent.name);
        char *name = u_malloc(l + 2);
        if (!name)
            break;
        memcpy(name, ent.name, l + 1);
        e[got].color = color_for(path, ent.name, (int)ent.is_dir);
        if (ent.is_dir) {
            name[l] = '/';
            name[l + 1] = 0;
        }
        e[got].name = name;
        e[got].size = ent.size;
        e[got].mtime = ent.mtime;
        e[got].is_dir = (int)ent.is_dir;
        got++;
    }
    sort_entries(e, got);

    if (long_format || !tty || one_per_line) {
        for (int i = 0; i < got; i++) {
            if (long_format) {
                printf("%s ", e[i].is_dir ? (tty ? C_BLUE "d" C_RESET : "d") : "-");
                print_time(e[i].mtime);
                printf(" ");
                print_size(e[i].size);
                printf("  ");
            }
            printf("%s%s%s\n", e[i].color, e[i].name, e[i].color[0] ? C_RESET : "");
        }
    } else {
        const char **names = u_malloc(sizeof(char *) * (u64)got), **colors = u_malloc(sizeof(char *) * (u64)got);
        if (names && colors) {
            for (int i = 0; i < got; i++) {
                names[i] = e[i].name;
                colors[i] = e[i].color;
            }
            print_columns(names, colors, got, term_width);
        }
        u_free((void *)names);
        u_free((void *)colors);
    }
    for (int i = 0; i < got; i++)
        u_free(e[i].name);
    u_free(e);
    return 0;
}

void _start(int argc, char **argv)
{
    s64 t = sys_isatty(1);
    tty = t != 0;
    if (tty && (t >> 16) > 0)
        term_width = (int)(t >> 16);

    int first = 1, rc = 0;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (const char *o = argv[first] + 1; *o; o++) {
            if (*o == 'l')
                long_format = 1;
            else if (*o == '1')
                one_per_line = 1;
        }
    }
    if (first >= argc)
        sys_exit(list(".", 0));

    int many = argc - first > 1;
    for (int i = first; i < argc; i++) {
        if (many && i > first)
            printf("\n");
        rc |= list(argv[i], many);
    }
    sys_exit(rc);
}
