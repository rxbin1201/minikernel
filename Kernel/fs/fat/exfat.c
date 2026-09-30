/* exFAT: Eintragssaetze lesen und schreiben, Operationen auf exFAT-Volumes */

#include "lib/string.h"
#include "core/syscall.h" /* ERR_* */
#include "drivers/rtc.h"
#include "lib/utf8.h"
#include "fs/fat/fat_internal.h"

/* ---------- exFAT: Eintragssaetze lesen ---------- */

typedef struct {
    char     name[256];
    int      is_dir;
    uint16_t attr;
    uint64_t size, valid;
    uint64_t mtime;
    uint32_t first;
    int      contig;
    FatXLoc  loc;
} XItem;

static void it_init_x(DirIter *it, uint32_t first, int contig, uint64_t size)
{
    uint32_t cb = cluster_bytes();
    it->fixed_root = 0;
    it->contig = contig;
    it->remaining = contig ? (uint32_t)((size + cb - 1) / cb) : 0;
    it->cluster = it->last_cluster = valid_cluster(first) && (!contig || it->remaining) ? first : 0;
    it->sector = it->index = 0;
    it->loaded = 0;
    it->ord = 0;
    it->xfirst = first;
    it->xcontig = contig;
    it->xsize = size;
}

static int x_entry(DirIter *it, uint8_t out[32])
{
    DirEntry *e;
    DirLoc l;
    int r = fat_it_next(it, &e, &l);
    if (r == 1)
        memcpy(out, e, 32);
    return r;
}

/* Naechste Datei bzw. das naechste Verzeichnis: ein Eintragssatz aus 0x85 (Datei), 0xC0 (Stream) und 0xC1 (Namen).
 * 1 = Eintrag, 0 = Ende, -1 = Fehler */
static int xdir_next(DirIter *it, XItem *out)
{
    uint8_t b[32], s[32];
    for (;;) {
        int r = x_entry(it, b);
        if (r <= 0)
            return r;
        if (b[0] == 0x00)
            return 0;
        if (b[0] != 0x85) /* Label, Bitmap, Upcase-Tabelle, geloeschte Saetze (Typ ohne Bit 7) */
            continue;
        uint32_t ord = it->ord - 1;
        int secondary = b[1];
        if (secondary < 2)
            continue;
        uint16_t attr = le16(b + 4);

        r = x_entry(it, s);
        if (r <= 0)
            return r;
        if (s[0] != 0xC0)
            continue;
        memset(out, 0, sizeof(*out));
        int nlen = s[3];
        out->attr = attr;
        out->mtime = dos_to_unix(le16(b + 14), le16(b + 12));
        out->is_dir = (attr & 0x10) != 0;
        out->contig = (s[1] & 2) != 0;
        out->valid = le64(s + 8);
        out->first = le32(s + 20);
        out->size = le64(s + 24);
        out->loc.dfirst = it->xfirst;
        out->loc.dcontig = it->xcontig;
        out->loc.dsize = it->xsize;
        out->loc.ord = ord;
        out->loc.nsec = secondary;

        uint16_t units[256];
        int n = 0, bad = 0;
        for (int k = 1; k < secondary; k++) {
            r = x_entry(it, s);
            if (r <= 0)
                return r;
            if (s[0] != 0xC1) {
                bad = 1;
                break;
            }
            for (int i = 0; i < 15 && n < nlen; i++)
                units[n++] = le16(s + 2 + i * 2);
        }
        if (bad || n == 0)
            continue;
        utf16_to_utf8(units, n, out->name, sizeof(out->name));
        return 1;
    }
}

static int x_find(uint32_t first, int contig, uint64_t size, const char *name, XItem *out)
{
    DirIter it;
    it_init_x(&it, first, contig, size);
    int r;
    while ((r = xdir_next(&it, out)) == 1)
        if (fat_iequal(out->name, name))
            return 1;
    return r < 0 ? -1 : 0;
}

/* Verzeichnis, in dem etwas angelegt oder geaendert wird */
typedef struct {
    uint32_t first;
    int      contig;
    uint64_t size;
    int      is_root;
    FatXLoc  self;          /* Eintragssatz des Verzeichnisses selbst (nicht bei der Wurzel) */
} XDirRef;

