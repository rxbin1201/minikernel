#include "fs/fs.h"
#include "drivers/block/blk.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "core/syscall.h" /* ERR_* */

static unsigned seen_generation = ~0u;

void fs_rescan(void)
{
    unsigned g = blk_generation();
    if (g != seen_generation) {
        seen_generation = g;
        fat_scan();
    }
}

void fs_init(void)
{
    fs_rescan();
}

int fs_disk_volume(void)
{
    return fat_find_data_volume();
}

/* Name unter /mnt: "usb0" bzw. "usb0p1" fuer Partition 1. Das Datenvolume MINIKERNEL steht unter /disk, nicht hier. */
static int mnt_name(int v, char out[24])
{
    FatVolumeInfo vi;
    if (fat_volume_info(v, &vi) != 0 || v == fat_find_data_volume())
        return -1;
    size_t n = 0;
    for (size_t i = 0; vi.device[i] && n < 16; i++)
        out[n++] = vi.device[i];
    if (vi.part) {
        out[n++] = 'p';
        char digits[8];
        int d = 0;
        for (int x = vi.part; x > 0 && d < 7; x /= 10)
            digits[d++] = (char)('0' + x % 10);
        while (d)
            out[n++] = digits[--d];
    }
    out[n] = 0;
    return 0;
}

/* Vergleicht den Pfadanfang (bis '/' oder Ende) mit einem Namen; liefert den Rest dahinter oder NULL */
static const char *match_component(const char *p, const char *name)
{
    size_t n = strlen(name);
    if (strncmp(p, name, n) == 0 && (p[n] == '/' || p[n] == 0))
        return p + n;
    return 0;
}

enum { R_INITRD, R_FAT, R_MNT_DIR, R_MISSING };

/* Ordnet den Pfad einem Dateisystem zu. Bei R_FAT: *volume und *rest ("" oder "/..." hinter dem Mountpunkt). */
static int resolve(const char *path, int *volume, const char **rest)
{
    const char *p = path;
    while (*p == '/')
        p++;

    const char *r;
    if ((r = match_component(p, "disk"))) {
        int v = fat_find_data_volume();
        if (v < 0)
            return R_MISSING;
        *volume = v;
        *rest = r;
        return R_FAT;
    }
    if ((r = match_component(p, "mnt"))) {
        while (*r == '/')
            r++;
        if (!*r)
            return R_MNT_DIR;
        for (int pass = 0; pass < 2; pass++) {
            for (int v = 0; v < fat_volume_count(); v++) {
                char name[24];
                if (mnt_name(v, name) != 0)
                    continue;
                const char *after = match_component(r, name);
                if (after) {
                    *volume = v;
                    *rest = after;
                    return R_FAT;
                }
            }
            if (pass == 0)
                fs_rescan(); /* vielleicht wurde der Stick gerade erst eingesteckt */
        }
        return R_MISSING;
    }
    return R_INITRD;
}

int fs_mount_info(unsigned index, MountInfo *out)
{
    memset(out, 0, sizeof(*out));
    unsigned nv = (unsigned)fat_volume_count();
    if (index < nv) {
        FatVolumeInfo vi;
        if (fat_volume_info((int)index, &vi) != 0)
            return ERR_NOENT;
        char name[24];
        if ((int)index == fat_find_data_volume()) {
            memcpy(out->point, "/disk", 6);
        } else {
            memcpy(out->point, "/mnt/", 5);
            if (mnt_name((int)index, name) == 0)
                strncpy(out->point + 5, name, sizeof(out->point) - 6);
        }
        strncpy(out->device, vi.device, sizeof(out->device) - 1);
        strncpy(out->label, vi.label, sizeof(out->label) - 1);
        if (vi.fat_bits == 0) {
            memcpy(out->fstype, "exFAT", 6);
        } else {
            out->fstype[0] = 'F';
            out->fstype[1] = 'A';
            out->fstype[2] = 'T';
            out->fstype[3] = (char)('0' + vi.fat_bits / 10);
            out->fstype[4] = (char)('0' + vi.fat_bits % 10);
        }
        out->mib = vi.sectors / 2048;
        out->flags = (vi.readonly ? 1 : 0) | (vi.dirty ? 4 : 0);
        return 0;
    }
    FatForeignInfo fi;
    if (fat_foreign_info((int)(index - nv), &fi) != 0)
        return ERR_NOENT;
    strncpy(out->device, fi.device, sizeof(out->device) - 1);
    if (fi.part) { /* "usb0p1" statt "usb0" */
        size_t n = strlen(out->device);
        if (n + 3 < sizeof(out->device)) {
            out->device[n++] = 'p';
            out->device[n++] = (char)('0' + (fi.part % 10));
            out->device[n] = 0;
        }
    }
    memcpy(out->point, "-", 2);
    strncpy(out->fstype, fi.fstype, sizeof(out->fstype) - 1);
    out->mib = fi.sectors / 2048;
    out->flags = 2 | 1;
    return 0;
}

