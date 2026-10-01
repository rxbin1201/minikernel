#include "gfx.h"
#include "malloc.h"
#include "ui.h"

/* files [ordner]: Dateien verwalten (Fenster mit aenderbarer Groesse).
 *   Links die Orte (Platte /disk, System /, angesteckte Datentraeger), oben Zurueck/Vor/Hoch und Neuer Ordner,
 *   Umbenennen, Loeschen. Doppelklick oder Enter oeffnet: Ordner hier, Dateien im passenden Programm (der Desktop
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
enum { M_OPEN = 1, M_RENAME, M_DUP, M_COPY, M_CUT, M_PASTE, M_DELETE, M_NEWFOLDER, M_NEWFILE };
static struct {
    const char *label, *keys;
    int         action, enabled;
} menu[10];
static int nmenu, menu_x, menu_y, menu_hover = -1;

/* Ziehen */
static int drag_pending, drag_active, drag_x, drag_y, press_x, press_y, drop_row = -1, drop_place = -1;
static int hover_btn = -1;

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

static int cmp_ent(const Ent *a, const Ent *b) /* Ordner zuerst, dann nach Namen */
{
    if (a->is_dir != b->is_dir)
        return b->is_dir - a->is_dir;
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
    free_bytes = sys_statfs(dir, fsz) == 0 ? (s64)fsz[1] : -1;
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
    gfx_set_title(strcmp(dir, "/") == 0 ? "System" : strcmp(dir, "/disk") == 0 ? "Platte" : base_of(dir));
}

static void go(const char *p, int remember)
{
    Stat st;
    if (sys_stat(p, &st) != 0 || !st.is_dir) {
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
    load(0);
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
    if (strcmp(dir, "/") == 0)
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
    }
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Aussehen
 * ------------------------------------------------------------------------------------------------------------------- */

static int tb_h(void) { return U(44); }
static int side_w(void) { return U(170); }
static int head_h(void) { return U(26); }
static int foot_h(void) { return U(24); }
static int list_y(void) { return tb_h() + head_h(); }
static int list_h(void) { return gfx_screen.h - list_y() - foot_h(); }
static int visible(void) { int v = list_h() / ROW_H; return v < 1 ? 1 : v; }
static int col_date(void) { return gfx_screen.w - U(250); }
static int col_size(void) { return gfx_screen.w - U(92); }

/* Werkzeugleiste: 0 zurueck, 1 vor, 2 hoch, dann rechts 3 neuer Ordner, 4 umbenennen, 5 loeschen */
static const char *const tb_label[6] = {"\xE2\x86\x90", "\xE2\x86\x92", "\xE2\x86\x91", "Neuer Ordner", "Umbenennen", "L\xC3\xB6schen"};

static void tb_rect(int i, int *x, int *y, int *w, int *h)
{
    *y = U(8);
    *h = tb_h() - U(16);
    if (i < 3) {
        *w = U(30);
        *x = U(12) + i * (U(30) + U(4));
        return;
    }
    int right = gfx_screen.w - U(12);
    for (int k = 5; k >= i; k--) {
        *w = text_width(font_ui, FS, tb_label[k]) + U(22);
        right -= *w;
        *x = right;
        right -= U(6);
    }
}

static int tb_enabled(int i)
{
    switch (i) {
    case 0: return nback > 0;
    case 1: return nfwd > 0;
    case 2: return strcmp(dir, "/") != 0;
    case 4: return nsel() == 1;
    case 5: return nsel() > 0;
    default: return 1;
    }
}

static int tb_at(int px, int py)
{
    for (int i = 0; i < 6; i++) {
        int x, y, w, h;
        tb_rect(i, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h)
            return i;
    }
    return -1;
}

static int place_y(int i) { return tb_h() + U(34) + i * U(28); }

static int place_at(int px, int py)
{
    if (px >= side_w())
        return -1;
    for (int i = 0; i < nplaces; i++)
        if (py >= place_y(i) && py < place_y(i) + U(26))
            return i;
    return -1;
}

static int row_at(int px, int py)
{
    if (px < side_w() || py < list_y() || py >= list_y() + list_h())
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
        if (inside(dir, places[i].path) && (best < 0 || l > bl)) {
            best = i;
            bl = l;
        }
    }
    return best;
}

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

