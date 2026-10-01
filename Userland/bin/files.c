#include "gfx.h"
#include "malloc.h"
#include "ui.h"

/* files [ordner]: Dateien verwalten (Fenster mit aenderbarer Groesse).
 *   Links die Seitenleiste: Schnellzugriff (Ordner der Platte), die Orte (Platte /disk, System /, angesteckte
 *   Datentraeger) mit aufklappbaren Unterordnern, unten der belegte Speicher. Rechts Tabs (Strg+T, Strg+W,
 *   Strg+Tab), die Menuezeile (Datei, Bearbeiten, Ansicht), Zurueck/Vor/Hoch, die Pfadleiste (jeder Teil anklickbar)
 *   und die Suche (Strg+F: filtert den Ordner; im Schnellzugriff sucht sie auf der ganzen Platte).
 *   Ohne Ordner beginnt es im Schnellzugriff: grosse Ordner der Platte und die zuletzt geaenderten Dateien.
 *   Doppelklick oder Enter oeffnet: Ordner hier, Dateien im passenden Programm (der Desktop
 *   waehlt: Bilder in der Bildansicht, sonst der Texteditor). Ruecktaste: eine Ebene hoeher.
 *   Auswahl: Klick, Strg+Klick (dazu/weg), Shift+Klick (Bereich), Pfeile (mit Shift erweitern), Strg+A.
 *   Strg+C/X/V kopieren, ausschneiden, einfuegen (ueber die Zwischenablage, also auch zwischen zwei Fenstern),
 *   Strg+D duplizieren, Strg+N neuer Ordner, Entf loeschen (mit Rueckfrage). Rechtsklick: Kontextmenue.
 *   Ziehen auf einen Ordner oder Ort verschiebt dorthin, mit Strg beim Loslassen wird kopiert.
 *   Zwischen Datentraegern wird kopiert und danach geloescht. */

typedef struct {
    char name[256];
    int  is_dir, sel;
    u64  size, mtime;
} Ent;

#define MAXHIST   16
#define MAXPLACES 10

static char dir[256] = "/disk";
static Ent *ents;
static int  nent, scroll, cur = -1, anchor = -1, focus = 1;
static char back_hist[MAXHIST][256], fwd_hist[MAXHIST][256];
static int  nback, nfwd;
static char status[200];
static s64  status_t;
static s64  free_bytes = -1; /* freier Platz des Datentraegers (-1 = unbekannt), beim Laden bestimmt */
static u64  total_bytes;      /* Groesse des Datentraegers (0 = unbekannt) */
static char query[64];        /* Suche: filtert den Ordner bzw. sucht im Schnellzugriff auf der Platte */
static int  search_focus, sort_mode; /* sort_mode: 0 Name, 1 Datum (neueste zuerst), 2 Groesse */

static struct {
    char label[40], path[64];
    int  kind; /* 0 Platte, 1 System, 2 Datentraeger */
} places[MAXPLACES];
static int nplaces;

/* Rueckfragen */
enum { D_NONE, D_RENAME, D_NEWFOLDER, D_DELETE, D_ERROR };
static int  dialog, dialog_hover = -1;
static char field[256], dialog_msg[200];
static int  flen;

/* Kontextmenue */
enum { M_OPEN = 1, M_RENAME, M_DUP, M_COPY, M_CUT, M_PASTE, M_DELETE, M_NEWFOLDER, M_NEWFILE, M_OPENTAB, M_NEWTAB,
       M_CLOSETAB, M_SELALL, M_HOME, M_REFRESH, M_SORT_NAME, M_SORT_DATE, M_SORT_SIZE };
static struct {
    const char *label, *keys;
    int         action, enabled, checked;
} menu[16];
static int nmenu, menu_x, menu_y, menu_hover = -1;

/* Ziehen (drop_place: Knoten der Seitenleiste) */
static int drag_pending, drag_active, drag_x, drag_y, press_x, press_y, drop_row = -1, drop_place = -1;

/* weiter unten */
static void dir_title(const char *d, char *out, int max);
static void home_enter(void);
static void home_scan(int force);
static void load_quick(void);
static void build_side(void);
static void tab_new(const char *d);
static void tab_close_current(void);
static int  contains_ci(const char *s, const char *q);

/* ---------------------------------------------------------------------------------------------------------------------
 * Hilfen
 * ------------------------------------------------------------------------------------------------------------------- */

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static int join(char *out, size_t max, const char *d, const char *n)
{
    int len = snprintf(out, max, "%s%s%s", d, strcmp(d, "/") == 0 ? "" : "/", n);
    return len < 0 || (size_t)len >= max ? -1 : 0;
}

static const char *base_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void parent_of(const char *p, char *out, size_t max)
{
    snprintf(out, max, "%s", p);
    char *s = strrchr(out, '/');
    if (s && s != out)
        *s = 0;
    else
        snprintf(out, max, "/");
}

static int exists(const char *p)
{
    Stat st;
    return sys_stat(p, &st) == 0;
}

static int inside(const char *p, const char *parent) /* p liegt in parent (oder ist es) */
{
    size_t n = strlen(parent);
    return strncmp(p, parent, n) == 0 && (p[n] == 0 || p[n] == '/' || strcmp(parent, "/") == 0);
}

static void fmt_size(u64 b, char *out, size_t max)
{
    if (b < 1024)
        snprintf(out, max, "%llu Byte", (unsigned long long)b);
    else if (b < 1024 * 1024)
        snprintf(out, max, "%llu KB", (unsigned long long)((b + 512) / 1024));
    else if (b < (1ULL << 30))
        snprintf(out, max, "%llu,%llu MB", (unsigned long long)(b >> 20), (unsigned long long)((b % (1 << 20)) * 10 >> 20));
    else
        snprintf(out, max, "%llu,%llu GB", (unsigned long long)(b >> 30), (unsigned long long)((b % (1ULL << 30)) * 10 >> 30));
}

static void say(const char *fmt, const char *arg)
{
    snprintf(status, sizeof(status), fmt, arg);
    status_t = sys_time_us();
}

static const char *errtext(s64 r)
{
    static char buf[48];
    switch (r) {
    case ERR_ROFS: return "Dieser Ort ist schreibgesch\xC3\xBCtzt";
    case ERR_EXIST: return "Es gibt schon ein Objekt mit diesem Namen";
    case ERR_NOSPC: return "Kein Platz mehr auf dem Datentr\xC3\xA4ger";
    case ERR_NOTEMPTY: return "Der Ordner ist nicht leer";
    case ERR_NOENT: return "Nicht gefunden";
    case ERR_INVAL: return "Das geht hier nicht";
    default: snprintf(buf, sizeof(buf), "Fehler %lld", (long long)r); return buf;
    }
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Dateioperationen
 * ------------------------------------------------------------------------------------------------------------------- */

static s64 copy_file(const char *src, const char *dst)
{
    s64 in = sys_open(src, O_RDONLY);
    if (in < 0)
        return in;
    s64 out = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        sys_close((int)in);
        return out;
    }
    char *buf = u_malloc(65536);
    s64 r, rc = 0;
    while ((r = sys_read((int)in, buf, 65536)) > 0)
        if ((rc = write_all((int)out, buf, (size_t)r)) != 0)
            break;
    if (r < 0)
        rc = r;
    u_free(buf);
    sys_close((int)in);
    sys_close((int)out);
    return rc;
}

/* Namen eines Ordners (u_free der Liste durch den Aufrufer) */
static int list_names(const char *d, char (**names)[256])
{
    int cap = 16, n = 0;
    char (*v)[256] = u_malloc(sizeof(*v) * (u64)cap);
    DirEnt de;
    for (u64 i = 0; sys_readdir(d, i, &de) == 0; i++) {
        if (n == cap) {
            char (*nv)[256] = u_malloc(sizeof(*v) * (u64)cap * 2);
            memcpy(nv, v, sizeof(*v) * (u64)n);
            u_free(v);
            v = nv;
            cap *= 2;
        }
        snprintf(v[n++], 256, "%s", de.name);
    }
    *names = v;
    return n;
}

static s64 copy_tree(const char *src, const char *dst, int depth)
{
    Stat st;
    s64 r = sys_stat(src, &st);
    if (r != 0)
        return r;
    if (!st.is_dir)
        return copy_file(src, dst);
    if (depth > 16 || inside(dst, src))
        return ERR_INVAL; /* nicht in sich selbst kopieren */
    if ((r = sys_mkdir(dst)) != 0)
        return r;
    char (*names)[256];
    int n = list_names(src, &names);
    for (int i = 0; i < n && r == 0; i++) {
        char a[512], b[512];
        if (join(a, sizeof(a), src, names[i]) || join(b, sizeof(b), dst, names[i]))
            r = ERR_INVAL;
        else
            r = copy_tree(a, b, depth + 1);
    }
    u_free(names);
    return r;
}

static s64 delete_tree(const char *p, int depth)
{
    Stat st;
    s64 r = sys_stat(p, &st);
    if (r != 0)
        return r;
    if (st.is_dir && depth < 16) {
        char (*names)[256];
        int n = list_names(p, &names);
        for (int i = 0; i < n && r == 0; i++) {
            char a[512];
            r = join(a, sizeof(a), p, names[i]) ? ERR_INVAL : delete_tree(a, depth + 1);
        }
        u_free(names);
        if (r != 0)
            return r;
    }
    return sys_unlink(p);
}

/* Verschieben: auf demselben Datentraeger umbenennen, sonst kopieren und loeschen */
static s64 move_path(const char *src, const char *dst)
{
    if (inside(dst, src))
        return ERR_INVAL;
    s64 r = sys_rename(src, dst);
    if (r == ERR_INVAL) {
        r = copy_tree(src, dst, 0);
        if (r == 0)
            r = delete_tree(src, 0);
    }
    return r;
}

/* freier Name im Ordner d: "name", sonst "name Kopie.ext", "name Kopie 2.ext" ... */
static void unique(const char *d, const char *name, int copy, char *out, size_t max)
{
    char p[512];
    if (!copy && join(p, sizeof(p), d, name) == 0 && !exists(p)) {
        snprintf(out, max, "%s", name);
        return;
    }
    char stem[256], ext[64] = "";
    snprintf(stem, sizeof(stem), "%s", name);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem && strlen(dot) < sizeof(ext)) {
        snprintf(ext, sizeof(ext), "%s", dot);
        *dot = 0;
    }
    for (int i = 1; i < 1000; i++) {
        char cand[256];
        if (i == 1)
            snprintf(cand, sizeof(cand), "%s Kopie%s", stem, ext);
        else
            snprintf(cand, sizeof(cand), "%s Kopie %d%s", stem, i, ext);
        if (join(p, sizeof(p), d, cand) == 0 && !exists(p)) {
            snprintf(out, max, "%s", cand);
            return;
        }
    }
    snprintf(out, max, "%s", name);
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Ordner laden, Orte
 * ------------------------------------------------------------------------------------------------------------------- */

static int nsel(void)
{
    int n = 0;
    for (int i = 0; i < nent; i++)
        n += ents[i].sel;
    return n;
}

static void load_places(void)
{
    nplaces = 0;
    if (exists("/disk")) {
        snprintf(places[nplaces].label, 40, "Platte");
        snprintf(places[nplaces].path, 64, "/disk");
        places[nplaces++].kind = 0;
    }
    snprintf(places[nplaces].label, 40, "System");
    snprintf(places[nplaces].path, 64, "/");
    places[nplaces++].kind = 1;
    MountInfo mi;
    for (u64 i = 0; nplaces < MAXPLACES && sys_mountinfo(i, &mi) == 0; i++) {
        if (strncmp(mi.point, "/mnt/", 5) != 0 || (mi.flags & 2))
            continue;
        snprintf(places[nplaces].label, 40, "%s", mi.label[0] ? mi.label : mi.device);
        snprintf(places[nplaces].path, 64, "%s", mi.point);
        places[nplaces++].kind = 2;
    }
}

