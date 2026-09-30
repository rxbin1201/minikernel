#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "fs/fat/fat32.h"
#include "core/syscall.h" /* MountInfo */
#include "fs/vfs.h"

/* Einheitliche Sicht auf die Dateisysteme:
 *   /              initrd (tar, nur lesbar)
 *   /disk          FAT32-Volume MINIKERNEL (lesen und schreiben), falls vorhanden
 *   /mnt/<geraet>  alle anderen FAT12/16/32-Volumes (nur lesbar), z.B. /mnt/usb0 oder /mnt/usb0p1 (Partition 1) */

#define FS_DISK_PREFIX "/disk"

/* open-Flags wie bei fat32.h (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND) */

typedef struct {
    int            kind;   /* 0 = frei, 1 = initrd, 2 = FAT */
    const VfsNode *node;   /* initrd */
    uint64_t       offset;
    FatFile        fat;
} FsFile;

/* Name/Groesse/Typ eines Verzeichniseintrags (gleiches Layout wie DirEnt in syscall.h) */
typedef struct {
    char     name[256];
    uint64_t size;
    uint64_t is_dir;
    uint64_t mtime;      /* Sekunden seit 1970 (Zeit wie in der RTC), 0 = unbekannt */
} FsDirEnt;

void fs_init(void);   /* nach vfs_init/fat_scan aufrufen */
void fs_rescan(void); /* sucht nach neuen Datentraegern, falls sich die Blockgeraete geaendert haben (z.B. USB-Stick) */

/* Auskunft fuer "mount": erst alle FAT-Volumes, dann nicht unterstuetzte Datentraeger. 0 oder ERR_NOENT am Ende. */
int fs_mount_info(unsigned index, MountInfo *out);
int fs_disk_volume(void); /* FAT-Volume-Index von /disk oder -1 */
uint32_t fs_free_clusters(const char *path);
/* Groesse und freier Platz des Datentraegers, auf dem der Pfad liegt (initrd: Groesse der RAM-Disk, frei 0). 0 oder Fehler */
int fs_statfs(const char *path, uint64_t *total, uint64_t *free); /* freie Cluster des Volumes, zu dem der Pfad gehoert (0 = kein FAT-Volume) */

/* 0 oder negativer Fehlercode */
int     fs_open(const char *path, int flags, FsFile *f);
int64_t fs_read(FsFile *f, void *buf, uint64_t len);
int64_t fs_write(FsFile *f, const void *buf, uint64_t len);
int64_t fs_seek(FsFile *f, int64_t offset, int whence); /* 0 = Anfang, 1 = aktuell, 2 = Ende */
int     fs_close(FsFile *f);
int     fs_readdir(const char *path, unsigned index, FsDirEnt *out);
int     fs_mkdir(const char *path);
int     fs_rename(const char *from, const char *to);       /* nur innerhalb eines beschreibbaren Volumes */

typedef struct {
    uint64_t size;
    uint64_t is_dir;
    uint64_t mtime;
} FsStat;
int     fs_stat(const char *path, FsStat *out);
int     fs_unlink(const char *path);

/* Liest eine ganze Datei in einen kmalloc-Puffer (Aufrufer gibt ihn mit kfree frei). 0 = ok. */
int fs_read_file(const char *path, void **data, uint64_t *size);

#endif