/* Zerlegt den Pfad in das Verzeichnis der letzten Komponente und deren Namen */
static int x_resolve_parent(const char *path, XDirRef *d, char comp[256], int *is_root)
{
    memset(d, 0, sizeof(*d));
    d->first = vol.root_cluster;
    d->is_root = 1;
    *is_root = 0;

    const char *p = path;
    while (*p == '/')
        p++;
    if (!*p) {
        *is_root = 1;
        return 0;
    }
    for (;;) {
        const char *end = p;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - p);
        if (len == 0 || len > 255)
            return ERR_INVAL;
        char name[256];
        memcpy(name, p, len);
        name[len] = 0;
        while (*end == '/')
            end++;

        if (!*end) {
            memcpy(comp, name, len + 1);
            return 0;
        }
        XItem it;
        int r = x_find(d->first, d->contig, d->size, name, &it);
        if (r < 0)
            return ERR_IO;
        if (r == 0)
            return ERR_NOENT;
        if (!it.is_dir)
            return ERR_NOTDIR;
        d->first = it.first;
        d->contig = it.contig;
        d->size = it.size;
        d->is_root = 0;
        d->self = it.loc;
        p = end;
    }
}

/* Sucht den Pfad. *is_root = 1 fuer "/" (dann ist *out nicht gesetzt). 0 oder negativer Fehler. */
static int x_walk(const char *path, XItem *out, int *is_root)
{
    XDirRef d;
    char comp[256];
    int r = x_resolve_parent(path, &d, comp, is_root);
    if (r != 0 || *is_root)
        return r;
    r = x_find(d.first, d.contig, d.size, comp, out);
    return r < 0 ? ERR_IO : r == 0 ? ERR_NOENT : 0;
}

/* Liest Label und Bitmap-Eintrag aus dem Wurzelverzeichnis */
void exfat_scan_root(void)
{
    DirIter it;
    it_init_x(&it, vol.root_cluster, 0, 0);
    uint8_t b[32];
    int have_label = 0;
    vol.label[0] = 0;
    vol.xbm_first = 0;
    while (x_entry(&it, b) == 1) {
        if (b[0] == 0x00)
            break;
        if (b[0] == 0x83 && !have_label) {
            int n = b[1] > 11 ? 11 : b[1];
            uint16_t units[11];
            for (int i = 0; i < n; i++)
                units[i] = le16(b + 2 + i * 2);
            utf16_to_utf8(units, n, vol.label, sizeof(vol.label));
            have_label = 1;
        } else if (b[0] == 0x81 && !(b[1] & 1) && !vol.xbm_first) {
            vol.xbm_first = le32(b + 20);
            vol.xbm_bytes = le64(b + 24);
        }
    }
}

/* ---------- exFAT: Eintragssaetze schreiben ---------- */

static uint16_t x_checksum(const uint8_t *set, int entries)
{
    uint16_t chk = 0;
    for (int i = 0; i < entries * 32; i++) {
        if (i == 2 || i == 3)
            continue;
        chk = (uint16_t)((((chk & 1) << 15) | (chk >> 1)) + set[i]);
    }
    return chk;
}

/* Hash ueber die Grossbuchstaben-Form des Namens in UTF-16 (Bytes: niedrig, hoch) */
static uint16_t x_name_hash(const uint16_t *units, int n)
{
    uint16_t h = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = units[i];
        if (c < 0xD800 || c > 0xDFFF)
            c = uni_upper(c);
        h = (uint16_t)((((h & 1) << 15) | (h >> 1)) + (c & 0xFF));
        h = (uint16_t)((((h & 1) << 15) | (h >> 1)) + (c >> 8));
    }
    return h;
}

/* Baut einen Eintragssatz; liefert die Zahl der Eintraege */
static int x_build_set(uint8_t *set, const char *name, uint16_t attr, uint32_t first, uint64_t size, uint64_t valid,
                       int contig)
{
    uint16_t units[256];
    int len = utf8_to_utf16(name, units, 255);
    if (len < 0)
        len = 0;
    int names = (len + 14) / 15;
    int entries = 2 + names;
    memset(set, 0, (size_t)entries * 32);
    uint8_t *e0 = set, *e1 = set + 32;
    e0[0] = 0x85;
    e0[1] = (uint8_t)(1 + names);
    put16(e0 + 4, attr);
    uint16_t stamp_date, stamp_time;
    dos_now(&stamp_date, &stamp_time);
    uint32_t ts = ((uint32_t)stamp_date << 16) | stamp_time;
    put32(e0 + 8, ts);
    put32(e0 + 12, ts);
    put32(e0 + 16, ts);
    e1[0] = 0xC0;
    e1[1] = (uint8_t)(contig ? 0x03 : 0x01);
    e1[3] = (uint8_t)len;
    put16(e1 + 4, x_name_hash(units, len));
    put64(e1 + 8, valid);
    put32(e1 + 20, first);
    put64(e1 + 24, size);
    for (int k = 0; k < names; k++) {
        uint8_t *e = set + (2 + k) * 32;
        e[0] = 0xC1;
        for (int i = 0; i < 15; i++) {
            int idx = k * 15 + i;
            if (idx < len)
                put16(e + 2 + i * 2, units[idx]);
        }
    }
    put16(e0 + 2, x_checksum(set, entries));
    return entries;
}