static int cmp_ent(const Ent *a, const Ent *b) /* Ordner zuerst, dann nach sort_mode, sonst nach Namen */
{
    if (a->is_dir != b->is_dir)
        return b->is_dir - a->is_dir;
    if (sort_mode == 1 && a->mtime != b->mtime)
        return a->mtime > b->mtime ? -1 : 1;
    if (sort_mode == 2 && a->size != b->size)
        return a->size > b->size ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

/* Ordner (neu) lesen; keep = Auswahl nach Namen behalten */
static void load(int keep)
{
    char (*selnames)[256] = 0;
    int ns = 0;
    if (keep && nsel()) {
        selnames = u_malloc(sizeof(*selnames) * (u64)nsel());
        for (int i = 0; i < nent; i++)
            if (ents[i].sel)
                snprintf(selnames[ns++], 256, "%s", ents[i].name);
    }
    char curname[256] = "";
    if (keep && cur >= 0 && cur < nent)
        snprintf(curname, sizeof(curname), "%s", ents[cur].name);
    u_free(ents);
    ents = 0;
    nent = 0;
    int cap = 64;
    Ent *e = u_malloc(sizeof(Ent) * (u64)cap);
    DirEnt de;
    for (u64 i = 0; sys_readdir(dir, i, &de) == 0; i++) {
        if (query[0] && !contains_ci(de.name, query))
            continue;
        if (nent == cap) {
            Ent *ne = u_malloc(sizeof(Ent) * (u64)cap * 2);
            memcpy(ne, e, sizeof(Ent) * (u64)nent);
            u_free(e);
            e = ne;
            cap *= 2;
        }
        snprintf(e[nent].name, sizeof(e[nent].name), "%s", de.name);
        e[nent].is_dir = (int)de.is_dir;
        e[nent].size = de.size;
        e[nent].mtime = de.mtime;
        e[nent].sel = 0;
        nent++;
    }
    for (int i = 1; i < nent; i++) { /* Einfuegesortieren */
        Ent x = e[i];
        int j = i - 1;
        while (j >= 0 && cmp_ent(&e[j], &x) > 0) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = x;
    }
    ents = e;
    u64 fsz[2];
    int sok = sys_statfs(dir, fsz) == 0;
    free_bytes = sok ? (s64)fsz[1] : -1;
    total_bytes = sok ? fsz[0] : 0;
    cur = anchor = -1;
    for (int i = 0; i < nent; i++) {
        for (int k = 0; k < ns; k++)
            if (strcmp(ents[i].name, selnames[k]) == 0)
                ents[i].sel = 1;
        if (curname[0] && strcmp(ents[i].name, curname) == 0)
            cur = anchor = i;
    }
    u_free(selnames);
    if (!keep)
        scroll = 0;
    char title[64];
    dir_title(dir, title, sizeof(title));
    gfx_set_title(title);
}

/* p "" = Schnellzugriff */
static void go(const char *p, int remember)
{
    Stat st;
    if (p[0] && (sys_stat(p, &st) != 0 || !st.is_dir)) {
        say("Nicht gefunden: %s", p);
        return;
    }
    if (remember && strcmp(p, dir) != 0) {
        if (nback == MAXHIST) {
            memmove(back_hist, back_hist + 1, sizeof(back_hist[0]) * (MAXHIST - 1));
            nback--;
        }
        snprintf(back_hist[nback++], 256, "%s", dir);
        nfwd = 0;
    }
    snprintf(dir, sizeof(dir), "%s", p);
    status[0] = 0;
    query[0] = 0;
    search_focus = 0;
    if (dir[0])
        load(0);
    else
        home_enter();
}

static void go_back(void)
{
    if (!nback)
        return;
    snprintf(fwd_hist[nfwd < MAXHIST ? nfwd++ : MAXHIST - 1], 256, "%s", dir);
    char p[256];
    snprintf(p, sizeof(p), "%s", back_hist[--nback]);
    go(p, 0);
}

static void go_forward(void)
{
    if (!nfwd)
        return;
    snprintf(back_hist[nback < MAXHIST ? nback++ : MAXHIST - 1], 256, "%s", dir);
    char p[256];
    snprintf(p, sizeof(p), "%s", fwd_hist[--nfwd]);
    go(p, 0);
}

static void go_up(void)
{
    if (!dir[0] || strcmp(dir, "/") == 0)
        return;
    char p[256], from[256];
    snprintf(from, sizeof(from), "%s", base_of(dir));
    parent_of(dir, p, sizeof(p));
    go(p, 1);
    for (int i = 0; i < nent; i++) /* den Ordner, aus dem man kommt, auswaehlen */
        if (strcmp(ents[i].name, from) == 0) {
            ents[i].sel = 1;
            cur = anchor = i;
        }
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Aktionen
 * ------------------------------------------------------------------------------------------------------------------- */

static void busy(const char *what);

static void open_entry(int i)
{
    if (i < 0 || i >= nent)
        return;
    char p[512];
    if (join(p, sizeof(p), dir, ents[i].name))
        return;
    if (ents[i].is_dir)
        go(p, 1);
    else if (gfx_desktop_open(p) == 0)
        say("%s wird ge\xC3\xB6" "ffnet", ents[i].name);
    else
        say("%s kann ohne Desktop nicht ge\xC3\xB6" "ffnet werden", ents[i].name);
}

static void select_only(int i)
{
    for (int k = 0; k < nent; k++)
        ents[k].sel = k == i;
    cur = anchor = i;
}

static void select_range(int a, int b)
{
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    for (int k = 0; k < nent; k++)
        ents[k].sel = k >= a && k <= b;
}

/* Zwischenablage: eine Zeile je Pfad; beim Ausschneiden steht "#verschieben" davor */
static void clip_put(int cut)
{
    int n = nsel();
    if (!n)
        return;
    u64 cap = 16 + (u64)n * 520, len = 0;
    char *t = u_malloc(cap);
    if (cut)
        len += (u64)snprintf(t, cap, "#verschieben\n");
    for (int i = 0; i < nent; i++)
        if (ents[i].sel) {
            char p[512];
            if (join(p, sizeof(p), dir, ents[i].name) == 0)
                len += (u64)snprintf(t + len, cap - len, "%s\n", p);
        }
    sys_clipboard_set(t, len);
    u_free(t);
    char m[64];
    snprintf(m, sizeof(m), "%d %s", n, n == 1 ? "Objekt" : "Objekte");
    say(cut ? "%s ausgeschnitten - Strg+V legt sie hier ab, wo du einf\xC3\xBCgst" : "%s kopiert", m);
}

/* Liste der Pfade aus der Zwischenablage; 0 = keine Dateien darin */
static int clip_paths(char **text, int *cut)
{
    s64 n = sys_clipboard_get(0, 0);
    if (n <= 0)
        return 0;
    char *t = u_malloc((u64)n + 1);
    n = sys_clipboard_get(t, (u64)n);
    t[n] = 0;
    *cut = strncmp(t, "#verschieben\n", 13) == 0;
    int count = 0;
    char *p = t + (*cut ? 13 : 0);
    while (*p) {
        char *e = strchr(p, '\n');
        if (e)
            *e = 0;
        if (p[0] == '/' && exists(p))
            count++;
        else if (p[0]) {
            u_free(t);
            return 0; /* das ist Text, keine Dateiliste */
        }
        if (!e)
            break;
        p = e + 1;
    }
    *text = t;
    return count;
}

/* Dateien/Ordner (Pfade, durch 0 getrennt, n Stueck ab p) nach target: verschieben oder kopieren */
static void transfer(const char *p, int n, const char *target, int move)
{
    busy(move ? "Verschiebe \xE2\x80\xA6" : "Kopiere \xE2\x80\xA6");
    int done = 0;
    s64 err = 0;
    for (int i = 0; i < n; i++, p += strlen(p) + 1) {
        while (!*p)
            p++;
        char parent[256], name[256], dst[512];
        parent_of(p, parent, sizeof(parent));
        if (move && strcmp(parent, target) == 0)
            continue; /* liegt schon dort */
        unique(target, base_of(p), !move && strcmp(parent, target) == 0, name, sizeof(name));
        if (join(dst, sizeof(dst), target, name)) {
            err = ERR_INVAL;
            continue;
        }
        s64 r = move ? move_path(p, dst) : copy_tree(p, dst, 0);
        if (r == 0)
            done++;
        else
            err = r;
    }
    load(1);
    if (err) {
        say("%s", errtext(err));
    } else {
        char m[64];
        snprintf(m, sizeof(m), "%d %s %s", done, done == 1 ? "Objekt" : "Objekte", move ? "verschoben" : "kopiert");
        say("%s", m);
    }
}

static void paste(void)
{
    char *t;
    int cut, n = clip_paths(&t, &cut);
    if (!n) {
        say("%s", "Keine Dateien in der Zwischenablage");
        return;
    }
    char *p = t + (cut ? 13 : 0);
    for (char *q = p; *q; q++) /* Zeilen -> durch 0 getrennt */
        if (*q == '\n')
            *q = 0;
    transfer(p, n, dir, cut);
    if (cut)
        sys_clipboard_set("", 0); /* ausgeschnittenes nur einmal einfuegen */
    u_free(t);
}

/* ausgewaehlte Objekte als Liste (durch 0 getrennt) */
static char *selected_paths(int *n)
{
    *n = nsel();
    char *t = u_malloc((u64)*n * 520 + 1), *p = t;
    for (int i = 0; i < nent; i++)
        if (ents[i].sel && join(p, 512, dir, ents[i].name) == 0)
            p += strlen(p) + 1;
    *p = 0;
    return t;
}

static void duplicate(void)
{
    int n;
    char *t = selected_paths(&n);
    if (n)
        transfer(t, n, dir, 0);
    u_free(t);
}

static void drop_on(const char *target, int copy)
{
    int n;
    char *t = selected_paths(&n);
    if (n)
        transfer(t, n, target, !copy);
    u_free(t);
}

static void new_textfile(void)
{
    char name[256], p[512];
    unique(dir, "Neue Datei.txt", 0, name, sizeof(name));
    if (join(p, sizeof(p), dir, name))
        return;
    s64 fd = sys_open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        say("%s", errtext(fd));
        return;
    }
    sys_close((int)fd);
    load(0);
    for (int i = 0; i < nent; i++)
        if (strcmp(ents[i].name, name) == 0)
            select_only(i);
    gfx_desktop_open(p);
}

static void open_dialog(int kind)
{
    dialog = kind;
    dialog_hover = -1;
    field[0] = 0;
    if (kind == D_RENAME) {
        for (int i = 0; i < nent; i++)
            if (ents[i].sel) {
                snprintf(field, sizeof(field), "%s", ents[i].name);
                break;
            }
    } else if (kind == D_NEWFOLDER) {
        unique(dir, "Neuer Ordner", 0, field, sizeof(field));
    } else if (kind == D_DELETE) {
        int n = nsel();
        if (n == 1) {
            for (int i = 0; i < nent; i++)
                if (ents[i].sel)
                    snprintf(dialog_msg, sizeof(dialog_msg), "\xE2\x80\x9E%s\xE2\x80\x9C l\xC3\xB6schen?", ents[i].name);
        } else {
            snprintf(dialog_msg, sizeof(dialog_msg), "%d Objekte l\xC3\xB6schen?", n);
        }
    }
    flen = (int)strlen(field);
}

static void dialog_ok(void)
{
    int kind = dialog;
    dialog = D_NONE;
    if (kind == D_RENAME) {
        int i = 0;
        while (i < nent && !ents[i].sel)
            i++;
        if (i == nent || !field[0] || strchr(field, '/') || strcmp(field, ents[i].name) == 0)
            return;
        char a[512], b[512];
        if (join(a, sizeof(a), dir, ents[i].name) || join(b, sizeof(b), dir, field))
            return;
        s64 r = exists(b) && strcasecmp(field, ents[i].name) != 0 ? ERR_EXIST : sys_rename(a, b);
        if (r != 0) {
            say("%s", errtext(r));
            return;
        }
        load(0);
        for (int k = 0; k < nent; k++)
            if (strcmp(ents[k].name, field) == 0)
                select_only(k);
    } else if (kind == D_NEWFOLDER) {
        char p[512];
        if (!field[0] || strchr(field, '/') || join(p, sizeof(p), dir, field))
            return;
        s64 r = exists(p) ? ERR_EXIST : sys_mkdir(p);
        if (r != 0) {
            say("%s", errtext(r));
            return;
        }
        load(0);
        for (int k = 0; k < nent; k++)
            if (strcmp(ents[k].name, field) == 0)
                select_only(k);
    } else if (kind == D_DELETE) {
        busy("L\xC3\xB6sche \xE2\x80\xA6");
        int n, done = 0;
        s64 err = 0;
        char *t = selected_paths(&n), *p = t;
        for (int i = 0; i < n; i++, p += strlen(p) + 1) {
            s64 r = delete_tree(p, 0);
            if (r == 0)
                done++;
            else
                err = r;
        }
        u_free(t);
        load(0);
        if (err) {
            say("%s", errtext(err));
        } else {
            char m[64];
            snprintf(m, sizeof(m), "%d %s gel\xC3\xB6scht", done, done == 1 ? "Objekt" : "Objekte");
            say("%s", m);
        }
    }
}

static void action(int a)
{
    if (!dir[0] && (a == M_NEWFOLDER || a == M_NEWFILE || a == M_PASTE)) {
        say("%s", "Im Schnellzugriff geht das nicht \xE2\x80\x93 erst einen Ordner \xC3\xB6" "ffnen");
        return;
    }
    switch (a) {
    case M_OPEN:
        for (int i = 0; i < nent; i++)
            if (ents[i].sel) {
                open_entry(i);
                if (ents[i].is_dir)
                    break; /* der Ordner ersetzt die Liste */
            }
        break;
    case M_RENAME: if (nsel() == 1) open_dialog(D_RENAME); break;
    case M_DUP: duplicate(); break;
    case M_COPY: clip_put(0); break;
    case M_CUT: clip_put(1); break;
    case M_PASTE: paste(); break;
    case M_DELETE: if (nsel()) open_dialog(D_DELETE); break;
    case M_NEWFOLDER: open_dialog(D_NEWFOLDER); break;
    case M_NEWFILE: new_textfile(); break;
    case M_OPENTAB:
        if (cur >= 0 && cur < nent && ents[cur].is_dir) {
            char p[512];
            if (!join(p, sizeof(p), dir, ents[cur].name))
                tab_new(p);
        }
        break;
    case M_NEWTAB: tab_new(""); break;
    case M_CLOSETAB: tab_close_current(); break;
    case M_SELALL:
        for (int i = 0; i < nent; i++)
            ents[i].sel = 1;
        break;
    case M_HOME: if (dir[0]) go("", 1); break;
    case M_REFRESH:
        load_places();
        load_quick();
        build_side();
        if (dir[0])
            load(1);
        else
            home_scan(1);
        say("%s", "Aktualisiert");
        break;
    case M_SORT_NAME:
    case M_SORT_DATE:
    case M_SORT_SIZE:
        sort_mode = a - M_SORT_NAME;
        if (dir[0])
            load(1);
        break;
    }
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Aussehen: links die Seitenleiste (Baum, Speicher), rechts Tabs, Menuezeile, Navigation und der Inhalt
 * (Schnellzugriff oder Ordnerliste), unten die Statuszeile
 * ------------------------------------------------------------------------------------------------------------------- */

#define C_SIDE    0xEEF1F7
#define C_SIDE_HL 0xDCE4F4
#define C_STRIP   0xE6EAF2
#define C_FIELD   0xF0F2F7
#define C_HOME_BG 0xF8F9FC

static int side_w(void) { int w = U(220); return w > gfx_screen.w / 3 ? gfx_screen.w / 3 : w; }
static int tabs_h(void) { return U(38); }
static int mrow_h(void) { return U(30); }
static int nav_h(void) { return U(46); }
static int nav_y(void) { return tabs_h() + mrow_h(); }
static int tb_h(void) { return tabs_h() + mrow_h() + nav_h(); } /* ganzer Kopfbereich */
static int head_h(void) { return U(26); }
static int foot_h(void) { return U(24); }
static int list_y(void) { return tb_h() + head_h(); }
static int list_h(void) { return gfx_screen.h - list_y() - foot_h(); }
static int visible(void) { int v = list_h() / ROW_H; return v < 1 ? 1 : v; }
static int col_date(void) { return gfx_screen.w - U(250); }
static int col_size(void) { return gfx_screen.w - U(92); }
static int show_date(void) { return col_date() > side_w() + U(200); }

/* Klickbare Flaechen: beim Zeichnen gesammelt, beim naechsten Ereignis abgefragt */
enum { H_NONE, H_TAB, H_TABX, H_TABNEW, H_MENU, H_BACK, H_FWD, H_UP, H_CRUMB, H_REFRESH, H_SEARCH, H_SIDE, H_SIDECHEV,
       H_TILE, H_RECENT, H_COL, H_DRAG, H_WMIN, H_WMAX, H_WCLOSE };
typedef struct {
    int kind, idx, x, y, w, h;
} Hit;
static Hit hits[200];
static int nhits, hov_kind, hov_idx = -1, hover_row = -1;

static void hit_add(int kind, int idx, int x, int y, int w, int h)
{
    if (nhits < (int)(sizeof(hits) / sizeof(hits[0])))
        hits[nhits++] = (Hit){kind, idx, x, y, w, h};
}

static const Hit *hit_at(int px, int py)
{
    for (int i = nhits - 1; i >= 0; i--)
        if (px >= hits[i].x && px < hits[i].x + hits[i].w && py >= hits[i].y && py < hits[i].y + hits[i].h)
            return &hits[i];
    return 0;
}

static int hovered(int kind, int idx) { return hov_kind == kind && hov_idx == idx; }

static int row_at(int px, int py)
{
    if (!dir[0] || px < side_w() || py < list_y() || py >= list_y() + list_h())
        return -2; /* nicht in der Liste */
    int i = scroll + (py - list_y()) / ROW_H;
    return i < nent ? i : -1; /* -1: leere Flaeche */
}

static int current_place(void) /* der Ort, in dem man gerade ist (laengster passender Pfad) */
{
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < nplaces; i++) {
        size_t l = strlen(places[i].path);
        if (dir[0] && inside(dir, places[i].path) && (best < 0 || l > bl)) {
            best = i;
            bl = l;
        }
    }
    return best;
}

static void dir_title(const char *d, char *out, int max)
{
    if (!d[0]) {
        snprintf(out, max, "Schnellzugriff");
        return;
    }
    for (int i = 0; i < nplaces; i++)
        if (strcmp(d, places[i].path) == 0) {
            snprintf(out, max, "%s", places[i].label);
            return;
        }
    snprintf(out, max, "%s", base_of(d));
}

/* ---------- kleine Symbole (aus Linien, die Schrift hat nicht alle Zeichen) ---------- */

static void place_icon(Surface *s, int kind, int x, int y, int sz)
{
    if (kind == 1) {
        ui_folder_icon(s, x, y, sz);
        return;
    }
    u32 c = kind == 0 ? 0x8E8E93 : 0xFF9F0A;
    gfx_round_rect_grad(s, x, y + sz / 5, sz, sz * 3 / 5, sz / 6, gfx_mix(c, 0xFFFFFF, 70), c, 255);
    gfx_disc(s, x + sz * 0.78f, y + sz * 0.5f, sz * 0.07f, 0xFFFFFF, 255);
}

static void star_icon(Surface *s, float cx, float cy, float r, u32 c)
{
    for (int i = 0; i < 5; i++) { /* fuenf Zacken von der Mitte aus */
        float a = -1.5707963f + 6.2831853f * i / 5;
        float tx = cx + ui_sin(a + 1.5707963f) * r, ty = cy + ui_sin(a) * r;
        gfx_capsule(s, cx, cy, tx, ty, r * 0.42f, c, 255);
    }
    gfx_disc(s, cx, cy, r * 0.5f, c, 255);
}

static void chevron(Surface *s, float cx, float cy, float k, int dir_, u32 c) /* 0 rechts, 1 unten, 2 links, 3 oben */
{
    float dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    float ax = dx[dir_] * 2.5f * k, ay = dy[dir_] * 2.5f * k, px = -dy[dir_] * 4 * k, py = dx[dir_] * 4 * k;
    gfx_capsule(s, cx - ax + px, cy - ay + py, cx + ax, cy + ay, 1.6f * k, c, 255);
    gfx_capsule(s, cx - ax - px, cy - ay - py, cx + ax, cy + ay, 1.6f * k, c, 255);
}

static void arrow_icon(Surface *s, float cx, float cy, float k, int dir_, u32 c) /* Pfeil: 0 rechts, 2 links, 3 oben */
{
    float dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    float ex = cx + dx[dir_] * 7 * k, ey = cy + dy[dir_] * 7 * k;
    gfx_capsule(s, cx - dx[dir_] * 7 * k, cy - dy[dir_] * 7 * k, ex, ey, 1.8f * k, c, 255);
    float px = -dy[dir_] * 5 * k, py = dx[dir_] * 5 * k, bx = ex - dx[dir_] * 5 * k, by = ey - dy[dir_] * 5 * k;
    gfx_capsule(s, bx + px, by + py, ex, ey, 1.8f * k, c, 255);
    gfx_capsule(s, bx - px, by - py, ex, ey, 1.8f * k, c, 255);
}

static void search_icon(Surface *s, float cx, float cy, float k, u32 c)
{
    const int n = 18;
    float r = 4.5f * k, ox = cx - 1.5f * k, oy = cy - 1.5f * k;
    for (int i = 0; i < n; i++) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        gfx_capsule(s, ox + ui_sin(a0 + 1.5707963f) * r, oy + ui_sin(a0) * r, ox + ui_sin(a1 + 1.5707963f) * r,
                    oy + ui_sin(a1) * r, 1.6f * k, c, 255);
    }
    gfx_capsule(s, cx + 2 * k, cy + 2 * k, cx + 5.5f * k, cy + 5.5f * k, 2 * k, c, 255);
}

