#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>

/* Read-only Dateisystem aus einem tar-Archiv (ustar), das der Bootloader als Modul in den Speicher geladen hat.
 * Pfade sind absolut ("/bin/sh"); "." und doppelte/abschliessende Schraegstriche werden ignoriert. */

#define VFS_PATH_MAX 1024 /* laengster Pfad im System (mit der abschliessenden 0), fuer alle Dateisysteme */
#define VFS_NODE_PATH 256 /* Pfade in der initrd: tar (ustar) kann ohnehin nur 255 Zeichen */

typedef struct {
    char           path[VFS_NODE_PATH]; /* normalisiert, z.B. "/bin/sh"; Verzeichnisse ohne abschliessenden '/' */
    int            is_dir;
    const uint8_t *data;               /* Zeigt direkt ins tar (kein Kopieren) */
    uint64_t       size;
} VfsNode;

/* Liest das Archiv ein. Liefert die Anzahl der Eintraege oder -1. */
int vfs_init(const void *tar, uint64_t size);

/* Pfad zu "/a/b" vereinfachen ("." und ".." aufloesen; relative Pfade werden als ab der Wurzel gelesen).
 * 0 = ok, -1 = passt nicht in max Zeichen (out ist dann unbrauchbar - nie stillschweigend gekuerzt). */
int vfs_normalize(const char *in, char *out, size_t max);

/* Anzahl der Eintraege (ohne die Wurzel). */
int vfs_count(void);

/* Summe der Dateigroessen (fuer df) */
uint64_t vfs_total_size(void);

/* NULL, wenn es den Pfad nicht gibt. "/" liefert einen Verzeichnis-Knoten. */
const VfsNode *vfs_lookup(const char *path);

/* index-ter direkter Kindeintrag von `dir` oder NULL am Ende. */
const VfsNode *vfs_readdir(const char *dir, unsigned index);

/* Letzte Pfadkomponente ("/bin/sh" -> "sh"). */
const char *vfs_basename(const char *path);

#endif
