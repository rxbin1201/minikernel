/* FAT: Verzeichnisse lesen (Rohdaten, lange Namen) und Pfade aufloesen */

#include "lib/string.h"
#include "core/syscall.h" /* ERR_* */
#include "lib/utf8.h"
#include "fs/fat/fat_internal.h"

/* ---------- Verzeichnisse: Iterator ueber die Rohdaten ---------- */

/* start_cluster 0 = Wurzel */
void fat_it_init(DirIter *it, uint32_t start_cluster)
{
    it->fixed_root = start_cluster == 0 && vol.fat_bits != 32;
    it->cluster = it->last_cluster = it->fixed_root ? 1 : (start_cluster ? start_cluster : vol.root_cluster);
    it->sector = it->index = 0;
    it->loaded = 0;
    it->contig = 0;
    it->remaining = 0;
    it->ord = 0;
}

/* Naechster Eintrag (auch freie/geloeschte). 1 = Eintrag, 0 = Ende, -1 = Fehler. */
int fat_it_next(DirIter *it, DirEntry **e, DirLoc *loc)
{
    if (it->cluster == 0)
        return 0;

    uint64_t lba;
    if (it->fixed_root) {
        if (it->sector >= vol.root_sectors)
            return 0;
        lba = vol.root_lba + it->sector;
    } else {
        lba = cluster_lba(it->cluster) + it->sector;
    }
    if (!it->loaded) {
        if (fat_vread(lba, 1, it->buf) != 0)
            return -1;
        it->loaded = 1;
    }
    *e = (DirEntry *)it->buf + it->index;
    loc->lba = lba;
    loc->off = it->index * 32;
    it->ord++;

    if (++it->index == SECTOR / 32) {
        it->index = 0;
        it->loaded = 0;
        it->sector++;
        if (it->fixed_root) {
            if (it->sector >= vol.root_sectors)
                it->cluster = 0;
        } else if (it->sector == vol.spc) {
            it->sector = 0;
            it->last_cluster = it->cluster;
            if (it->contig) {
                it->cluster = (--it->remaining > 0 && valid_cluster(it->cluster + 1)) ? it->cluster + 1 : 0;
                return 1;
            }
            uint32_t next;
            if (fat_get(it->cluster, &next) != 0)
                return -1;
            it->cluster = (is_eoc(next) || !valid_cluster(next)) ? 0 : next;
        }
    }
    return 1;
}

uint32_t fat_entry_cluster(const DirEntry *e)
{
    return ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
}

/* Liest-aendert-schreibt einen Verzeichniseintrag (32 Bytes) */
int fat_entry_write(const DirLoc *loc, const DirEntry *e)
{
    uint8_t sec[SECTOR];
    if (fat_vread(loc->lba, 1, sec) != 0)
        return -1;
    memcpy(sec + loc->off, e, 32);
    return fat_vwrite(loc->lba, 1, sec);
}

int fat_entry_read(const DirLoc *loc, DirEntry *e)
{
    uint8_t sec[SECTOR];
    if (fat_vread(loc->lba, 1, sec) != 0)
        return -1;
    memcpy(e, sec + loc->off, 32);
    return 0;
}

/* Markiert einen Eintrag als geloescht */
int fat_entry_delete(const DirLoc *loc)
{
    DirEntry e;
    if (fat_entry_read(loc, &e) != 0)
        return -1;
    e.name[0] = 0xE5;
    return fat_entry_write(loc, &e);
}

/* ---------- Verzeichnisse: Eintraege mit langen Namen ---------- */

uint8_t fat_lfn_checksum(const uint8_t name11[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
    return sum;
}

/* "NAME.EXT" aus dem Kurznamen (mit den Klein-Flags von Windows) */
void fat_short_display(const DirEntry *e, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) {
        char c = (char)e->name[i];
        if ((e->ntres & 0x08) && c >= 'A' && c <= 'Z')
            c += 32;
        out[n++] = c;
    }
    if (e->name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e->name[i] != ' '; i++) {
            char c = (char)e->name[i];
            if ((e->ntres & 0x10) && c >= 'A' && c <= 'Z')
                c += 32;
            out[n++] = c;
        }
    }
    out[n] = 0;
}

/* Namensvergleich ohne Beachtung der Schreibweise (ASCII, Latin-1, Latin Erweitert-A, Griechisch, Kyrillisch) */
int fat_iequal(const char *a, const char *b)
{
    for (;;) {
        uint32_t x = utf8_next(&a), y = utf8_next(&b);
        if (uni_upper(x) != uni_upper(y))
            return 0;
        if (!x)
            return 1;
    }
}

/* Naechster Datei-/Verzeichniseintrag mit zusammengesetztem langen Namen. Ueberspringt geloeschte Eintraege und das
 * Volume-Label. 1 = Eintrag, 0 = Ende, -1 = Fehler. */