static void refresh_icon(Surface *s, float cx, float cy, float k, u32 c)
{
    const int n = 14;
    float r = 5.5f * k;
    for (int i = 0; i < n; i++) { /* Kreis mit Luecke oben rechts */
        float a0 = 0.5f + 5.2f * i / n, a1 = 0.5f + 5.2f * (i + 1) / n;
        gfx_capsule(s, cx + ui_sin(a0 + 1.5707963f) * r, cy - ui_sin(a0) * r, cx + ui_sin(a1 + 1.5707963f) * r,
                    cy - ui_sin(a1) * r, 1.6f * k, c, 255);
    }
    float ex = cx + ui_sin(0.5f + 1.5707963f) * r, ey = cy - ui_sin(0.5f) * r; /* Pfeilspitze am Anfang */
    gfx_capsule(s, ex, ey, ex - 3.5f * k, ey - 1 * k, 1.6f * k, c, 255);
    gfx_capsule(s, ex, ey, ex + 0.5f * k, ey - 3.8f * k, 1.6f * k, c, 255);
}

static void plus_icon(Surface *s, float cx, float cy, float k, u32 c)
{
    gfx_capsule(s, cx - 6 * k, cy, cx + 6 * k, cy, 1.7f * k, c, 255);
    gfx_capsule(s, cx, cy - 6 * k, cx, cy + 6 * k, 1.7f * k, c, 255);
}

static void close_icon(Surface *s, float cx, float cy, float k, u32 c)
{
    gfx_capsule(s, cx - 3.5f * k, cy - 3.5f * k, cx + 3.5f * k, cy + 3.5f * k, 1.4f * k, c, 255);
    gfx_capsule(s, cx - 3.5f * k, cy + 3.5f * k, cx + 3.5f * k, cy - 3.5f * k, 1.4f * k, c, 255);
}

static void check_icon(Surface *s, float cx, float cy, float k, u32 c)
{
    gfx_capsule(s, cx - 4 * k, cy, cx - 1 * k, cy + 3 * k, 1.7f * k, c, 255);
    gfx_capsule(s, cx - 1 * k, cy + 3 * k, cx + 5 * k, cy - 4 * k, 1.7f * k, c, 255);
}

static u32 file_accent(const char *name)
{
    return ends_with(name, ".bmp") ? 0x34C759 : ends_with(name, ".sh") ? 0x0A84FF :
           ends_with(name, ".wav") || ends_with(name, ".mp3") ? 0xFF2D55 : ends_with(name, ".txt") ? 0xFFCC00 : 0;
}

static void entry_icon(Surface *s, const char *name, int is_dir, int x, int y, int sz)
{
    if (is_dir)
        ui_folder_icon(s, x, y, sz);
    else
        ui_doc_icon(s, x, y, sz, file_accent(name));
}

/* Grosser Ordner mit Zeichen nach dem Namen (Musik, Bilder, Downloads, Dokumente) */
static void big_folder(Surface *s, const char *name, int x, int y, int sz)
{
    ui_folder_icon(s, x, y, sz);
    char l[64];
    int n = 0;
    for (; name[n] && n < 63; n++)
        l[n] = (char)((name[n] >= 'A' && name[n] <= 'Z') ? name[n] + 32 : name[n]);
    l[n] = 0;
    int e = sz * 2 / 5, ex = x + sz - e - sz / 10, ey = y + sz * 9 / 10 - e - sz / 14;
    float k = sz / 70.0f;
    if (strstr(l, "musik") || strstr(l, "music") || strstr(l, "mp3"))
        ui_app_icon(s, ICON_MUSIC, ex, ey, e);
    else if (strstr(l, "bild") || strstr(l, "foto") || strstr(l, "pic") || strstr(l, "image"))
        ui_app_icon(s, ICON_IMAGE, ex, ey, e);
    else if (strstr(l, "download")) {
        float cx = ex + e * 0.5f, cy = ey + e * 0.45f;
        gfx_capsule(s, cx, cy - 9 * k, cx, cy + 7 * k, 4.5f * k, 0x0A64D6, 255);
        gfx_capsule(s, cx - 8 * k, cy, cx, cy + 9 * k, 4.5f * k, 0x0A64D6, 255);
        gfx_capsule(s, cx + 8 * k, cy, cx, cy + 9 * k, 4.5f * k, 0x0A64D6, 255);
    } else if (strstr(l, "dokument") || strstr(l, "doc") || strstr(l, "text"))
        ui_doc_icon(s, ex, ey, e, 0);
}

/* ---------- Seitenleiste: Schnellzugriff (Ordner der Platte), Orte mit aufklappbaren Unterordnern ---------- */