uint32_t fs_free_clusters(const char *path)
{
    int volume;
    const char *rest;
    return resolve(path, &volume, &rest) == R_FAT ? fat_free_clusters(volume) : 0;
}

int fs_statfs(const char *path, uint64_t *total, uint64_t *free)
{
    int volume;
    const char *rest;
    int r = resolve(path, &volume, &rest);
    if (r == R_FAT) {
        uint64_t cb = fat_cluster_bytes(volume);
        *total = (uint64_t)fat_total_clusters(volume) * cb;
        *free = (uint64_t)fat_free_clusters(volume) * cb;
        return 0;
    }
    if (r == R_INITRD) {
        *total = vfs_total_size();
        *free = 0;
        return 0;
    }
    return ERR_NOENT;
}

int fs_open(const char *path, int flags, FsFile *f)
{
    int volume;
    const char *rest;
    memset(f, 0, sizeof(*f));

    switch (resolve(path, &volume, &rest)) {
    case R_FAT: {
        int r = fat_open(volume, rest, flags, &f->fat);
        if (r == 0)
            f->kind = 2;
        return r;
    }
    case R_MNT_DIR:
        return ERR_ISDIR;
    case R_MISSING:
        return ERR_NOENT;
    }

    if ((flags & 3) != FAT_O_RDONLY || (flags & (FAT_O_CREAT | FAT_O_TRUNC)))
        return ERR_ROFS;
    const VfsNode *n = vfs_lookup(path);
    if (!n)
        return ERR_NOENT;
    if (n->is_dir)
        return ERR_ISDIR;
    f->kind = 1;
    f->node = n;
    return 0;
}

int64_t fs_read(FsFile *f, void *buf, uint64_t len)
{
    if (f->kind == 2)
        return fat_read(&f->fat, buf, len);
    if (f->kind != 1)
        return ERR_BADF;
    if (f->offset >= f->node->size)
        return 0;
    uint64_t n = f->node->size - f->offset;
    if (n > len)
        n = len;
    memcpy(buf, f->node->data + f->offset, n);
    f->offset += n;
    return (int64_t)n;
}

int64_t fs_write(FsFile *f, const void *buf, uint64_t len)
{
    if (f->kind == 2)
        return fat_write(&f->fat, buf, len);
    return f->kind == 1 ? ERR_ROFS : ERR_BADF;
}

int64_t fs_seek(FsFile *f, int64_t offset, int whence)
{
    if (f->kind == 2)
        return fat_seek(&f->fat, offset, whence);
    if (f->kind != 1)
        return ERR_BADF;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->offset : whence == 2 ? (int64_t)f->node->size : -1;
    if (base < 0)
        return ERR_INVAL;
    int64_t np = base + offset;
    if (np < 0 || np > (int64_t)f->node->size)
        return ERR_INVAL;
    f->offset = (uint64_t)np;
    return np;
}

int fs_close(FsFile *f)
{
    if (f->kind == 0)
        return ERR_BADF;
    f->kind = 0; /* FAT schreibt sofort durch, es gibt nichts zu flushen */
    return 0;
}

static void fill_from_node(FsDirEnt *out, const VfsNode *n)
{
    memset(out, 0, sizeof(*out));
    const char *base = vfs_basename(n->path);
    for (size_t i = 0; base[i] && i < sizeof(out->name) - 1; i++)
        out->name[i] = base[i];
    out->size = n->size;
    out->is_dir = n->is_dir;
}

static void fill_dir(FsDirEnt *out, const char *name)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, name, sizeof(out->name) - 1);
    out->is_dir = 1;
}

