/* FAT/exFAT: Dateien und Verzeichnisse (oeffentliche Funktionen aus fat32.h) */

#include "lib/string.h"
#include "core/syscall.h" /* ERR_* */
#include "drivers/rtc.h"
#include "fs/fat/fat_internal.h"

/* ---------- Dateien ---------- */

/* Wie viele Sektoren ab Sektor sec des Clusters c (Index idx) am Stueck auf der Platte liegen, hoechstens want:
 * folgen weitere Cluster der Datei direkt dahinter (beim Anlegen der Normalfall), geht alles in einen Auftrag -
 * bei kleinen Clustern (512 Bytes) sonst ein Plattenzugriff je Cluster. */
static uint32_t run_sectors(FatFile *f, uint32_t c, uint32_t idx, uint32_t sec, uint64_t want, int alloc);

/* Liefert den Cluster mit Index idx in der Kette der Datei (legt ihn bei alloc an). */
static int file_cluster(FatFile *f, uint32_t idx, int alloc, uint32_t *out)
{
    uint32_t cb = cluster_bytes();
    if (f->contiguous) { /* exFAT ohne FAT-Kette */
        uint32_t n = (uint32_t)((f->size + cb - 1) / cb);
        if (idx < n) {
            if (!valid_cluster(f->first_cluster + idx))
                return -1;
            *out = f->first_cluster + idx;
            return 0;
        }
        if (!alloc)
            return -1;
        /* Die Datei waechst ueber ihre Cluster hinaus: die Kette muss jetzt in der FAT stehen */
        if (n && exfat_make_chain(f->first_cluster, n) != 0)
            return ERR_IO;
        f->contiguous = 0;
    }
    if (f->first_cluster == 0) {
        if (!alloc)
            return -1;
        uint32_t c = fat_alloc_cluster(0);
        if (!c)
            return ERR_NOSPC;
        f->first_cluster = c;
        f->cur_cluster = c;
        f->cur_index = 0;
    }
    if (f->cur_cluster == 0 || f->cur_index > idx) {
        f->cur_cluster = f->first_cluster;
        f->cur_index = 0;
    }
    while (f->cur_index < idx) {
        uint32_t next;
        if (fat_get(f->cur_cluster, &next) != 0)
            return ERR_IO;
        if (is_eoc(next) || !valid_cluster(next)) {
            if (!alloc)
                return -1;
            next = fat_alloc_cluster(f->cur_cluster);
            if (!next)
                return ERR_NOSPC;
        }
        f->cur_cluster = next;
        f->cur_index++;
    }
    *out = f->cur_cluster;
    return 0;
}

static uint32_t run_sectors(FatFile *f, uint32_t c, uint32_t idx, uint32_t sec, uint64_t want, int alloc)
{
    uint32_t sectors = vol.spc - sec, last = c;
    if (sectors > want)
        return (uint32_t)want;
    while (sectors + vol.spc <= want && sectors + vol.spc <= 128) {
        uint32_t next;
        if (file_cluster(f, ++idx, alloc, &next) != 0 || next != last + 1)
            break;
        last = next;
        sectors += vol.spc;
    }
    return sectors;
}

static int file_update_entry(const FatFile *f)
{
    if (vol.exfat)
        return exfat_update_set(&f->xloc, f->first_cluster, f->size, f->size, f->contiguous && f->first_cluster, 1) == 0
                   ? 0 : ERR_IO;
    DirLoc loc = {f->dirent_lba, f->dirent_off};
    DirEntry e;
    if (fat_entry_read(&loc, &e) != 0)
        return ERR_IO;
    e.cluster_hi = f->first_cluster >> 16;
    e.cluster_lo = f->first_cluster & 0xFFFF;
    e.size = (uint32_t)f->size;
    uint16_t stamp_date, stamp_time;
    dos_now(&stamp_date, &stamp_time);
    e.wrt_date = e.acc_date = stamp_date;
    e.wrt_time = stamp_time;
    e.attr |= ATTR_ARCHIVE;
    return fat_entry_write(&loc, &e) == 0 ? 0 : ERR_IO;
}