typedef struct {
    char label[48], path[256];
    int  depth, icon, place, open; /* icon: 0 Stern, 1 Ordner, 2.. Ort (2 + Art); place: Index in places oder -1 */
} Node;
static Node nodes[64];
static int  nnodes, qa_open = 1, place_open[MAXPLACES];

/* Ordner der Platte (fuer Schnellzugriff und die grossen Ordner) */
static char quick[8][256];
static int  nquick;

static void load_quick(void)
{
    nquick = 0;
    DirEnt de;
    for (u64 i = 0; nquick < 8 && sys_readdir("/disk", i, &de) == 0; i++)
        if (de.is_dir && de.name[0] != '.')
            join(quick[nquick++], 256, "/disk", de.name);
}

static void node_add(const char *label, const char *path, int depth, int icon, int place, int open)
{
    if (nnodes == (int)(sizeof(nodes) / sizeof(nodes[0])))
        return;
    Node *n = &nodes[nnodes++];
    snprintf(n->label, sizeof(n->label), "%s", label);
    snprintf(n->path, sizeof(n->path), "%s", path);
    n->depth = depth;
    n->icon = icon;
    n->place = place;
    n->open = open;
}

static void build_side(void)
{
    nnodes = 0;
    node_add("Schnellzugriff", "", 0, 0, -1, qa_open);
    if (qa_open)
        for (int i = 0; i < nquick; i++)
            node_add(base_of(quick[i]), quick[i], 1, 1, -1, 0);
    for (int p = 0; p < nplaces; p++) {
        node_add(places[p].label, places[p].path, 0, 2 + places[p].kind, p, place_open[p]);
        if (!place_open[p])
            continue;
        DirEnt de;
        int n = 0;
        for (u64 i = 0; n < 12 && sys_readdir(places[p].path, i, &de) == 0; i++)
            if (de.is_dir && de.name[0] != '.') {
                char full[256];
                join(full, sizeof(full), places[p].path, de.name);
                node_add(de.name, full, 1, 1, -1, 0);
                n++;
            }
    }
}

static int side_selected(void) /* Knoten des aktuellen Ordners: genau, sonst der Ort darum */
{
    for (int i = 0; i < nnodes; i++)
        if (strcmp(nodes[i].path, dir) == 0)
            return i;
    int cp = current_place();
    for (int i = 0; i < nnodes; i++)
        if (nodes[i].place == cp && cp >= 0)
            return i;
    return -1;
}

/* ---------- Schnellzugriff: alles unter /disk einmal durchsehen (fuer "Zuletzt geaendert" und die Suche) ---------- */

typedef struct {
    char path[256];
    u64  mtime, size;
    int  is_dir;
} Found;
#define MAXFOUND 800
static Found *found;
static int    nfound, recent[12], nrecent, results[40], nresults;
static s64    scanned_us;

static void scan(const char *d, int depth)
{
    DirEnt de;
    for (u64 i = 0; nfound < MAXFOUND && sys_readdir(d, i, &de) == 0; i++) {
        if (de.name[0] == '.')
            continue;
        Found *f = &found[nfound];
        if (join(f->path, sizeof(f->path), d, de.name))
            continue;
        f->mtime = de.mtime;
        f->size = de.size;
        f->is_dir = (int)de.is_dir;
        nfound++;
        if (de.is_dir && depth < 3) {
            char sub[256];
            snprintf(sub, sizeof(sub), "%s", f->path);
            scan(sub, depth + 1);
        }
    }
}

static int contains_ci(const char *s, const char *q)
{
    int n = (int)strlen(q);
    for (; *s; s++) {
        int i = 0;
        while (i < n && ((s[i] | 0x20) == (q[i] | 0x20) || s[i] == q[i]))
            i++;
        if (i == n)
            return 1;
    }
    return n == 0;
}

static void home_search(void)
{
    nresults = 0;
    for (int i = 0; i < nfound && nresults < 40; i++)
        if (contains_ci(base_of(found[i].path), query))
            results[nresults++] = i;
}

static void home_scan(int force)
{
    if (!force && found && sys_time_us() - scanned_us < 10000000)
        return;
    if (!found)
        found = u_malloc(sizeof(Found) * MAXFOUND);
    nfound = 0;
    if (exists("/disk"))
        scan("/disk", 0);
    scanned_us = sys_time_us();
    nrecent = 0; /* die zuletzt geaenderten Dateien, neueste zuerst */
    for (int i = 0; i < nfound; i++) {
        if (found[i].is_dir || !found[i].mtime)
            continue;
        int pos = nrecent;
        while (pos > 0 && found[recent[pos - 1]].mtime < found[i].mtime)
            pos--;
        if (pos >= 12)
            continue;
        if (nrecent < 12)
            nrecent++;
        for (int k = nrecent - 1; k > pos; k--)
            recent[k] = recent[k - 1];
        recent[pos] = i;
    }
    home_search();
}

static void home_enter(void)
{
    u_free(ents);
    ents = 0;
    nent = 0;
    scroll = 0;
    cur = anchor = -1;
    home_scan(0);
    u64 fsz[2];
    total_bytes = sys_statfs("/disk", fsz) == 0 ? fsz[0] : 0;
    free_bytes = total_bytes ? (s64)fsz[1] : -1;
    gfx_set_title("Schnellzugriff");
}

/* Ordner "Platte > Musik > Alben" */
static void path_label(const char *p, char *out, int max)
{
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < nplaces; i++) {
        size_t l = strlen(places[i].path);
        if (inside(p, places[i].path) && (best < 0 || l > bl)) {
            best = i;
            bl = l;
        }
    }
    const char *rest = best >= 0 ? p + bl : p;
    int n = snprintf(out, max, "%s", best >= 0 ? places[best].label : "");
    for (; *rest && n < max - 8; rest++) {
        if (*rest == '/') {
            if (rest[1])
                n += snprintf(out + n, max - n, "  >  ");
        } else {
            out[n++] = *rest;
            out[n] = 0;
        }
    }
}

static void when_str(u64 t, char *out, int max)
{
    out[0] = 0;
    s64 now = sys_time();
    if (!t || now <= 0)
        return;
    s64 d = now - (s64)t;
    if (d >= 0 && d < 60) {
        snprintf(out, max, "gerade eben");
        return;
    }
    if (d >= 0 && d < 3600) {
        snprintf(out, max, "vor %lld Min.", (long long)(d / 60));
        return;
    }
    DateTime a, b, y;
    time_to_date(t, &a);
    time_to_date((u64)now, &b);
    time_to_date((u64)(now - 86400), &y);
    if (a.year == b.year && a.month == b.month && a.day == b.day)
        snprintf(out, max, "Heute um %02d:%02d", a.hour, a.min);
    else if (a.year == y.year && a.month == y.month && a.day == y.day)
        snprintf(out, max, "Gestern um %02d:%02d", a.hour, a.min);
    else
        snprintf(out, max, "%02d.%02d.%04d", a.day, a.month, a.year);
}

/* ---------- Tabs ---------- */

#define MAXTABS 8
typedef struct {
    char dir[256];
    char back[MAXHIST][256], fwd[MAXHIST][256];
    int  nback, nfwd, scroll;
} Tab;
static Tab *tabs[MAXTABS];
static int  ntabs, cur_tab;

static void tab_save(Tab *t)
{
    snprintf(t->dir, sizeof(t->dir), "%s", dir);
    memcpy(t->back, back_hist, sizeof(back_hist));
    memcpy(t->fwd, fwd_hist, sizeof(fwd_hist));
    t->nback = nback;
    t->nfwd = nfwd;
    t->scroll = scroll;
}

static void tab_restore(const Tab *t)
{
    snprintf(dir, sizeof(dir), "%s", t->dir);
    memcpy(back_hist, t->back, sizeof(back_hist));
    memcpy(fwd_hist, t->fwd, sizeof(fwd_hist));
    nback = t->nback;
    nfwd = t->nfwd;
    query[0] = 0;
    search_focus = 0;
    status[0] = 0;
    if (!dir[0] || !exists(dir)) {
        dir[0] = 0;
        home_enter();
        return;
    }
    load(0);
    scroll = t->scroll;
    if (scroll > nent - visible())
        scroll = nent - visible();
    if (scroll < 0)
        scroll = 0;
}

static void tab_new(const char *d)
{
    if (ntabs == MAXTABS) {
        say("H\xC3\xB6" "chstens %s Tabs", "8");
        return;
    }
    if (ntabs)
        tab_save(tabs[cur_tab]);
    Tab *t = u_malloc(sizeof(Tab));
    memset(t, 0, sizeof(*t));
    snprintf(t->dir, sizeof(t->dir), "%s", d);
    tabs[ntabs] = t;
    cur_tab = ntabs++;
    tab_restore(t);
}

static void tab_switch(int i)
{
    if (i < 0 || i >= ntabs || i == cur_tab)
        return;
    tab_save(tabs[cur_tab]);
    cur_tab = i;
    tab_restore(tabs[i]);
}

static void tab_close(int i)
{
    if (ntabs <= 1 || i < 0 || i >= ntabs)
        return;
    if (i != cur_tab)
        tab_save(tabs[cur_tab]);
    u_free(tabs[i]);
    for (int k = i; k < ntabs - 1; k++)
        tabs[k] = tabs[k + 1];
    ntabs--;
    if (cur_tab > i || cur_tab == ntabs)
        cur_tab = cur_tab > 0 ? cur_tab - 1 : 0;
    if (cur_tab >= ntabs)
        cur_tab = ntabs - 1;
    tab_restore(tabs[cur_tab]);
}

/* ---------- Menues (Kontextmenue und die Menuezeile teilen sich die Darstellung) ---------- */

static const char *const mrow_label[3] = {"Datei", "Bearbeiten", "Ansicht"};
static int dropdown; /* offenes Menue der Menuezeile + 1 */

static void menu_size(int *w, int *h)
{
    *w = U(240);
    *h = U(10) + nmenu * U(24);
}

static void dialog_box(int *x, int *y, int *w, int *h)
{
    *w = U(420);
    *h = dialog == D_DELETE || dialog == D_ERROR ? U(150) : U(160);
    if (*w > gfx_screen.w - U(20))
        *w = gfx_screen.w - U(20);
    *x = (gfx_screen.w - *w) / 2;
    *y = tb_h() + U(40);
}

static void dialog_btn(int i, int *bx, int *by, int *bw, int *bh)
{
    int x, y, w, h;
    dialog_box(&x, &y, &w, &h);
    *bw = U(120);
    *bh = U(30);
    *by = y + h - U(16) - *bh;
    *bx = x + w - U(16) - (2 - i) * *bw - (1 - i) * U(10);
}

static void draw_dialog(Surface *s)
{
    int x, y, w, h;
    dialog_box(&x, &y, &w, &h);
    gfx_blend_fill(s, 0, 0, s->w, s->h, 0x000000, 40);
    gfx_shadow(s, x, y + U(4), w, h, U(12), U(24), 80);
    gfx_round_rect(s, x, y, w, h, U(12), 0xF6F6F8, 255);
    gfx_round_frame(s, x, y, w, h, U(12), 0x000000, 30);
    int tx = x + U(20), ty = y + U(16);
    const char *title = dialog == D_RENAME ? "Umbenennen" : dialog == D_NEWFOLDER ? "Neuer Ordner" : dialog_msg;
    text_draw(s, font_bold, U(15), tx, ty, title, C_TEXT);
    ty += text_height(font_bold, U(15)) + U(8);
    if (dialog == D_DELETE) {
        text_draw(s, font_ui, FS, tx, ty, "Das l\xC3\xA4sst sich nicht r\xC3\xBC" "ckg\xC3\xA4ngig machen.", C_TEXT2);
    } else {
        int fh = U(28), fw = w - U(40);
        gfx_round_rect(s, tx, ty, fw, fh, U(6), 0xFFFFFF, 255);
        gfx_round_frame(s, tx, ty, fw, fh, U(6), C_ACCENT, 200);
        int fy = ty + (fh - text_height(font_ui, FS)) / 2, tw = text_width(font_ui, FS, field), sx = tx + U(10);
        if (tw > fw - U(24))
            sx -= tw - (fw - U(24));
        gfx_set_clip(tx + U(4), ty, fw - U(8), fh);
        text_draw(s, font_ui, FS, sx, fy, field, C_TEXT);
        gfx_fill(s, sx + tw + 1, fy, 2, text_height(font_ui, FS), C_ACCENT);
        gfx_no_clip();
    }
    for (int i = 0; i < 2; i++) {
        int bx, by, bw, bh;
        dialog_btn(i, &bx, &by, &bw, &bh);
        u32 bg = i ? (dialog == D_DELETE ? 0xFF3B30 : C_ACCENT) : 0xE3E3E8, fg = i ? 0xFFFFFF : C_TEXT;
        if (i == dialog_hover)
            bg = gfx_mix(bg, 0x000000, 30);
        gfx_round_rect(s, bx, by, bw, bh, U(7), bg, 255);
        const char *t = i == 0 ? "Abbrechen" : dialog == D_DELETE ? "L\xC3\xB6schen" : dialog == D_RENAME ? "Umbenennen" : "Anlegen";
        text_draw(s, font_bold, FS, bx + (bw - text_width(font_bold, FS, t)) / 2, by + (bh - text_height(font_bold, FS)) / 2, t, fg);
    }
}