int fs_readdir(const char *path, unsigned index, FsDirEnt *out)
{
    int volume;
    const char *rest;
    switch (resolve(path, &volume, &rest)) {
    case R_FAT: {
        FatDirEntry e;
        int r = fat_readdir(volume, rest, index, &e);
        if (r != 0)
            return r;
        memset(out, 0, sizeof(*out));
        for (size_t i = 0; e.name[i] && i < sizeof(out->name) - 1; i++)
            out->name[i] = e.name[i];
        out->size = e.size;
        out->is_dir = e.is_dir;
        out->mtime = e.mtime;
        return 0;
    }
    case R_MNT_DIR: {
        unsigned seen = 0;
        for (int v = 0; v < fat_volume_count(); v++) {
            char name[24];
            if (mnt_name(v, name) != 0)
                continue;
            if (seen++ == index) {
                fill_dir(out, name);
                return 0;
            }
        }
        return ERR_NOENT;
    }
    case R_MISSING:
        return ERR_NOENT;
    }

    const VfsNode *n = vfs_readdir(path, index);
    if (n) {
        fill_from_node(out, n);
        return 0;
    }

    /* Die Wurzel zeigt zusaetzlich die Mount-Verzeichnisse "disk" und "mnt" */
    const VfsNode *root = vfs_lookup(path);
    if (root && root->path[0] == '/' && root->path[1] == 0) {
        unsigned tar_children = 0;
        while (vfs_readdir("/", tar_children))
            tar_children++;
        unsigned extra = index - tar_children; /* unsigned: bei index < tar_children riesig, dann ohne Treffer */
        if (index >= tar_children && fat_find_data_volume() >= 0) {
            if (extra == 0) {
                fill_dir(out, "disk");
                return 0;
            }
            extra--;
        }
        if (index >= tar_children && extra == 0) {
            fill_dir(out, "mnt");
            return 0;
        }
    }
    return ERR_NOENT;
}

int fs_mkdir(const char *path)
{
    int volume;
    const char *rest;
    switch (resolve(path, &volume, &rest)) {
    case R_FAT:
        return fat_mkdir(volume, rest);
    case R_MISSING:
        return ERR_NOENT;
    case R_MNT_DIR:
        return ERR_ROFS;
    }
    return ERR_ROFS;
}

int fs_unlink(const char *path)
{
    int volume;
    const char *rest;
    switch (resolve(path, &volume, &rest)) {
    case R_FAT:
        return fat_unlink(volume, rest);
    case R_MISSING:
        return ERR_NOENT;
    case R_MNT_DIR:
        return ERR_ROFS;
    }
    return ERR_ROFS;
}

int fs_read_file(const char *path, void **data, uint64_t *size)
{
    FsFile f;
    int r = fs_open(path, FAT_O_RDONLY, &f);
    if (r != 0)
        return r;

    uint64_t total = f.kind == 1 ? f.node->size : f.fat.size;
    uint8_t *buf = kmalloc(total ? total : 1);
    if (!buf) {
        fs_close(&f);
        return ERR_NOMEM;
    }
    uint64_t got = 0;
    while (got < total) {
        int64_t n = fs_read(&f, buf + got, total - got);
        if (n <= 0) {
            kfree(buf);
            fs_close(&f);
            return n < 0 ? (int)n : ERR_INVAL;
        }
        got += (uint64_t)n;
    }
    fs_close(&f);
    *data = buf;
    *size = total;
    return 0;
}

int fs_rename(const char *from, const char *to)
{
    int v1, v2;
    const char *r1, *r2;
    if (resolve(from, &v1, &r1) != R_FAT || resolve(to, &v2, &r2) != R_FAT)
        return ERR_ROFS; /* die initrd ist nur lesbar */
    if (v1 != v2)
        return ERR_INVAL; /* zwischen Datentraegern wird nicht verschoben */
    return fat_rename(v1, r1, r2);
}

int fs_stat(const char *path, FsStat *out)
{
    int volume;
    const char *rest;
    switch (resolve(path, &volume, &rest)) {
    case R_FAT: {
        FatDirEntry e;
        int r = fat_stat(volume, rest, &e);
        if (r == 0) {
            out->size = e.size;
            out->is_dir = e.is_dir;
            out->mtime = e.mtime;
        }
        return r;
    }
    case R_MNT_DIR:
        out->size = 0;
        out->is_dir = 1;
        out->mtime = 0;
        return 0;
    case R_MISSING:
        return ERR_NOENT;
    }
    const VfsNode *n = vfs_lookup(path);
    if (n) {
        out->size = n->size;
        out->is_dir = n->is_dir;
        out->mtime = 0;
        return 0;
    }
    return ERR_NOENT;
}