int fat_open(int v, const char *path, int flags, FatFile *f)
{
    mutex_lock(&fat_lock);
    int ret = fat_activate(v);
    if (ret != 0)
        goto out;

    int wants_write = (flags & 3) != FAT_O_RDONLY || (flags & (FAT_O_CREAT | FAT_O_TRUNC));
    if (wants_write && vol.readonly) {
        ret = ERR_ROFS;
        goto out;
    }
    memset(f, 0, sizeof(*f));
    f->volume = v;
    f->writable = (flags & 3) != FAT_O_RDONLY;
    f->append = (flags & FAT_O_APPEND) != 0;

    if (vol.exfat) {
        ret = exfat_open(path, flags, f);
        goto out;
    }

    uint32_t parent;
    char comp[256];
    int is_root;
    DirItem item;

    ret = fat_resolve_parent(path, &parent, comp, &is_root);
    if (ret == 0 && is_root)
        ret = ERR_ISDIR;
    if (ret != 0)
        goto out;

    int r = fat_dir_lookup(parent, comp, &item);
    if (r < 0) {
        ret = ERR_IO;
        goto out;
    }

    if (r == 1) {
        if (item.e.attr & ATTR_DIR) {
            ret = ERR_ISDIR;
            goto out;
        }
        f->first_cluster = fat_entry_cluster(&item.e);
        f->size = item.e.size;
        f->dirent_lba = item.loc.lba;
        f->dirent_off = item.loc.off;
        if ((flags & FAT_O_TRUNC) && f->writable) {
            uint32_t old = f->first_cluster;
            f->first_cluster = 0;
            f->size = 0;
            ret = file_update_entry(f);
            if (ret == 0)
                fat_free_chain(old); /* erst den Eintrag, dann die Cluster */
            goto out;
        }
    } else {
        if (!(flags & FAT_O_CREAT)) {
            ret = ERR_NOENT;
            goto out;
        }
        DirLoc loc;
        ret = fat_dir_add_name(parent, comp, ATTR_ARCHIVE, 0, 0, &loc);
        if (ret != 0)
            goto out;
        f->dirent_lba = loc.lba;
        f->dirent_off = loc.off;
    }
    ret = 0;
out:
    if (fat_cur && vol.used)
        fat_fsinfo_sync();
    mutex_unlock(&fat_lock);
    return ret;
}

int64_t fat_read(FatFile *f, void *buf, uint64_t len)
{
    mutex_lock(&fat_lock);
    uint8_t *out = buf;
    uint64_t done = 0;
    int64_t ret = 0;

    if (fat_activate(f->volume) != 0) {
        ret = ERR_NOENT;
        goto out;
    }
    if (f->pos >= f->size)
        goto out;
    if (len > f->size - f->pos)
        len = f->size - f->pos;

    uint32_t cb = cluster_bytes();
    while (done < len) {
        uint32_t c;
        if (file_cluster(f, (uint32_t)(f->pos / cb), 0, &c) != 0) {
            ret = done ? (int64_t)done : ERR_IO;
            goto out;
        }
        uint32_t off = (uint32_t)(f->pos % cb);
        uint32_t sec = off / SECTOR, boff = off % SECTOR;
        uint64_t lba = cluster_lba(c) + sec;

        uint64_t chunk;
        if (boff == 0 && len - done >= SECTOR) {
            uint32_t sectors = run_sectors(f, c, (uint32_t)(f->pos / cb), sec, (len - done) / SECTOR, 0);
            if (fat_vread(lba, sectors, out + done) != 0) {
                ret = done ? (int64_t)done : ERR_IO;
                goto out;
            }
            chunk = (uint64_t)sectors * SECTOR;
        } else {
            uint8_t tmp[SECTOR];
            if (fat_vread(lba, 1, tmp) != 0) {
                ret = done ? (int64_t)done : ERR_IO;
                goto out;
            }
            chunk = SECTOR - boff;
            if (chunk > len - done)
                chunk = len - done;
            memcpy(out + done, tmp + boff, chunk);
        }
        if (vol.exfat && f->pos + chunk > f->valid) { /* hinter ValidDataLength stehen evtl. Altdaten */
            uint64_t from = f->valid > f->pos ? f->valid - f->pos : 0;
            memset(out + done + from, 0, chunk - from);
        }
        done += chunk;
        f->pos += chunk;
    }
    ret = (int64_t)done;
out:
    mutex_unlock(&fat_lock);
    return ret;
}