/* ---------- Zeichnen ---------- */

static void draw_side(Surface *s, int H)
{
    int sw = side_w();
    float k = (float)U(1);
    gfx_fill(s, 0, 0, sw, H, C_SIDE);
    gfx_fill(s, sw - 1, 0, 1, H, 0xDCE0E8);
    hit_add(H_DRAG, 0, 0, 0, sw, tabs_h()); /* Kopf: Fenster verschieben (es hat keine Titelleiste) */
    ui_app_icon(s, ICON_FILES, U(14), (tabs_h() - U(22)) / 2 + U(2), U(22));
    text_draw(s, font_bold, U(15), U(44), (tabs_h() - text_height(font_bold, U(15))) / 2 + U(2), "Dateien", C_TEXT);

    int card_h = U(78), bottom = H - card_h, rh = U(30), sel = side_selected();
    gfx_set_clip(0, tabs_h() + U(8), sw, bottom - tabs_h() - U(8));
    for (int i = 0; i < nnodes; i++) {
        Node *n = &nodes[i];
        int y = tabs_h() + U(10) + i * rh;
        if (y + rh > bottom)
            break;
        int drop = i == drop_place;
        if (drop)
            gfx_round_rect(s, U(6), y, sw - U(12), rh - U(2), U(7), C_ACCENT, 255);
        else if (i == sel)
            gfx_round_rect(s, U(6), y, sw - U(12), rh - U(2), U(7), C_SIDE_HL, 255);
        else if (hovered(H_SIDE, i) || hovered(H_SIDECHEV, i))
            gfx_round_rect(s, U(6), y, sw - U(12), rh - U(2), U(7), 0xE4E8F1, 255);
        int ix = U(14) + n->depth * U(22), isz = U(18), iy = y + (rh - U(2) - isz) / 2;
        if (n->icon == 0)
            star_icon(s, ix + isz * 0.5f, iy + isz * 0.5f, isz * 0.48f, 0x0A84FF);
        else if (n->icon == 1)
            ui_folder_icon(s, ix, iy, isz);
        else
            place_icon(s, n->icon - 2, ix, iy, isz);
        Font *f = n->depth ? font_ui : font_bold;
        int tx = ix + isz + U(10), chev = n->depth == 0;
        gfx_set_clip(0, y, sw - (chev ? U(34) : U(10)), rh);
        text_draw(s, f, FS, tx, y + (rh - U(2) - text_height(f, FS)) / 2, n->label, drop ? 0xFFFFFF : C_TEXT);
        gfx_set_clip(0, tabs_h() + U(8), sw, bottom - tabs_h() - U(8));
        hit_add(H_SIDE, i, U(6), y, sw - U(12), rh - U(2));
        if (chev) {
            chevron(s, sw - U(22), y + (rh - U(2)) * 0.5f, k, n->open ? 1 : 0, drop ? 0xFFFFFF : 0x6E6E73);
            hit_add(H_SIDECHEV, i, sw - U(38), y, U(32), rh - U(2));
        }
    }
    gfx_no_clip();

    /* Speicher des aktuellen Ortes */
    gfx_fill(s, 0, bottom, sw - 1, card_h, 0xE8ECF3);
    gfx_fill(s, 0, bottom, sw - 1, 1, 0xDCE0E8);
    if (total_bytes) {
        int cp = current_place();
        char lab[64], a[24], b[24], t[80];
        snprintf(lab, sizeof(lab), "%s", cp >= 0 ? places[cp].label : "Platte");
        place_icon(s, cp >= 0 ? places[cp].kind : 0, U(14), bottom + U(13), U(18));
        text_draw(s, font_bold, FS, U(42), bottom + U(14), lab, C_TEXT);
        u64 used = free_bytes >= 0 && (u64)free_bytes <= total_bytes ? total_bytes - (u64)free_bytes : 0;
        int bw = sw - U(30), fill = (int)((u64)bw * used / total_bytes);
        gfx_round_rect(s, U(14), bottom + U(42), bw, U(5), U(2), 0xD3D8E3, 255);
        if (fill > U(4))
            gfx_round_rect(s, U(14), bottom + U(42), fill, U(5), U(2), C_ACCENT, 255);
        fmt_size(used, a, sizeof(a));
        fmt_size(total_bytes, b, sizeof(b));
        snprintf(t, sizeof(t), "%s von %s belegt", a, b);
        text_draw(s, font_ui, FS_SMALL, U(14), bottom + U(54), t, C_TEXT2);
    }
}

static void draw_tabs(Surface *s, int W)
{
    int sw = side_w(), th = tabs_h(), x = sw + U(8);
    float k = (float)U(1);
    gfx_fill(s, sw, 0, W - sw, th, C_STRIP);
    hit_add(H_DRAG, 0, sw, 0, W - sw, th); /* freie Flaeche: Fenster verschieben, Doppelklick maximiert */
    int wb = gfx_windowed() ? U(46) : 0; /* Fensterknoepfe rechts (nur unter dem Desktop) */
    int avail = W - x - U(46) - 3 * wb - U(40), tw = ntabs ? avail / ntabs : avail;
    if (tw > U(210))
        tw = U(210);
    for (int i = 0; i < ntabs; i++) {
        int active = i == cur_tab, ty = U(6), h = th - ty;
        char lab[64];
        dir_title(active ? dir : tabs[i]->dir, lab, sizeof(lab));
        if (active) {
            gfx_round_rect(s, x, ty, tw, h, U(9), C_WINDOW, 255);
            gfx_fill(s, x, ty + U(10), tw, h - U(10), C_WINDOW);
        } else if (hovered(H_TAB, i) || hovered(H_TABX, i)) {
            gfx_round_rect(s, x, ty, tw, h - U(4), U(9), 0xF2F4F9, 255);
        }
        int cy = ty + h / 2, isz = U(16);
        if (!(active ? dir : tabs[i]->dir)[0])
            star_icon(s, x + U(12) + isz * 0.5f, cy, isz * 0.48f, 0x0A84FF);
        else
            ui_folder_icon(s, x + U(12), cy - isz / 2, isz);
        int show_x = ntabs > 1 && (active || hovered(H_TAB, i) || hovered(H_TABX, i));
        gfx_set_clip(x, 0, tw - (show_x ? U(30) : U(8)), th);
        text_draw(s, active ? font_bold : font_ui, FS, x + U(36), cy - text_height(font_ui, FS) / 2, lab, C_TEXT);
        gfx_no_clip();
        hit_add(H_TAB, i, x, ty, tw, h);
        if (show_x) {
            int bx = x + tw - U(26);
            if (hovered(H_TABX, i))
                gfx_round_rect(s, bx, cy - U(10), U(20), U(20), U(5), 0x000000, 20);
            close_icon(s, bx + U(10), cy, k, 0x6E6E73);
            hit_add(H_TABX, i, bx, cy - U(10), U(20), U(20));
        }
        x += tw + U(2);
    }
    int bx = x + U(4), by = U(6) + (th - U(6) - U(28)) / 2;
    if (hovered(H_TABNEW, 0))
        gfx_round_rect(s, bx, by, U(28), U(28), U(7), 0x000000, 18);
    plus_icon(s, bx + U(14), by + U(14), k, C_TEXT);
    hit_add(H_TABNEW, 0, bx, by, U(28), U(28));

    /* Minimieren, Maximieren (bzw. Wiederherstellen), Schliessen - wie bei Windows */
    if (!wb)
        return;
    int kinds[3] = {H_WMIN, H_WMAX, H_WCLOSE};
    for (int i = 0; i < 3; i++) {
        int x0 = W - (3 - i) * wb, hv = hovered(kinds[i], 0);
        float cx = x0 + wb * 0.5f, cy = th * 0.5f;
        if (hv)
            gfx_fill(s, x0, 0, wb, th, i == 2 ? 0xE81123 : 0xD8DDE8);
        u32 c = hv && i == 2 ? 0xFFFFFF : C_TEXT;
        if (i == 0) {
            gfx_capsule(s, cx - 5 * k, cy, cx + 5 * k, cy, 1.1f * k, c, 255);
        } else if (i == 1) {
            if (gfx_window_zoomed()) { /* zwei Fenster: wiederherstellen */
                gfx_capsule(s, cx - 2.5f * k, cy - 5 * k, cx + 5 * k, cy - 5 * k, 1.1f * k, c, 255);
                gfx_capsule(s, cx + 5 * k, cy - 5 * k, cx + 5 * k, cy + 2.5f * k, 1.1f * k, c, 255);
                gfx_fill(s, (int)(cx - 5 * k), (int)(cy - 2.5f * k), (int)(7.5f * k), (int)(7.5f * k), hv ? 0xD8DDE8 : C_STRIP);
                gfx_capsule(s, cx - 5 * k, cy - 2.5f * k, cx + 2.5f * k, cy - 2.5f * k, 1.1f * k, c, 255);
                gfx_capsule(s, cx + 2.5f * k, cy - 2.5f * k, cx + 2.5f * k, cy + 5 * k, 1.1f * k, c, 255);
                gfx_capsule(s, cx + 2.5f * k, cy + 5 * k, cx - 5 * k, cy + 5 * k, 1.1f * k, c, 255);
                gfx_capsule(s, cx - 5 * k, cy + 5 * k, cx - 5 * k, cy - 2.5f * k, 1.1f * k, c, 255);
            } else {
                gfx_capsule(s, cx - 5 * k, cy - 5 * k, cx + 5 * k, cy - 5 * k, 1.1f * k, c, 255);
                gfx_capsule(s, cx + 5 * k, cy - 5 * k, cx + 5 * k, cy + 5 * k, 1.1f * k, c, 255);
                gfx_capsule(s, cx + 5 * k, cy + 5 * k, cx - 5 * k, cy + 5 * k, 1.1f * k, c, 255);
                gfx_capsule(s, cx - 5 * k, cy + 5 * k, cx - 5 * k, cy - 5 * k, 1.1f * k, c, 255);
            }
        } else {
            gfx_capsule(s, cx - 5 * k, cy - 5 * k, cx + 5 * k, cy + 5 * k, 1.2f * k, c, 255);
            gfx_capsule(s, cx - 5 * k, cy + 5 * k, cx + 5 * k, cy - 5 * k, 1.2f * k, c, 255);
        }
        hit_add(kinds[i], 0, x0, 0, wb, th);
    }
}

/* Pfade der Pfadleiste (beim Zeichnen gemerkt) */
static char crumb_path[16][256];
static void crumb_paths_set(int i, const char *p)
{
    if (i >= 0 && i < 16)
        snprintf(crumb_path[i], 256, "%s", p);
}

static void draw_crumbs(Surface *s, int x, int y, int w, int h)
{
    char seg[16][64], segp[16][256];
    int n = 0;
    if (!dir[0]) {
        snprintf(seg[0], 64, "Schnellzugriff");
        segp[0][0] = 0;
        n = 1;
    } else {
        int cp = current_place();
        const char *root = cp >= 0 ? places[cp].path : "/";
        snprintf(seg[0], 64, "%s", cp >= 0 ? places[cp].label : "System");
        snprintf(segp[0], 256, "%s", root);
        n = 1;
        const char *r = dir + strlen(root);
        char acc[256];
        snprintf(acc, sizeof(acc), "%s", root);
        while (*r && n < 16) {
            while (*r == '/')
                r++;
            if (!*r)
                break;
            int l = 0;
            char name[64];
            while (r[l] && r[l] != '/' && l < 63) {
                name[l] = r[l];
                l++;
            }
            name[l] = 0;
            r += l;
            char next[256];
            join(next, sizeof(next), acc, name);
            snprintf(acc, sizeof(acc), "%s", next);
            snprintf(seg[n], 64, "%s", name);
            snprintf(segp[n], 256, "%s", acc);
            n++;
        }
    }
    float k = (float)U(1);
    int cy = y + h / 2, isz = U(16), ix = x + U(12);
    if (!dir[0])
        star_icon(s, ix + isz * 0.5f, cy, isz * 0.48f, 0x0A84FF);
    else {
        int cp = current_place();
        if (cp >= 0)
            place_icon(s, places[cp].kind, ix, cy - isz / 2, isz);
        else
            ui_folder_icon(s, ix, cy - isz / 2, isz);
    }
    int sep = U(20), first = 0, tx = ix + isz + U(10), maxw = x + w - tx - U(36), total = 0;
    for (int i = 0; i < n; i++)
        total += text_width(font_ui, FS, seg[i]) + U(12) + sep;
    while (first < n - 1 && total > maxw) { /* zu lang: vorne weglassen */
        total -= text_width(font_ui, FS, seg[first]) + U(12) + sep;
        first++;
    }
    gfx_set_clip(x, y, w - U(34), h);
    for (int i = first; i < n; i++) {
        int sw_ = text_width(font_ui, FS, seg[i]) + U(12);
        if (hovered(H_CRUMB, i))
            gfx_round_rect(s, tx, y + U(4), sw_, h - U(8), U(6), 0x000000, 16);
        text_draw(s, i == n - 1 ? font_bold : font_ui, FS, tx + U(6), cy - text_height(font_ui, FS) / 2, seg[i], C_TEXT);
        hit_add(H_CRUMB, i, tx, y, sw_, h);
        crumb_paths_set(i, segp[i]);
        tx += sw_;
        chevron(s, tx + sep * 0.5f, cy, k * 0.85f, 0, 0x8E8E93);
        tx += sep;
        if (i == n - 1)
            break;
    }
    gfx_no_clip();
    int rx = x + w - U(30);
    if (hovered(H_REFRESH, 0))
        gfx_round_rect(s, rx, y + U(4), U(26), h - U(8), U(6), 0x000000, 16);
    refresh_icon(s, rx + U(13), cy, k, C_TEXT);
    hit_add(H_REFRESH, 0, rx, y, U(26), h);
}