static void menu_size(int *w, int *h)
{
    *w = U(230);
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

static void draw(void)
{
    Surface *s = &gfx_screen;
    int W = s->w, H = s->h, sw = side_w(), ly = list_y(), lh = list_h();
    gfx_fill(s, 0, 0, W, H, C_WINDOW);

    /* Werkzeugleiste */
    gfx_gradient(s, 0, 0, W, tb_h(), 0xF7F7F8, 0xEDEDF0);
    gfx_fill(s, 0, tb_h() - 1, W, 1, 0xDDDDE0);
    for (int i = 0; i < 6; i++) {
        int x, y, w, h, en = tb_enabled(i);
        tb_rect(i, &x, &y, &w, &h);
        if (i >= 3 && x < U(220))
            continue; /* zu schmal */
        gfx_round_rect(s, x, y, w, h, U(6), i == hover_btn && en ? 0xDCDCE2 : 0xFFFFFF, 255);
        gfx_round_frame(s, x, y, w, h, U(6), 0x000000, 28);
        int fs = i < 3 ? U(16) : FS;
        text_draw(s, font_ui, fs, x + (w - text_width(font_ui, fs, tb_label[i])) / 2, y + (h - text_height(font_ui, fs)) / 2,
                  tb_label[i], en ? C_TEXT : 0xB8B8BE);
    }
    int tx = U(12) + 3 * (U(30) + U(4)) + U(8);
    const char *title = strcmp(dir, "/") == 0 ? "System" : strcmp(dir, "/disk") == 0 ? "Platte" : base_of(dir);
    text_draw(s, font_bold, U(15), tx, (tb_h() - text_height(font_bold, U(15))) / 2, title, C_TEXT);

    /* Orte */
    gfx_fill(s, 0, tb_h(), sw, H - tb_h(), 0xF2F2F5);
    gfx_fill(s, sw - 1, tb_h(), 1, H - tb_h(), 0xE0E0E4);
    text_draw(s, font_bold, FS_SMALL, U(14), tb_h() + U(12), "Orte", 0x8E8E93);
    int cp = current_place();
    for (int i = 0; i < nplaces; i++) {
        int py = place_y(i);
        if (i == drop_place)
            gfx_round_rect(s, U(6), py, sw - U(12), U(26), U(6), C_ACCENT, 255);
        else if (i == cp)
            gfx_round_rect(s, U(6), py, sw - U(12), U(26), U(6), 0xDCDCE2, 255);
        place_icon(s, places[i].kind, U(14), py + U(5), U(16));
        gfx_set_clip(0, py, sw - U(8), U(26));
        text_draw(s, font_ui, FS, U(38), py + (U(26) - text_height(font_ui, FS)) / 2, places[i].label,
                  i == drop_place ? 0xFFFFFF : C_TEXT);
        gfx_no_clip();
    }

    /* Kopfzeile der Liste */
    gfx_fill(s, sw, tb_h(), W - sw, head_h(), 0xFAFAFA);
    gfx_fill(s, sw, ly - 1, W - sw, 1, 0xE5E5E5);
    int hy = tb_h() + (head_h() - text_height(font_bold, FS_SMALL)) / 2;
    text_draw(s, font_bold, FS_SMALL, sw + U(40), hy, "Name", C_TEXT2);
    if (col_date() > sw + U(200))
        text_draw(s, font_bold, FS_SMALL, col_date(), hy, "Ge\xC3\xA4ndert", C_TEXT2);
    text_draw(s, font_bold, FS_SMALL, col_size(), hy, "Gr\xC3\xB6\xC3\x9F" "e", C_TEXT2);

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
        else if (i % 2)
            gfx_fill(s, sw, ry, W - sw, ROW_H, 0xF7F7F9);
        int white = target || (e->sel && focus && hl);
        int isz = ROW_H - U(8), iy = ry + U(4);
        if (e->is_dir)
            ui_folder_icon(s, sw + U(14), iy, isz);
        else
            ui_doc_icon(s, sw + U(14), iy, isz, ends_with(e->name, ".bmp") ? 0x34C759 : ends_with(e->name, ".sh") ? 0x0A84FF :
                        ends_with(e->name, ".wav") || ends_with(e->name, ".mp3") ? 0xFF2D55 :
                        ends_with(e->name, ".txt") ? 0xFFCC00 : 0);
        u32 tc = white ? 0xFFFFFF : C_TEXT, tc2 = white ? 0xEAF2FF : C_TEXT2;
        int ty = ry + (ROW_H - text_height(font_ui, FS)) / 2;
        int name_end = (col_date() > sw + U(200) ? col_date() : col_size()) - U(10);
        gfx_set_clip(sw, ly, name_end - sw, lh);
        text_draw(s, font_ui, FS, sw + U(40), ty, e->name, tc);
        gfx_set_clip(sw, ly, W - sw, lh);
        if (col_date() > sw + U(200) && e->mtime) {
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
    if (!nent)
        text_draw(s, font_ui, FS, sw + (W - sw - text_width(font_ui, FS, "Ordner ist leer")) / 2, ly + U(30), "Ordner ist leer", 0xAEAEB2);
    gfx_no_clip();
    ui_scrollbar(s, W, ly, lh, nent, visible(), scroll);

    /* Fusszeile */
    int fy = H - foot_h();
    gfx_fill(s, sw, fy, W - sw, foot_h(), 0xFAFAFA);
    gfx_fill(s, sw, fy, W - sw, 1, 0xE5E5E5);
    char t[300];
    if (status[0] && sys_time_us() - status_t < 6000000) {
        snprintf(t, sizeof(t), "%s", status);
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

    /* Kontextmenue */
    if (nmenu) {
        int mw, mh;
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
            text_draw(s, font_ui, FS, menu_x + U(14), ty, menu[i].label, c);
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
    menu[nmenu++].enabled = en;
}

static void open_menu(int x, int y, int on_item)
{
    nmenu = 0;
    menu_hover = -1;
    char *t;
    int cut, can_paste = clip_paths(&t, &cut);
    if (can_paste)
        u_free(t);
    if (on_item) {
        add_item("\xC3\x96" "ffnen", "Enter", M_OPEN, 1);
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
    int mw, mh;
    menu_size(&mw, &mh);
    menu_x = x + mw > gfx_screen.w ? gfx_screen.w - mw - 2 : x;
    menu_y = y + mh > gfx_screen.h ? gfx_screen.h - mh - 2 : y;
    if (menu_x < 0) menu_x = 0;
    if (menu_y < 0) menu_y = 0;
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

static void key(int k)
{
    int base = KEY_BASE(k), shift = (k & KEY_MOD_SHIFT) != 0;
    if (k & KEY_MOD_ALT)
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
    case 1: for (int i = 0; i < nent; i++) ents[i].sel = 1; return; /* Strg+A */
    case 3: clip_put(0); return;                                    /* Strg+C */
    case 24: clip_put(1); return;                                   /* Strg+X */
    case 22: paste(); return;                                       /* Strg+V */
    case 4: if (nsel()) duplicate(); return;                        /* Strg+D */
    case 14: open_dialog(D_NEWFOLDER); return;                      /* Strg+N */
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

static void click(Event *e, s64 *last_click, int *last_i)
{
    if (e->y < tb_h()) {
        int b = tb_at(e->x, e->y);
        if (b < 0 || !tb_enabled(b))
            return;
        switch (b) {
        case 0: go_back(); break;
        case 1: go_forward(); break;
        case 2: go_up(); break;
        case 3: open_dialog(D_NEWFOLDER); break;
        case 4: open_dialog(D_RENAME); break;
        case 5: open_dialog(D_DELETE); break;
        }
        return;
    }
    int p = place_at(e->x, e->y);
    if (p >= 0) {
        go(places[p].path, 1);
        return;
    }
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

void _start(int argc, char **argv)
{
    if (argc > 1)
        snprintf(dir, sizeof(dir), "%s", argv[1]);
    else if (!exists(dir))
        snprintf(dir, sizeof(dir), "/");
    ui_setup(0);
    if (gfx_open_window_ex(U(800), U(500), "Dateien", GFX_RESIZABLE) != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    load_places();
    load(0);
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
                    load(1);
                    load_places();
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
            } else if (nmenu) { /* Kontextmenue offen */
                if (e.type == EV_MOVE) {
                    int h = menu_at(e.x, e.y);
                    if (h == menu_hover)
                        continue;
                    menu_hover = h;
                } else if (e.type == EV_DOWN) {
                    int h = menu_at(e.x, e.y);
                    int act = h >= 0 && menu[h].enabled ? menu[h].action : 0;
                    nmenu = 0;
                    if (act)
                        action(act);
                } else if (e.type == EV_KEY && e.key == 0x1B) {
                    nmenu = 0;
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
                    drop_place = place_at(e.x, e.y);
                    if (drop_place >= 0 && strcmp(places[drop_place].path, dir) == 0)
                        drop_place = -1;
                    if (e.y < list_y() + ROW_H && scroll > 0) /* am Rand: mitscrollen */
                        scroll--;
                    else if (e.y > list_y() + list_h() - ROW_H && scroll + visible() < nent)
                        scroll++;
                } else {
                    int h = e.y < tb_h() ? tb_at(e.x, e.y) : -1;
                    if (h == hover_btn)
                        continue;
                    hover_btn = h;
                }
            } else if (e.type == EV_UP) {
                if (drag_active) {
                    char target[512] = "";
                    if (drop_row >= 0)
                        join(target, sizeof(target), dir, ents[drop_row].name);
                    else if (drop_place >= 0)
                        snprintf(target, sizeof(target), "%s", places[drop_place].path);
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