int64_t fat_write(FatFile *f, const void *buf, uint64_t len)
{
    if (!f->writable)
        return ERR_BADF;
    if (len == 0)
        return 0;

    mutex_lock(&fat_lock);
    if (fat_activate(f->volume) != 0 || vol.readonly) {
        mutex_unlock(&fat_lock);
        return ERR_ROFS;
    }
    const uint8_t *in = buf;
    uint64_t done = 0;
    int64_t ret;

    if (f->append)
        f->pos = f->size;
    if (f->pos > f->size)
        f->pos = f->size;
    if (!vol.exfat && len > 0xFFFFFFFFu - f->pos)
        len = 0xFFFFFFFFu - f->pos; /* FAT: Dateigroesse ist 32 Bit */

    uint32_t cb = cluster_bytes();
    int err = 0;
    while (done < len) {
        uint32_t c;
        err = file_cluster(f, (uint32_t)(f->pos / cb), 1, &c);
        if (err != 0)
            break;
        uint32_t off = (uint32_t)(f->pos % cb);
        uint32_t sec = off / SECTOR, boff = off % SECTOR;
        uint64_t lba = cluster_lba(c) + sec;

        uint64_t chunk;
        if (boff == 0 && len - done >= SECTOR) {
            uint32_t sectors = run_sectors(f, c, (uint32_t)(f->pos / cb), sec, (len - done) / SECTOR, 1);
            if (fat_vwrite(lba, sectors, in + done) != 0) {
                err = ERR_IO;
                break;
            }
            chunk = (uint64_t)sectors * SECTOR;
        } else {
            uint8_t tmp[SECTOR];
            chunk = SECTOR - boff;
            if (chunk > len - done)
                chunk = len - done;
            /* Teilsektor: alten Inhalt erhalten (read-modify-write) */
            if ((chunk < SECTOR) && fat_vread(lba, 1, tmp) != 0) {
                err = ERR_IO;
                break;
            }
            memcpy(tmp + boff, in + done, chunk);
            if (fat_vwrite(lba, 1, tmp) != 0) {
                err = ERR_IO;
                break;
            }
        }
        done += chunk;
        f->pos += chunk;
        if (f->pos > f->size)
            f->size = f->pos;
    }

    int uerr = file_update_entry(f); /* Groesse/Startcluster nachziehen, auch nach Teilerfolg */
    ret = done ? (int64_t)done : (err ? err : uerr);
    fat_fsinfo_sync();
    mutex_unlock(&fat_lock);
    return ret;
}

int64_t fat_seek(FatFile *f, int64_t offset, int whence)
{
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->pos : whence == 2 ? (int64_t)f->size : -1;
    if (base < 0)
        return ERR_INVAL;
    int64_t np = base + offset;
    if (np < 0 || np > (int64_t)f->size) /* keine Luecken in Dateien: hinter das Ende springt man nicht */
        return ERR_INVAL;
    f->pos = (uint64_t)np;
    return np;
}

/* ---------- Verzeichnisse anlegen / loeschen / lesen ---------- */

int fat_mkdir(int v, const char *path)
{
    mutex_lock(&fat_lock);
    int ret = fat_activate(v);
    if (ret != 0)
        goto out;
    if (vol.readonly) {
        ret = ERR_ROFS;
        goto out;
    }
    if (vol.exfat) {
        ret = exfat_mkdir(path);
        goto out;
    }

    uint32_t parent;
    char comp[256];
    int is_root;
    DirItem item;

    ret = fat_resolve_parent(path, &parent, comp, &is_root);
    if (ret == 0 && is_root)
        ret = ERR_EXIST;
    if (ret != 0)
        goto out;
    int r = fat_dir_lookup(parent, comp, &item);
    if (r < 0) {
        ret = ERR_IO;
        goto out;
    }
    if (r == 1) {
        ret = ERR_EXIST;
        goto out;
    }
    if (!fat_name_valid(comp, strlen(comp))) {
        ret = ERR_INVAL;
        goto out;
    }

    uint32_t c = fat_alloc_cluster(0);
    if (!c) {
        ret = ERR_NOSPC;
        goto out;
    }
    if (fat_zero_cluster(c) != 0) {
        fat_free_chain(c);
        ret = ERR_IO;
        goto out;
    }

    /* "." und ".." als erste zwei Eintraege des neuen Verzeichnisses */
    uint8_t sec[SECTOR];
    memset(sec, 0, sizeof(sec));
    DirEntry *d = (DirEntry *)sec;
    memset(d[0].name, ' ', 11);
    d[0].name[0] = '.';
    d[0].attr = ATTR_DIR;
    d[0].cluster_hi = c >> 16;
    d[0].cluster_lo = c & 0xFFFF;
    memset(d[1].name, ' ', 11);
    d[1].name[0] = '.';
    d[1].name[1] = '.';
    d[1].attr = ATTR_DIR;
    uint32_t up = parent == vol.root_cluster ? 0 : parent; /* ".." zeigt auf die Wurzel als 0 */
    d[1].cluster_hi = up >> 16;
    d[1].cluster_lo = up & 0xFFFF;
    uint16_t stamp_date, stamp_time;
    dos_now(&stamp_date, &stamp_time);
    for (int i = 0; i < 2; i++) {
        d[i].crt_date = d[i].acc_date = d[i].wrt_date = stamp_date;
        d[i].crt_time = d[i].wrt_time = stamp_time;
    }
    if (fat_vwrite(cluster_lba(c), 1, sec) != 0) {
        fat_free_chain(c);
        ret = ERR_IO;
        goto out;
    }

    ret = fat_dir_add_name(parent, comp, ATTR_DIR, c, 0, 0);
    if (ret != 0)
        fat_free_chain(c);
out:
    if (fat_cur && vol.used)
        fat_fsinfo_sync();
    mutex_unlock(&fat_lock);
    return ret;
}