static void draw_nav(Surface *s, int W)
{
    int sw = side_w(), y = tabs_h();
    float k = (float)U(1);
    /* Menuezeile */
    gfx_fill(s, sw, y, W - sw, mrow_h(), C_WINDOW);
    int x = sw + U(14);
    for (int i = 0; i < 3; i++) {
        int w = text_width(font_ui, FS, mrow_label[i]) + U(20);
        if (dropdown == i + 1 || hovered(H_MENU, i))
            gfx_round_rect(s, x, y + U(3), w, mrow_h() - U(6), U(6), dropdown == i + 1 ? 0xDFE5F2 : 0xEEF1F7, 255);
        text_draw(s, font_ui, FS, x + U(10), y + (mrow_h() - text_height(font_ui, FS)) / 2, mrow_label[i], C_TEXT);
        hit_add(H_MENU, i, x, y, w, mrow_h());
        x += w + U(6);
    }
    /* Navigation */
    y = nav_y();
    int h = nav_h();
    gfx_fill(s, sw, y, W - sw, h, C_WINDOW);
    gfx_fill(s, sw, y + h - 1, W - sw, 1, 0xE6E8EE);
    int bx = sw + U(10), bs = U(32), by = y + (h - bs) / 2;
    int en[3] = {nback > 0, nfwd > 0, dir[0] && strcmp(dir, "/") != 0}, dirs[3] = {2, 0, 3}, kinds[3] = {H_BACK, H_FWD, H_UP};
    for (int i = 0; i < 3; i++) {
        if (en[i] && hovered(kinds[i], 0))
            gfx_round_rect(s, bx, by, bs, bs, U(7), 0x000000, 16);
        arrow_icon(s, bx + bs * 0.5f, by + bs * 0.5f, k, dirs[i], en[i] ? C_TEXT : 0xC4C4CA);
        hit_add(kinds[i], 0, bx, by, bs, bs);
        bx += bs + U(2);
    }
    int sfw = (W - sw) / 3 < U(240) ? (W - sw) / 3 : U(240), sfx = W - U(12) - sfw, fy = y + U(7), fh = h - U(14);
    int ax = bx + U(8), aw = sfx - U(10) - ax;
    gfx_round_rect(s, ax, fy, aw, fh, U(8), C_FIELD, 255);
    draw_crumbs(s, ax, fy, aw, fh);
    /* Suchfeld */
    gfx_round_rect(s, sfx, fy, sfw, fh, U(8), search_focus ? C_WINDOW : C_FIELD, 255);
    if (search_focus)
        gfx_round_frame(s, sfx, fy, sfw, fh, U(8), C_ACCENT, 255);
    int ty = fy + (fh - text_height(font_ui, FS)) / 2;
    gfx_set_clip(sfx + U(8), fy, sfw - U(40), fh);
    if (query[0])
        text_draw(s, font_ui, FS, sfx + U(12), ty, query, C_TEXT);
    else
        text_draw(s, font_ui, FS, sfx + U(12), ty, dir[0] ? "In diesem Ordner suchen" : "Auf der Platte suchen", C_TEXT2);
    if (search_focus)
        gfx_fill(s, sfx + U(12) + (query[0] ? text_width(font_ui, FS, query) + 1 : 0), fy + U(7), 2, fh - U(14), C_ACCENT);
    gfx_no_clip();
    search_icon(s, sfx + sfw - U(18), fy + fh * 0.5f, k, C_TEXT2);
    hit_add(H_SEARCH, 0, sfx, fy, sfw, fh);
}

static void recent_row(Surface *s, int idx, int kind, const Found *f, int x, int y, int w, int rh)
{
    if (hovered(kind, idx)) {
        gfx_shadow(s, x, y + U(2), w, rh, U(8), U(12), 40);
        gfx_round_rect(s, x, y, w, rh, U(8), C_WINDOW, 255);
    }
    const char *name = base_of(f->path);
    int isz = U(30);
    entry_icon(s, name, f->is_dir, x + U(14), y + (rh - isz) / 2, isz);
    char where[300], parent[256], t[40];
    parent_of(f->path, parent, sizeof(parent));
    path_label(parent, where, sizeof(where));
    when_str(f->mtime, t, sizeof(t));
    int tw = text_width(font_ui, FS, t), right = x + w - U(16) - tw;
    gfx_set_clip(x, y, right - x - U(12), rh);
    text_draw(s, font_ui, FS, x + U(58), y + U(8), name, C_TEXT);
    text_draw(s, font_ui, FS_SMALL, x + U(58), y + U(10) + text_height(font_ui, FS), where, C_TEXT2);
    gfx_no_clip();
    text_draw(s, font_ui, FS, right, y + (rh - text_height(font_ui, FS)) / 2, t, C_TEXT2);
    hit_add(kind, idx, x, y, w, rh);
}

static void draw_home(Surface *s, int W, int H)
{
    int sw = side_w(), x = sw + U(26), y = tb_h() + U(16), w = W - sw - U(52), bottom = H - foot_h();
    gfx_fill(s, sw, tb_h(), W - sw, bottom - tb_h(), C_HOME_BG);
    gfx_set_clip(sw, tb_h(), W - sw, bottom - tb_h());
    int rh = U(54);
    if (query[0]) {
        char t[120];
        snprintf(t, sizeof(t), "Suchergebnisse f\xC3\xBCr \xE2\x80\x9E%s\xE2\x80\x9C", query);
        text_draw(s, font_bold, U(15), x, y, t, C_TEXT);
        y += U(32);
        for (int i = 0; i < nresults && y + rh <= bottom; i++, y += rh + U(2))
            recent_row(s, i, H_RECENT, &found[results[i]], x - U(10), y, w + U(20), rh);
        if (!nresults)
            text_draw(s, font_ui, FS, x, y + U(10), "Nichts gefunden", 0xAEAEB2);
        gfx_no_clip();
        return;
    }
    text_draw(s, font_bold, U(15), x, y, "H\xC3\xA4ufige Ordner", C_TEXT);
    y += U(30);
    int tw = U(138), th = U(128), per = w / tw;
    if (per < 1)
        per = 1;
    /* Ordner der Platte, aufgefuellt mit den Orten */
    int nt = 0;
    for (int i = 0; i < nquick && nt < per; i++, nt++) {
        int tx = x - U(10) + nt * tw;
        if (hovered(H_TILE, nt))
            gfx_round_rect(s, tx, y, tw - U(8), th, U(10), 0xE9EDF6, 255);
        int isz = U(84);
        big_folder(s, base_of(quick[i]), tx + (tw - U(8) - isz) / 2, y + U(6), isz);
        const char *lab = base_of(quick[i]);
        int lw = text_width(font_ui, FS, lab);
        gfx_set_clip(tx + U(4), y, tw - U(16), th);
        text_draw(s, font_ui, FS, tx + (tw - U(8) - (lw < tw - U(16) ? lw : tw - U(16))) / 2, y + th - U(28), lab, C_TEXT);
        gfx_set_clip(sw, tb_h(), W - sw, bottom - tb_h());
        hit_add(H_TILE, nt, tx, y, tw - U(8), th);
    }
    for (int p = 0; p < nplaces && nt < per && nt < 4; p++, nt++) {
        int tx = x - U(10) + nt * tw;
        if (hovered(H_TILE, nt))
            gfx_round_rect(s, tx, y, tw - U(8), th, U(10), 0xE9EDF6, 255);
        int isz = U(70);
        place_icon(s, places[p].kind, tx + (tw - U(8) - isz) / 2, y + U(12), isz);
        int lw = text_width(font_ui, FS, places[p].label);
        text_draw(s, font_ui, FS, tx + (tw - U(8) - lw) / 2, y + th - U(28), places[p].label, C_TEXT);
        hit_add(H_TILE, nt, tx, y, tw - U(8), th);
    }
    y += th + U(22);
    text_draw(s, font_bold, U(15), x, y, "Zuletzt ge\xC3\xA4ndert", C_TEXT);
    y += U(30);
    for (int i = 0; i < nrecent && y + rh <= bottom; i++, y += rh + U(2))
        recent_row(s, i, H_RECENT, &found[recent[i]], x - U(10), y, w + U(20), rh);
    if (!nrecent)
        text_draw(s, font_ui, FS, x, y + U(6), "Noch keine Dateien auf der Platte", 0xAEAEB2);
    gfx_no_clip();
}

/* Ziel beim Klick auf eine grosse Kachel: zuerst die Ordner der Platte, dann die Orte */
static const char *tile_path(int i)
{
    if (i < nquick)
        return quick[i];
    i -= nquick;
    return i >= 0 && i < nplaces ? places[i].path : 0;
}