static int x_read_set(const FatXLoc *l, uint8_t *set)
{
    DirIter it;
    it_init_x(&it, l->dfirst, l->dcontig, l->dsize);
    uint8_t e[32];
    for (uint32_t i = 0;; i++) {
        if (x_entry(&it, e) != 1)
            return -1;
        if (i >= l->ord) {
            memcpy(set + (i - l->ord) * 32, e, 32);
            if (i - l->ord == (uint32_t)l->nsec)
                return 0;
        }
    }
}

static int x_write_set(const FatXLoc *l, const uint8_t *set)
{
    DirIter it;
    it_init_x(&it, l->dfirst, l->dcontig, l->dsize);
    for (uint32_t i = 0;; i++) {
        DirEntry *e;
        DirLoc lc;
        if (fat_it_next(&it, &e, &lc) != 1)
            return -1;
        if (i >= l->ord) {
            if (fat_entry_write(&lc, (const DirEntry *)(set + (i - l->ord) * 32)) != 0)
                return -1;
            if (i - l->ord == (uint32_t)l->nsec)
                return 0;
        }
    }
}

/* Aendert Startcluster, Groesse und Flags eines Satzes (Pruefsumme wird neu berechnet) */
int exfat_update_set(const FatXLoc *l, uint32_t first, uint64_t size, uint64_t valid, int contig, int touch)
{
    uint8_t set[XSET_MAX * 32];
    if (l->nsec + 1 > XSET_MAX || x_read_set(l, set) != 0)
        return -1;
    uint8_t *e0 = set, *e1 = set + 32;
    if (touch) {
        put16(e0 + 4, (uint16_t)(le16(e0 + 4) | ATTR_ARCHIVE));
        uint16_t stamp_date, stamp_time;
        dos_now(&stamp_date, &stamp_time);
        put32(e0 + 12, ((uint32_t)stamp_date << 16) | stamp_time);
    }
    e1[1] = (uint8_t)(contig ? 0x03 : 0x01);
    put64(e1 + 8, valid);
    put32(e1 + 20, first);
    put64(e1 + 24, size);
    put16(e0 + 2, x_checksum(set, l->nsec + 1));
    return x_write_set(l, set);
}

static int x_delete_set(const FatXLoc *l)
{
    uint8_t set[XSET_MAX * 32];
    if (l->nsec + 1 > XSET_MAX || x_read_set(l, set) != 0)
        return -1;
    for (int i = 0; i <= l->nsec; i++)
        set[i * 32] &= 0x7F; /* InUse-Bit loeschen */
    return x_write_set(l, set);
}

/* Schreibt fuer n zusammenhaengende Cluster die FAT-Kette (wird gebraucht, sobald so eine Datei waechst) */
int exfat_make_chain(uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (fat_set(first + i, i + 1 < n ? first + i + 1 : 0xFFFFFFFFu) != 0)
            return -1;
    return 0;
}

/* Gibt die Daten einer Datei bzw. eines Verzeichnisses frei */
static void x_free_data(uint32_t first, uint64_t size, int contig)
{
    if (!valid_cluster(first))
        return;
    if (contig) {
        uint32_t cb = cluster_bytes();
        uint32_t n = (uint32_t)((size + cb - 1) / cb);
        for (uint32_t i = 0; i < n; i++)
            fat_xbm_put(first + i, 0);
        vol.free_hint = 2;
    } else {
        fat_free_chain(first);
    }
}