/* Loescht den Kurznamen-Eintrag samt zugehoerigen LFN-Eintraegen */
static int delete_item(const DirItem *item)
{
    for (int i = 0; i < item->lfn_count; i++)
        if (fat_entry_delete(&item->lfn_locs[i]) != 0)
            return ERR_IO;
    return fat_entry_delete(&item->loc) == 0 ? 0 : ERR_IO;
}

int fat_unlink(int v, const char *path)
{
    mutex_lock(&fat_lock);
    int ret = fat_activate(v);
    if (ret != 0)
        goto out;
    if (vol.readonly) {
        ret = ERR_ROFS;
        goto out;
    }
    if (vol.exfat) {
        ret = exfat_unlink(path);
        goto out;
    }

    uint32_t parent;
    char comp[256];
    int is_root;
    DirItem item;

    ret = fat_resolve_parent(path, &parent, comp, &is_root);
    if (ret == 0 && is_root)
        ret = ERR_INVAL;
    if (ret != 0)
        goto out;
    int r = fat_dir_lookup(parent, comp, &item);
    if (r < 0) {
        ret = ERR_IO;
        goto out;
    }
    if (r == 0) {
        ret = ERR_NOENT;
        goto out;
    }

    if (item.e.attr & ATTR_DIR) { /* nur leere Verzeichnisse */
        DirIter it;
        fat_it_init(&it, fat_entry_cluster(&item.e));
        DirItem child;
        int rr;
        while ((rr = fat_dir_next_item(&it, &child)) == 1) {
            if (child.e.name[0] != '.') {
                ret = ERR_NOTEMPTY;
                goto out;
            }
        }
        if (rr < 0) {
            ret = ERR_IO;
            goto out;
        }
    }

    ret = delete_item(&item); /* erst den Eintrag, dann die Cluster: ein Abbruch dazwischen verliert nur Platz */
    if (ret == 0)
        fat_free_chain(fat_entry_cluster(&item.e));
out:
    if (fat_cur && vol.used)
        fat_fsinfo_sync();
    mutex_unlock(&fat_lock);
    return ret;
}

int fat_readdir(int v, const char *path, unsigned index, FatDirEntry *out)
{
    mutex_lock(&fat_lock);
    int ret = fat_activate(v);
    if (ret != 0)
        goto out;
    if (vol.exfat) {
        ret = exfat_readdir(path, index, out);
        goto out;
    }

    uint32_t parent, dir;
    char comp[256];
    int is_root;
    DirItem item;

    ret = fat_resolve_parent(path, &parent, comp, &is_root);
    if (ret != 0)
        goto out;
    if (is_root) {
        dir = vol.root_cluster;
    } else {
        int r = fat_dir_lookup(parent, comp, &item);
        if (r < 0) {
            ret = ERR_IO;
            goto out;
        }
        if (r == 0) {
            ret = ERR_NOENT;
            goto out;
        }
        if (!(item.e.attr & ATTR_DIR)) {
            ret = ERR_NOTDIR;
            goto out;
        }
        dir = fat_entry_cluster(&item.e);
    }

    DirIter it;
    fat_it_init(&it, dir);
    unsigned seen = 0;
    int rr;
    ret = ERR_NOENT;
    while ((rr = fat_dir_next_item(&it, &item)) == 1) {
        if (item.e.name[0] == '.') /* "." und ".." */
            continue;
        if (seen++ != index)
            continue;
        if (item.has_long)
            fat_copy_str(out->name, sizeof(out->name), item.lname);
        else
            fat_short_display(&item.e, out->name);
        out->size = item.e.size;
        out->is_dir = (item.e.attr & ATTR_DIR) != 0;
        out->mtime = dos_to_unix(item.e.wrt_date, item.e.wrt_time);
        ret = 0;
        break;
    }
    if (rr < 0)
        ret = ERR_IO;
out:
    mutex_unlock(&fat_lock);
    return ret;
}