static void draw_list(Surface *s, int W)
{
    int sw = side_w(), ly = list_y(), lh = list_h();
    float k = (float)U(1);
    /* Kopfzeile: Klick sortiert */
    gfx_fill(s, sw, tb_h(), W - sw, head_h(), 0xFAFAFC);
    gfx_fill(s, sw, ly - 1, W - sw, 1, 0xE5E5EA);
    int hy = tb_h() + (head_h() - text_height(font_bold, FS_SMALL)) / 2;
    int cx[3] = {sw + U(40), col_date(), col_size()};
    const char *cl[3] = {"Name", "Ge\xC3\xA4ndert", "Gr\xC3\xB6\xC3\x9F" "e"};
    for (int c = 0; c < 3; c++) {
        if (c == 1 && !show_date())
            continue;
        int w = text_width(font_bold, FS_SMALL, cl[c]);
        text_draw(s, font_bold, FS_SMALL, cx[c], hy, cl[c], sort_mode == c ? C_TEXT : C_TEXT2);
        if (sort_mode == c)
            chevron(s, cx[c] + w + U(8), tb_h() + head_h() * 0.5f, k * 0.7f, c == 0 ? 3 : 1, C_TEXT);
        hit_add(H_COL, c, cx[c] - U(6), tb_h(), w + U(24), head_h());
    }

    /* Eintraege */
    gfx_set_clip(sw, ly, W - sw, lh);
    int vis = visible() + 1;
    for (int i = 0; i < vis && scroll + i < nent; i++) {
        Ent *e = &ents[scroll + i];
        int ry = ly + i * ROW_H, idx = scroll + i, hl = e->sel && !drag_active, target = idx == drop_row;
        if (target)
            gfx_round_rect(s, sw + U(6), ry + 1, W - sw - U(12), ROW_H - 2, U(5), C_ACCENT, 255);
        else if (e->sel)
            gfx_round_rect(s, sw + U(6), ry + 1, W - sw - U(12), ROW_H - 2, U(5), focus && hl ? C_ACCENT : 0xDCDCE0, 255);
        else if (idx == hover_row)
            gfx_round_rect(s, sw + U(6), ry + 1, W - sw - U(12), ROW_H - 2, U(5), 0xEEF1F7, 255);
        else if (i % 2)
            gfx_fill(s, sw, ry, W - sw, ROW_H, 0xF8F8FA);
        int white = target || (e->sel && focus && hl);
        int isz = ROW_H - U(8), iy = ry + U(4);
        entry_icon(s, e->name, e->is_dir, sw + U(14), iy, isz);
        u32 tc = white ? 0xFFFFFF : C_TEXT, tc2 = white ? 0xEAF2FF : C_TEXT2;
        int ty = ry + (ROW_H - text_height(font_ui, FS)) / 2;
        int name_end = (show_date() ? col_date() : col_size()) - U(10);
        gfx_set_clip(sw, ly, name_end - sw, lh);
        text_draw(s, font_ui, FS, sw + U(40), ty, e->name, tc);
        gfx_set_clip(sw, ly, W - sw, lh);
        if (show_date() && e->mtime) {
            DateTime dt;
            time_to_date(e->mtime, &dt);
            char d[32];
            snprintf(d, sizeof(d), "%02d.%02d.%04d %02d:%02d", dt.day, dt.month, dt.year, dt.hour, dt.min);
            text_draw(s, font_ui, FS, col_date(), ty, d, tc2);
        }
        char sz[24];
        if (e->is_dir)
            snprintf(sz, sizeof(sz), "\xE2\x80\x94");
        else
            fmt_size(e->size, sz, sizeof(sz));
        text_draw(s, font_ui, FS, col_size(), ty, sz, tc2);
    }
    if (!nent) {
        const char *m = query[0] ? "Nichts gefunden" : "Ordner ist leer";
        text_draw(s, font_ui, FS, sw + (W - sw - text_width(font_ui, FS, m)) / 2, ly + U(30), m, 0xAEAEB2);
    }
    gfx_no_clip();
    ui_scrollbar(s, W, ly, lh, nent, visible(), scroll);
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    int W = s->w, H = s->h, sw = side_w();
    nhits = 0;
    gfx_fill(s, 0, 0, W, H, C_WINDOW);
    draw_side(s, H);
    draw_tabs(s, W);
    draw_nav(s, W);
    if (dir[0])
        draw_list(s, W);
    else
        draw_home(s, W, H);

    /* Fusszeile */
    int fy = H - foot_h();
    gfx_fill(s, sw, fy, W - sw, foot_h(), 0xFAFAFC);
    gfx_fill(s, sw, fy, W - sw, 1, 0xE5E5EA);
    char t[300];
    if (status[0] && sys_time_us() - status_t < 6000000) {
        snprintf(t, sizeof(t), "%s", status);
    } else if (!dir[0]) {
        snprintf(t, sizeof(t), query[0] ? "%d Treffer auf der Platte" : "%d Dateien und Ordner auf der Platte",
                 query[0] ? nresults : nfound);
    } else {
        char free_s[32] = "";
        if (free_bytes >= 0) { /* beim Laden des Ordners bestimmt, nicht bei jedem Bild */
            char b[24];
            fmt_size((u64)free_bytes, b, sizeof(b));
            snprintf(free_s, sizeof(free_s), "  \xC2\xB7  %s frei", b);
        }
        int n = nsel();
        if (n)
            snprintf(t, sizeof(t), "%d von %d ausgew\xC3\xA4hlt%s", n, nent, free_s);
        else if (query[0])
            snprintf(t, sizeof(t), "%d %s f\xC3\xBCr \xE2\x80\x9E%s\xE2\x80\x9C%s", nent, "Treffer", query, free_s);
        else
            snprintf(t, sizeof(t), "%d %s%s", nent, nent == 1 ? "Objekt" : "Objekte", free_s);
    }
    text_draw(s, font_ui, FS_SMALL, sw + U(12), fy + (foot_h() - text_height(font_ui, FS_SMALL)) / 2, t, C_TEXT2);

    /* Ziehen: kleines Schild am Zeiger */
    if (drag_active) {
        int n = nsel();
        char lab[280];
        if (n == 1) {
            for (int i = 0; i < nent; i++)
                if (ents[i].sel)
                    snprintf(lab, sizeof(lab), "%s", ents[i].name);
        } else {
            snprintf(lab, sizeof(lab), "%d Objekte", n);
        }
        int lw = text_width(font_ui, FS, lab) + U(40), lh2 = U(26), lx = drag_x + U(12), lyy = drag_y + U(8);
        gfx_shadow(s, lx, lyy + 2, lw, lh2, U(6), U(10), 60);
        gfx_round_rect(s, lx, lyy, lw, lh2, U(6), 0xFFFFFF, 225);
        ui_doc_icon(s, lx + U(6), lyy + U(4), U(18), 0);
        text_draw(s, font_ui, FS, lx + U(30), lyy + (lh2 - text_height(font_ui, FS)) / 2, lab, C_TEXT);
    }

    /* Kontextmenue bzw. Menue der Menuezeile */
    if (nmenu) {
        int mw, mh;
        float k = (float)U(1);
        menu_size(&mw, &mh);
        gfx_shadow(s, menu_x, menu_y + U(4), mw, mh, U(8), U(18), 60);
        gfx_round_rect(s, menu_x, menu_y, mw, mh, U(8), 0xF6F6F8, 250);
        gfx_round_frame(s, menu_x, menu_y, mw, mh, U(8), 0x000000, 30);
        for (int i = 0; i < nmenu; i++) {
            int iy = menu_y + U(5) + i * U(24);
            if (!menu[i].label) {
                gfx_blend_fill(s, menu_x + U(10), iy + U(11), mw - U(20), 1, 0x000000, 30);
                continue;
            }
            int hv = i == menu_hover && menu[i].enabled;
            if (hv)
                gfx_round_rect(s, menu_x + U(5), iy, mw - U(10), U(24), U(5), C_ACCENT, 255);
            u32 c = !menu[i].enabled ? 0xB8B8BE : hv ? 0xFFFFFF : C_TEXT;
            int ty = iy + (U(24) - text_height(font_ui, FS)) / 2;
            if (menu[i].checked)
                check_icon(s, menu_x + U(16), iy + U(12), k * 0.9f, c);
            text_draw(s, font_ui, FS, menu_x + U(30), ty, menu[i].label, c);
            if (menu[i].keys)
                text_draw(s, font_ui, FS, menu_x + mw - U(14) - text_width(font_ui, FS, menu[i].keys), ty, menu[i].keys,
                          hv ? 0xFFFFFF : 0x8E8E93);
        }
    }
    if (dialog)
        draw_dialog(s);
    gfx_present_all();
}