int fat_dir_next_item(DirIter *it, DirItem *item)
{
    uint16_t u16[LFN_PARTS * 13];
    int valid = 0, expected = 0;
    uint8_t checksum = 0;
    item->lfn_count = 0;
    item->has_long = 0;

    for (;;) {
        DirEntry *e;
        DirLoc l;
        int r = fat_it_next(it, &e, &l);
        if (r <= 0)
            return r;
        if (e->name[0] == 0x00)
            return 0; /* hinter dem ersten leeren Eintrag folgt nichts mehr */
        if (e->name[0] == 0xE5) {
            valid = 0;
            item->lfn_count = 0;
            continue;
        }

        if (e->attr == ATTR_LFN) {
            const uint8_t *b = (const uint8_t *)e;
            int seq = b[0] & 0x1F;
            if (b[0] & 0x40) { /* erster physischer Eintrag = hoechster Teil */
                valid = seq >= 1 && seq <= LFN_PARTS;
                expected = seq;
                checksum = b[13];
                item->lfn_count = 0;
                for (int i = 0; i < LFN_PARTS * 13; i++)
                    u16[i] = 0xFFFF;
            } else if (!valid || seq != expected - 1 || b[13] != checksum) {
                valid = 0;
                continue;
            } else {
                expected = seq;
            }
            if (valid) {
                static const uint8_t offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int k = 0; k < 13; k++)
                    u16[(seq - 1) * 13 + k] = le16(b + offs[k]);
                if (item->lfn_count < LFN_PARTS)
                    item->lfn_locs[item->lfn_count++] = l;
            }
            continue;
        }

        if (e->attr & ATTR_LABEL) { /* Volume-Label ist kein Eintrag */
            valid = 0;
            item->lfn_count = 0;
            continue;
        }

        item->e = *e;
        item->loc = l;
        if (valid && expected == 1 && checksum == fat_lfn_checksum(e->name)) {
            int n = utf16_to_utf8(u16, LFN_PARTS * 13, item->lname, sizeof(item->lname));
            item->has_long = n > 0;
        }
        if (!item->has_long)
            item->lfn_count = 0; /* die LFN-Eintraege gehoerten nicht zu diesem Eintrag */
        return 1;
    }
}

/* Sucht einen Namen (kurz oder lang, Gross-/Kleinschreibung egal) in einem Verzeichnis. 1 = gefunden, 0 = nicht, -1 = Fehler */
int fat_dir_lookup(uint32_t dir_cluster, const char *name, DirItem *out)
{
    DirIter it;
    fat_it_init(&it, dir_cluster);
    int r;
    while ((r = fat_dir_next_item(&it, out)) == 1) {
        char sname[16];
        fat_short_display(&out->e, sname);
        if ((out->has_long && fat_iequal(out->lname, name)) || fat_iequal(sname, name))
            return 1;
    }
    return r < 0 ? -1 : 0;
}

/* ---------- Pfade (FAT) ---------- */

/* Zerlegt den Pfad: liefert das Verzeichnis (Cluster) der letzten Komponente und deren Text.
 * *is_root = 1, wenn der Pfad die Wurzel selbst ist. Fehler: negativer Code. */
int fat_resolve_parent(const char *path, uint32_t *parent, char comp[256], int *is_root)
{
    uint32_t dir = vol.root_cluster;
    *is_root = 0;

    const char *p = path;
    while (*p == '/')
        p++;
    if (!*p) {
        *is_root = 1;
        *parent = dir;
        return 0;
    }

    for (;;) {
        const char *end = p;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - p);
        if (len == 0 || len > 255)
            return ERR_INVAL;

        const char *next = end;
        while (*next == '/')
            next++;

        char name[256];
        memcpy(name, p, len);
        name[len] = 0;

        if (!*next) { /* letzte Komponente */
            memcpy(comp, name, len + 1);
            *parent = dir;
            return 0;
        }

        DirItem item;
        int r = fat_dir_lookup(dir, name, &item);
        if (r < 0)
            return ERR_IO;
        if (r == 0)
            return ERR_NOENT;
        if (!(item.e.attr & ATTR_DIR))
            return ERR_NOTDIR;
        dir = fat_entry_cluster(&item.e);
        if (dir == 0)
            dir = vol.root_cluster; /* ".." zeigt bei der Wurzel auf Cluster 0 */
        p = next;
    }
}

/* Liegt `to` unterhalb von `from` (oder ist gleich)? Dann darf ein Verzeichnis nicht dorthin verschoben werden. */
int fat_path_inside(const char *from, const char *to)
{
    while (*from == '/')
        from++;
    while (*to == '/')
        to++;
    size_t n = strlen(from);
    while (n && from[n - 1] == '/')
        n--;
    for (size_t i = 0; i < n; i++)
        if (upper_c(from[i]) != upper_c(to[i]))
            return 0;
    return to[n] == '/' || to[n] == 0;
}