/* Haengt einen Cluster an ein Verzeichnis an. `last` = letzter Cluster der Kette. 0 = Fehler */
static uint32_t x_dir_grow(XDirRef *d, uint32_t last)
{
    uint32_t cb = cluster_bytes();
    if (d->contig) { /* zusammenhaengend gespeichert: erst in eine FAT-Kette umwandeln */
        uint32_t n = (uint32_t)((d->size + cb - 1) / cb);
        if (n == 0 || exfat_make_chain(d->first, n) != 0)
            return 0;
        last = d->first + n - 1;
        d->contig = 0;
    }
    uint32_t c = fat_alloc_cluster(last);
    if (!c)
        return 0;
    if (fat_zero_cluster(c) != 0)
        return 0;
    if (!d->is_root) {
        d->size += cb;
        if (exfat_update_set(&d->self, d->first, d->size, d->size, 0, 0) != 0)
            return 0;
    }
    return c;
}

/* Legt einen Eintragssatz im Verzeichnis an. 0 = ok */
static int x_add(XDirRef *d, const char *name, uint16_t attr, uint32_t first, uint64_t size, uint64_t valid,
                 int contig, FatXLoc *out)
{
    size_t len = strlen(name);
    if (!fat_name_valid(name, len))
        return ERR_INVAL;
    uint8_t set[XSET_MAX * 32];
    int need = x_build_set(set, name, attr, first, size, valid, contig);

    DirIter it;
    it_init_x(&it, d->first, d->contig, d->size);
    DirLoc locs[XSET_MAX];
    DirEntry *e;
    DirLoc l;
    int run = 0, r;
    uint32_t run_ord = 0;
    while ((r = fat_it_next(&it, &e, &l)) == 1) {
        if (!(((uint8_t *)e)[0] & 0x80)) { /* frei: Ende-Marke oder geloeschter Satz */
            if (run == 0)
                run_ord = it.ord - 1;
            locs[run++] = l;
            if (run == need)
                break;
        } else {
            run = 0;
        }
    }
    if (r < 0)
        return ERR_IO;
    if (run < need) { /* am Ende des Verzeichnisses: Cluster anhaengen */
        uint32_t last = it.last_cluster, next_ord = it.ord;
        while (run < need) {
            uint32_t c = x_dir_grow(d, last);
            if (!c)
                return ERR_NOSPC;
            last = c;
            uint32_t per = vol.spc * 16;
            for (uint32_t j = 0; j < per && run < need; j++) {
                if (run == 0)
                    run_ord = next_ord + j;
                locs[run].lba = cluster_lba(c) + j / 16;
                locs[run].off = (j % 16) * 32;
                run++;
            }
            next_ord += per;
        }
    }
    for (int i = 0; i < need; i++)
        if (fat_entry_write(&locs[i], (const DirEntry *)(set + i * 32)) != 0)
            return ERR_IO;
    if (out) {
        out->dfirst = d->first;
        out->dcontig = d->contig;
        out->dsize = d->size;
        out->ord = run_ord;
        out->nsec = need - 1;
    }
    return 0;
}

/* ---------- exFAT: Operationen ---------- */

int exfat_open(const char *path, int flags, FatFile *f)
{
    XDirRef d;
    char comp[256];
    int is_root;
    XItem it;
    int r = x_resolve_parent(path, &d, comp, &is_root);
    if (r != 0)
        return r;
    if (is_root)
        return ERR_ISDIR;
    r = x_find(d.first, d.contig, d.size, comp, &it);
    if (r < 0)
        return ERR_IO;
    if (r == 1) {
        if (it.is_dir)
            return ERR_ISDIR;
        f->first_cluster = it.first;
        f->size = it.size;
        f->valid = it.valid;
        f->contiguous = it.contig;
        f->xloc = it.loc;
        if ((flags & FAT_O_TRUNC) && f->writable) {
            uint32_t old = f->first_cluster;
            uint64_t old_size = f->size;
            int old_contig = f->contiguous;
            f->first_cluster = 0;
            f->size = f->valid = 0;
            f->contiguous = 0;
            if (exfat_update_set(&f->xloc, 0, 0, 0, 0, 1) != 0)
                return ERR_IO;
            x_free_data(old, old_size, old_contig);
        }
        return 0;
    }
    if (!(flags & FAT_O_CREAT))
        return ERR_NOENT;
    r = x_add(&d, comp, ATTR_ARCHIVE, 0, 0, 0, 0, &f->xloc);
    return r;
}