static void busy(const char *what)
{
    say("%s", what);
    draw();
    gfx_flush();
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Eingaben
 * ------------------------------------------------------------------------------------------------------------------- */

static void keep_visible(void)
{
    int vis = visible();
    if (cur >= 0 && cur < scroll)
        scroll = cur;
    if (cur >= scroll + vis)
        scroll = cur - vis + 1;
    if (scroll > nent - vis)
        scroll = nent - vis;
    if (scroll < 0)
        scroll = 0;
}

static void add_item(const char *label, const char *keys, int act, int en)
{
    menu[nmenu].label = label;
    menu[nmenu].keys = keys;
    menu[nmenu].action = act;
    menu[nmenu].checked = 0;
    menu[nmenu++].enabled = en;
}

static void place_menu(int x, int y)
{
    int mw, mh;
    menu_size(&mw, &mh);
    menu_x = x + mw > gfx_screen.w ? gfx_screen.w - mw - 2 : x;
    menu_y = y + mh > gfx_screen.h ? gfx_screen.h - mh - 2 : y;
    if (menu_x < 0) menu_x = 0;
    if (menu_y < 0) menu_y = 0;
}

static int can_paste_now(void)
{
    char *t;
    int cut, ok = clip_paths(&t, &cut);
    if (ok)
        u_free(t);
    return ok && dir[0];
}

static void open_menu(int x, int y, int on_item)
{
    nmenu = 0;
    menu_hover = -1;
    dropdown = 0;
    int can_paste = can_paste_now();
    if (on_item) {
        add_item("\xC3\x96" "ffnen", "Enter", M_OPEN, 1);
        add_item("In neuem Tab \xC3\xB6" "ffnen", 0, M_OPENTAB, cur >= 0 && cur < nent && ents[cur].is_dir);
        add_item(0, 0, 0, 0);
        add_item("Umbenennen \xE2\x80\xA6", 0, M_RENAME, nsel() == 1);
        add_item("Duplizieren", "Strg+D", M_DUP, 1);
        add_item("Kopieren", "Strg+C", M_COPY, 1);
        add_item("Ausschneiden", "Strg+X", M_CUT, 1);
        add_item("Einf\xC3\xBCgen", "Strg+V", M_PASTE, can_paste);
        add_item(0, 0, 0, 0);
        add_item("L\xC3\xB6schen \xE2\x80\xA6", "Entf", M_DELETE, 1);
    } else {
        add_item("Neuer Ordner", "Strg+N", M_NEWFOLDER, 1);
        add_item("Neue Textdatei", 0, M_NEWFILE, 1);
        add_item(0, 0, 0, 0);
        add_item("Einf\xC3\xBCgen", "Strg+V", M_PASTE, can_paste);
    }
    place_menu(x, y);
}

/* Menues der Menuezeile */
static void open_dropdown(int i, int x)
{
    nmenu = 0;
    menu_hover = -1;
    int real = dir[0] != 0, n = nsel();
    if (i == 0) {
        add_item("Neuer Tab", "Strg+T", M_NEWTAB, ntabs < MAXTABS);
        add_item("Neuer Ordner", "Strg+N", M_NEWFOLDER, real);
        add_item("Neue Textdatei", 0, M_NEWFILE, real);
        add_item(0, 0, 0, 0);
        add_item("\xC3\x96" "ffnen", "Enter", M_OPEN, n > 0);
        add_item(0, 0, 0, 0);
        add_item("Tab schlie\xC3\x9F" "en", "Strg+W", M_CLOSETAB, ntabs > 1);
    } else if (i == 1) {
        add_item("Ausschneiden", "Strg+X", M_CUT, n > 0);
        add_item("Kopieren", "Strg+C", M_COPY, n > 0);
        add_item("Einf\xC3\xBCgen", "Strg+V", M_PASTE, can_paste_now());
        add_item("Duplizieren", "Strg+D", M_DUP, n > 0);
        add_item(0, 0, 0, 0);
        add_item("Umbenennen \xE2\x80\xA6", 0, M_RENAME, n == 1);
        add_item("L\xC3\xB6schen \xE2\x80\xA6", "Entf", M_DELETE, n > 0);
        add_item(0, 0, 0, 0);
        add_item("Alles ausw\xC3\xA4hlen", "Strg+A", M_SELALL, real && nent > 0);
    } else {
        add_item("Schnellzugriff", 0, M_HOME, 1);
        menu[nmenu - 1].checked = !dir[0];
        add_item("Aktualisieren", "Strg+R", M_REFRESH, 1);
        add_item(0, 0, 0, 0);
        add_item("Nach Name sortieren", 0, M_SORT_NAME, real);
        menu[nmenu - 1].checked = sort_mode == 0;
        add_item("Nach Datum sortieren", 0, M_SORT_DATE, real);
        menu[nmenu - 1].checked = sort_mode == 1;
        add_item("Nach Gr\xC3\xB6\xC3\x9F" "e sortieren", 0, M_SORT_SIZE, real);
        menu[nmenu - 1].checked = sort_mode == 2;
    }
    dropdown = i + 1;
    place_menu(x, tabs_h() + mrow_h() - U(2));
}

static int menu_at(int px, int py)
{
    int mw, mh;
    menu_size(&mw, &mh);
    if (px < menu_x || px >= menu_x + mw || py < menu_y || py >= menu_y + mh)
        return -2; /* ausserhalb */
    int i = (py - menu_y - U(5)) / U(24);
    return i >= 0 && i < nmenu && menu[i].label ? i : -1;
}

static int dialog_btn_at(int px, int py)
{
    for (int i = 0; i < 2; i++) {
        int bx, by, bw, bh;
        dialog_btn(i, &bx, &by, &bw, &bh);
        if (px >= bx && px < bx + bw && py >= by && py < by + bh)
            return i;
    }
    return -1;
}

static void dialog_key(int k)
{
    if (k == 0x1B) {
        dialog = D_NONE;
    } else if (k == '\n') {
        dialog_ok();
    } else if (dialog != D_DELETE && (k == '\b' || k == 0x7F)) {
        while (flen > 0 && (field[flen - 1] & 0xC0) == 0x80)
            flen--;
        if (flen > 0)
            flen--;
        field[flen] = 0;
    } else if (dialog != D_DELETE && k >= 32 && k < 0xF5 && k != '/' && flen < (int)sizeof(field) - 1) {
        field[flen++] = (char)k;
        field[flen] = 0;
    }
}

static void apply_query(void)
{
    if (dir[0])
        load(0);
    else
        home_search();
}

static void open_found(const Found *f)
{
    if (f->is_dir) {
        go(f->path, 1);
    } else if (gfx_desktop_open(f->path) == 0) {
        say("%s wird ge\xC3\xB6" "ffnet", base_of(f->path));
    } else {
        say("%s kann ohne Desktop nicht ge\xC3\xB6" "ffnet werden", base_of(f->path));
    }
}

/* Tasten im Suchfeld; 1 = verbraucht */
static int search_key(int k)
{
    int len = (int)strlen(query);
    if (k == 0x1B) {
        query[0] = 0;
        search_focus = 0;
        apply_query();
    } else if (k == '\n') {
        search_focus = 0;
        if (!dir[0] && nresults)
            open_found(&found[results[0]]);
    } else if (k == '\b' || k == 0x7F) {
        while (len > 0 && (query[len - 1] & 0xC0) == 0x80)
            len--;
        if (len > 0)
            len--;
        query[len] = 0;
        apply_query();
    } else if (k >= 32 && k < 0xF5 && k != 0x7F && len < (int)sizeof(query) - 1) {
        query[len] = (char)k;
        query[len + 1] = 0;
        apply_query();
    } else if (KEY_BASE(k) == KEY_DOWN) {
        search_focus = 0;
        return 0; /* weiter in die Liste */
    } else {
        return 0;
    }
    return 1;
}

static void key(int k)
{
    int base = KEY_BASE(k), shift = (k & KEY_MOD_SHIFT) != 0;
    if (k & KEY_MOD_ALT)
        return;
    if (base == '\t' && (k & KEY_MOD_CTRL)) { /* Strg+Tab: naechster Tab */
        tab_switch((cur_tab + (shift ? ntabs - 1 : 1)) % ntabs);
        return;
    }
    switch (k) { /* Strg+Buchstabe kommt als Steuerzeichen */
    case 20: action(M_NEWTAB); return;    /* Strg+T */
    case 23: action(M_CLOSETAB); return;  /* Strg+W */
    case 6: search_focus = 1; return;     /* Strg+F */
    case 18: action(M_REFRESH); return;   /* Strg+R */
    }
    if (search_focus && search_key(k))
        return;
    if ((base == KEY_UP || base == KEY_DOWN || base == KEY_HOME || base == KEY_END || base == KEY_PGUP || base == KEY_PGDN) &&
        nent) {
        int c = cur < 0 ? (base == KEY_UP || base == KEY_END ? nent : -1) : cur;
        if (base == KEY_UP) c--;
        else if (base == KEY_DOWN) c++;
        else if (base == KEY_HOME) c = 0;
        else if (base == KEY_END) c = nent - 1;
        else if (base == KEY_PGUP) c -= visible();
        else c += visible();
        if (c < 0) c = 0;
        if (c >= nent) c = nent - 1;
        if (shift && anchor >= 0) {
            select_range(anchor, c);
            cur = c;
        } else {
            select_only(c);
        }
        keep_visible();
        return;
    }
    if (k & (KEY_MOD_SHIFT | KEY_MOD_CTRL))
        return;
    switch (k) {
    case '\n': action(M_OPEN); return;
    case '\b':
    case 0x7F: go_up(); return;
    case KEY_DEL: action(M_DELETE); return;
    case 1: action(M_SELALL); return;      /* Strg+A */
    case 3: action(M_COPY); return;        /* Strg+C */
    case 24: action(M_CUT); return;        /* Strg+X */
    case 22: action(M_PASTE); return;      /* Strg+V */
    case 4: if (nsel()) duplicate(); return; /* Strg+D */
    case 14: action(M_NEWFOLDER); return;  /* Strg+N */
    }
    if (k > ' ' && k < 0x7F) { /* Tippen springt zum ersten passenden Namen */
        for (int i = 0; i < nent; i++) {
            char c = ents[i].name[0];
            if ((c | 0x20) == (k | 0x20)) {
                select_only(i);
                keep_visible();
                return;
            }
        }
    }
}

static void side_click(int i, int chev)
{
    if (i < 0 || i >= nnodes)
        return;
    Node *n = &nodes[i];
    if (chev) { /* auf- bzw. zuklappen */
        if (n->place >= 0)
            place_open[n->place] = !place_open[n->place];
        else if (!n->path[0])
            qa_open = !qa_open;
        build_side();
        return;
    }
    if (n->place >= 0 && !place_open[n->place]) { /* Ort anklicken klappt ihn auch auf */
        place_open[n->place] = 1;
        build_side();
    }
    char p[256];
    snprintf(p, sizeof(p), "%s", n->path);
    go(p, 1);
}

static void click(Event *e, s64 *last_click, int *last_i)
{
    const Hit *h = hit_at(e->x, e->y);
    if (h && h->kind != H_SEARCH)
        search_focus = 0;
    if (h) {
        switch (h->kind) {
        case H_DRAG: { /* Fenster verschieben, Doppelklick maximiert */
            static s64 last_drag;
            s64 now = sys_ticks();
            if (now - last_drag < 40) {
                last_drag = 0;
                gfx_window_cmd(GFX_WIN_ZOOM);
            } else {
                last_drag = now;
                gfx_window_cmd(GFX_WIN_MOVE);
            }
            return;
        }
        case H_WMIN: gfx_window_cmd(GFX_WIN_MINIMIZE); return;
        case H_WMAX: gfx_window_cmd(GFX_WIN_ZOOM); return;
        case H_WCLOSE: gfx_window_cmd(GFX_WIN_CLOSE); return;
        case H_TAB: tab_switch(h->idx); return;
        case H_TABX: tab_close(h->idx); return;
        case H_TABNEW: action(M_NEWTAB); return;
        case H_MENU: open_dropdown(h->idx, h->x); return;
        case H_BACK: go_back(); return;
        case H_FWD: go_forward(); return;
        case H_UP: if (dir[0]) go_up(); return;
        case H_CRUMB: {
            char p[256];
            snprintf(p, sizeof(p), "%s", crumb_path[h->idx]);
            if (strcmp(p, dir) != 0)
                go(p, 1);
            return;
        }
        case H_REFRESH: action(M_REFRESH); return;
        case H_SEARCH: search_focus = 1; return;
        case H_SIDE: side_click(h->idx, 0); return;
        case H_SIDECHEV: side_click(h->idx, 1); return;
        case H_TILE: {
            const char *p = tile_path(h->idx);
            if (p) {
                char q[256];
                snprintf(q, sizeof(q), "%s", p);
                go(q, 1);
            }
            return;
        }
        case H_RECENT:
            if (query[0] && h->idx < nresults)
                open_found(&found[results[h->idx]]);
            else if (!query[0] && h->idx < nrecent)
                open_found(&found[recent[h->idx]]);
            return;
        case H_COL:
            sort_mode = h->idx;
            load(1);
            return;
        }
    }
    search_focus = 0;
    int i = row_at(e->x, e->y);
    if (i == -2)
        return;
    if (i == -1) { /* leere Flaeche: Auswahl weg */
        if (!(e->key & (KEY_MOD_CTRL | KEY_MOD_SHIFT)))
            select_only(-1);
        return;
    }
    s64 now = sys_ticks();
    int dbl = i == *last_i && now - *last_click < 40 && !(e->key & (KEY_MOD_CTRL | KEY_MOD_SHIFT));
    *last_click = dbl ? 0 : now;
    *last_i = i;
    if (e->key & KEY_MOD_CTRL) {
        ents[i].sel = !ents[i].sel;
        cur = anchor = i;
    } else if ((e->key & KEY_MOD_SHIFT) && anchor >= 0) {
        select_range(anchor, i);
        cur = i;
    } else {
        if (!ents[i].sel)
            select_only(i);
        cur = anchor = i;
        drag_pending = 1; /* vielleicht wird gezogen */
        press_x = e->x;
        press_y = e->y;
    }
    if (dbl) {
        drag_pending = 0;
        select_only(i);
        open_entry(i);
    }
}

static int side_node_at(int px, int py)
{
    const Hit *h = hit_at(px, py);
    return h && (h->kind == H_SIDE || h->kind == H_SIDECHEV) ? h->idx : -1;
}

void _start(int argc, char **argv)
{
    char start[256] = "";
    if (argc > 1)
        snprintf(start, sizeof(start), "%s", argv[1]);
    ui_setup(0);
    if (gfx_open_window_ex(U(980), U(640), "Dateien", GFX_RESIZABLE | GFX_FRAMELESS) != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    load_places();
    load_quick();
    build_side();
    tab_new(start);
    draw();
    s64 last_click = 0, last_refresh = sys_time_us();
    int last_i = -1;
    for (;;) {
        Event e;
        if (!gfx_wait(&e, 500)) {
            if (status[0] && sys_time_us() - status_t > 6000000) { /* Meldung ist alt: wieder die Uebersicht */
                status[0] = 0;
                draw();
            }
            continue;
        }
        /* alle wartenden Ereignisse (z.B. eine gehaltene Taste) abarbeiten und dann einmal zeichnen */
        int quit = 0, nev = 0;
        do {
            if (e.type == EV_CLOSE) {
                quit = 1;
                break;
            }
            if (e.type == EV_FOCUS) {
                focus = e.key;
                if (focus && sys_time_us() - last_refresh > 1000000) { /* zurueck im Fenster: vielleicht hat sich etwas geaendert */
                    last_refresh = sys_time_us();
                    if (dir[0])
                        load(1);
                    else
                        home_scan(0);
                    load_places();
                    load_quick();
                    build_side();
                }
            } else if (e.type == EV_RESIZE) {
                keep_visible();
            } else if (dialog) {
                if (e.type == EV_KEY) {
                    dialog_key(e.key);
                } else if (e.type == EV_DOWN && e.button == 1) {
                    int b = dialog_btn_at(e.x, e.y);
                    if (b == 0)
                        dialog = D_NONE;
                    else if (b == 1)
                        dialog_ok();
                } else if (e.type == EV_MOVE) {
                    int b = dialog_btn_at(e.x, e.y);
                    if (b == dialog_hover)
                        continue;
                    dialog_hover = b;
                }
            } else if (nmenu) { /* Menue offen */
                if (e.type == EV_MOVE) {
                    int h = menu_at(e.x, e.y);
                    if (h == menu_hover)
                        continue;
                    menu_hover = h;
                } else if (e.type == EV_DOWN) {
                    int h = menu_at(e.x, e.y);
                    int act = h >= 0 && menu[h].enabled ? menu[h].action : 0;
                    int was = dropdown;
                    nmenu = 0;
                    dropdown = 0;
                    if (act) {
                        action(act);
                    } else if (h == -2) { /* daneben: anderes Menue der Menuezeile gleich oeffnen */
                        const Hit *hh = hit_at(e.x, e.y);
                        if (hh && hh->kind == H_MENU && hh->idx + 1 != was)
                            open_dropdown(hh->idx, hh->x);
                    }
                } else if (e.type == EV_KEY && e.key == 0x1B) {
                    nmenu = 0;
                    dropdown = 0;
                }
            } else if (e.type == EV_KEY) {
                key(e.key);
            } else if (e.type == EV_DOWN && e.button == 1) {
                click(&e, &last_click, &last_i);
            } else if (e.type == EV_DOWN && e.button == 2) { /* Rechtsklick: Kontextmenue */
                int i = row_at(e.x, e.y);
                if (i == -2)
                    continue;
                if (i >= 0 && !ents[i].sel)
                    select_only(i);
                open_menu(e.x, e.y, i >= 0);
            } else if (e.type == EV_MOVE) {
                if (drag_pending && !drag_active) {
                    int dx = e.x - press_x, dy = e.y - press_y;
                    if (dx * dx + dy * dy > U(6) * U(6))
                        drag_active = 1;
                }
                if (drag_active) {
                    drag_x = e.x;
                    drag_y = e.y;
                    int r = row_at(e.x, e.y);
                    drop_row = r >= 0 && ents[r].is_dir && !ents[r].sel ? r : -1;
                    drop_place = side_node_at(e.x, e.y);
                    if (drop_place >= 0 && (!nodes[drop_place].path[0] || strcmp(nodes[drop_place].path, dir) == 0))
                        drop_place = -1;
                    if (e.y < list_y() + ROW_H && scroll > 0) /* am Rand: mitscrollen */
                        scroll--;
                    else if (e.y > list_y() + list_h() - ROW_H && scroll + visible() < nent)
                        scroll++;
                } else {
                    const Hit *h = hit_at(e.x, e.y);
                    int hk = h ? h->kind : H_NONE, hi = h ? h->idx : -1, hr = row_at(e.x, e.y);
                    if (hk == hov_kind && hi == hov_idx && hr == hover_row)
                        continue;
                    hov_kind = hk;
                    hov_idx = hi;
                    hover_row = hr;
                }
            } else if (e.type == EV_UP) {
                if (drag_active) {
                    char target[512] = "";
                    if (drop_row >= 0)
                        join(target, sizeof(target), dir, ents[drop_row].name);
                    else if (drop_place >= 0)
                        snprintf(target, sizeof(target), "%s", nodes[drop_place].path);
                    drag_active = 0;
                    drop_row = drop_place = -1;
                    if (target[0])
                        drop_on(target, (e.key & KEY_MOD_CTRL) != 0);
                } else if (drag_pending && !(e.key & (KEY_MOD_CTRL | KEY_MOD_SHIFT)) && cur >= 0) {
                    select_only(cur); /* Klick auf eine Auswahl ohne Ziehen: nur diesen */
                }
                drag_pending = 0;
            } else if (e.type == EV_WHEEL) {
                scroll -= e.wheel * 3;
                if (scroll > nent - visible()) scroll = nent - visible();
                if (scroll < 0) scroll = 0;
            }
        } while (++nev < 64 && gfx_poll(&e));
        if (quit)
            break;
        draw();
    }
    gfx_close();
    sys_exit(0);
}

static void tab_close_current(void)
{
    tab_close(cur_tab);
}