int fat_rename(int v, const char *from, const char *to)
{
    mutex_lock(&fat_lock);
    int ret = fat_activate(v);
    if (ret != 0)
        goto out;
    if (vol.readonly) {
        ret = ERR_ROFS;
        goto out;
    }
    if (vol.exfat) {
        ret = exfat_rename(from, to);
        goto out;
    }

    uint32_t p_from, p_to;
    char c_from[256], c_to[256];
    int root_from, root_to;
    DirItem item, other;

    ret = fat_resolve_parent(from, &p_from, c_from, &root_from);
    if (ret == 0)
        ret = fat_resolve_parent(to, &p_to, c_to, &root_to);
    if (ret == 0 && (root_from || root_to))
        ret = ERR_INVAL;
    if (ret != 0)
        goto out;

    int r = fat_dir_lookup(p_from, c_from, &item);
    if (r < 0) {
        ret = ERR_IO;
        goto out;
    }
    if (r == 0) {
        ret = ERR_NOENT;
        goto out;
    }
    r = fat_dir_lookup(p_to, c_to, &other);
    if (r < 0) {
        ret = ERR_IO;
        goto out;
    }
    if (r == 1) {
        ret = (item.loc.lba == other.loc.lba && item.loc.off == other.loc.off) ? 0 : ERR_EXIST; /* auf sich selbst: nichts zu tun */
        goto out;
    }
    if ((item.e.attr & ATTR_DIR) && fat_path_inside(from, to)) { /* ein Verzeichnis nicht in sich selbst verschieben */
        ret = ERR_INVAL;
        goto out;
    }

    /* Erst den neuen Eintrag anlegen, dann den alten (samt LFN-Eintraegen) loeschen: bei einem Absturz dazwischen ist nichts verloren */
    ret = fat_dir_add_name(p_to, c_to, item.e.attr, fat_entry_cluster(&item.e), item.e.size, 0);
    if (ret != 0)
        goto out;
    if ((item.e.attr & ATTR_DIR) && p_from != p_to) { /* ".." des verschobenen Verzeichnisses anpassen */
        uint32_t dc = fat_entry_cluster(&item.e);
        DirLoc dd = {cluster_lba(dc), 32};
        DirEntry e;
        if (valid_cluster(dc) && fat_entry_read(&dd, &e) == 0) {
            uint32_t up = p_to == vol.root_cluster ? 0 : p_to;
            e.cluster_hi = up >> 16;
            e.cluster_lo = up & 0xFFFF;
            fat_entry_write(&dd, &e);
        }
    }
    ret = delete_item(&item);
out:
    if (fat_cur && vol.used)
        fat_fsinfo_sync();
    mutex_unlock(&fat_lock);
    return ret;
}

int fat_stat(int v, const char *path, FatDirEntry *out)
{
    mutex_lock(&fat_lock);
    int ret = fat_activate(v);
    if (ret != 0)
        goto out;
    if (vol.exfat) {
        ret = exfat_stat(path, out);
        goto out;
    }

    uint32_t parent;
    char comp[256];
    int is_root;
    DirItem item;

    out->name[0] = 0;
    ret = fat_resolve_parent(path, &parent, comp, &is_root);
    if (ret == 0 && is_root) {
        out->size = 0;
        out->is_dir = 1;
        out->mtime = 0;
    } else if (ret == 0) {
        int r = fat_dir_lookup(parent, comp, &item);
        if (r < 0) {
            ret = ERR_IO;
        } else if (r == 0) {
            ret = ERR_NOENT;
        } else {
            out->size = item.e.size;
            out->is_dir = (item.e.attr & ATTR_DIR) != 0;
            out->mtime = dos_to_unix(item.e.wrt_date, item.e.wrt_time);
        }
    }
out:
    mutex_unlock(&fat_lock);
    return ret;
}