int exfat_readdir(const char *path, unsigned index, FatDirEntry *out)
{
    XItem item;
    int is_root;
    uint32_t first = vol.root_cluster;
    int contig = 0;
    uint64_t size = 0;

    int r = x_walk(path, &item, &is_root);
    if (r != 0)
        return r;
    if (!is_root) {
        if (!item.is_dir)
            return ERR_NOTDIR;
        first = item.first;
        contig = item.contig;
        size = item.size;
    }
    DirIter it;
    it_init_x(&it, first, contig, size);
    unsigned seen = 0;
    while ((r = xdir_next(&it, &item)) == 1) {
        if (seen++ != index)
            continue;
        fat_copy_str(out->name, sizeof(out->name), item.name);
        out->size = item.size;
        out->is_dir = item.is_dir;
        out->mtime = item.mtime;
        return 0;
    }
    return r < 0 ? ERR_IO : ERR_NOENT;
}

int exfat_stat(const char *path, FatDirEntry *out)
{
    XItem item;
    int is_root;
    out->name[0] = 0;
    int r = x_walk(path, &item, &is_root);
    if (r != 0)
        return r;
    out->size = is_root ? 0 : item.size;
    out->is_dir = is_root || item.is_dir;
    out->mtime = is_root ? 0 : item.mtime;
    return 0;
}

int exfat_mkdir(const char *path)
{
    XDirRef d;
    char comp[256];
    int is_root;
    XItem ex;
    int r = x_resolve_parent(path, &d, comp, &is_root);
    if (r != 0)
        return r;
    if (is_root)
        return ERR_EXIST;
    r = x_find(d.first, d.contig, d.size, comp, &ex);
    if (r < 0)
        return ERR_IO;
    if (r == 1)
        return ERR_EXIST;
    if (!fat_name_valid(comp, strlen(comp)))
        return ERR_INVAL;

    uint32_t c = fat_alloc_cluster(0);
    if (!c)
        return ERR_NOSPC;
    if (fat_zero_cluster(c) != 0) {
        fat_free_chain(c);
        return ERR_IO;
    }
    FatXLoc loc;
    r = x_add(&d, comp, ATTR_DIR, c, cluster_bytes(), cluster_bytes(), 0, &loc);
    if (r != 0)
        fat_free_chain(c);
    return r;
}

int exfat_unlink(const char *path)
{
    XDirRef d;
    char comp[256];
    int is_root;
    XItem item;
    int r = x_resolve_parent(path, &d, comp, &is_root);
    if (r != 0)
        return r;
    if (is_root)
        return ERR_INVAL;
    r = x_find(d.first, d.contig, d.size, comp, &item);
    if (r < 0)
        return ERR_IO;
    if (r == 0)
        return ERR_NOENT;
    if (item.is_dir) { /* nur leere Verzeichnisse */
        DirIter it;
        it_init_x(&it, item.first, item.contig, item.size);
        XItem child;
        int rr = xdir_next(&it, &child);
        if (rr < 0)
            return ERR_IO;
        if (rr == 1)
            return ERR_NOTEMPTY;
    }
    if (x_delete_set(&item.loc) != 0) /* erst den Eintrag, dann die Cluster: ein Abbruch dazwischen verliert nur Platz */
        return ERR_IO;
    x_free_data(item.first, item.size, item.contig);
    return 0;
}

int exfat_rename(const char *from, const char *to)
{
    XDirRef d1, d2;
    char c1[256], c2[256];
    int root1, root2;
    XItem item, other;
    int r = x_resolve_parent(from, &d1, c1, &root1);
    if (r == 0)
        r = x_resolve_parent(to, &d2, c2, &root2);
    if (r == 0 && (root1 || root2))
        r = ERR_INVAL;
    if (r != 0)
        return r;

    r = x_find(d1.first, d1.contig, d1.size, c1, &item);
    if (r < 0)
        return ERR_IO;
    if (r == 0)
        return ERR_NOENT;
    r = x_find(d2.first, d2.contig, d2.size, c2, &other);
    if (r < 0)
        return ERR_IO;
    if (r == 1)
        return (other.loc.dfirst == item.loc.dfirst && other.loc.ord == item.loc.ord) ? 0 : ERR_EXIST;
    if (item.is_dir && fat_path_inside(from, to))
        return ERR_INVAL;

    FatXLoc loc;
    r = x_add(&d2, c2, item.attr, item.first, item.size, item.valid, item.contig, &loc);
    if (r != 0)
        return r;
    return x_delete_set(&item.loc) == 0 ? 0 : ERR_IO;
}
