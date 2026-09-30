#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

/* FAT12/16/32 und exFAT auf Blockgeraeten (ganzes Geraet oder MBR/GPT-Partition, 512-Byte-Sektoren), mehrere Volumes
 * gleichzeitig, lesen und schreiben.
 *
 *  - Lange Dateinamen (LFN) werden gelesen, beim Suchen beruecksichtigt und beim Anlegen erzeugt (FAT: LFN-Eintraege
 *    mit ~N-Kurznamen, exFAT: Namenseintraege). Namen sind UTF-8 (auf der Platte UTF-16); Vergleiche ignorieren die
 *    Schreibweise auch bei Umlauten.
 *  - Schreiben ist Standard auf allen Volumes. Ausnahmen (dann nur lesbar): das Volume wurde nicht sauber getrennt
 *    (Dirty-Flag von Windows/Linux gesetzt, z.B. nach Ruhezustand oder Herausziehen ohne Auswerfen), oder
 *    fat_set_default_readonly(1) wurde aufgerufen (Kommandozeile "fsro"); das Datenvolume MINIKERNEL ist davon ausgenommen.
 *  - Zeitstempel: neue und geaenderte Eintraege bekommen die Zeit der RTC (rtc.h), Auskuenfte liefern sie als mtime.
 * Alle Funktionen sind threadsicher (ein globaler Mutex) und schreiben sofort durch. */

#define FAT_MAX_VOLUMES  8
#define FAT_MAX_FOREIGN  8

/* open-Flags (Linux-Werte) */
#define FAT_O_RDONLY 0
#define FAT_O_WRONLY 1
#define FAT_O_RDWR   2
#define FAT_O_CREAT  0x40
#define FAT_O_TRUNC  0x200
#define FAT_O_APPEND 0x400

/* Lage eines exFAT-Eintragssatzes: Verzeichnis (Cluster, ob zusammenhaengend, Groesse) und laufende Nummer des ersten Eintrags */
typedef struct {
    uint32_t dfirst;
    int      dcontig;
    uint64_t dsize;
    uint32_t ord;
    int      nsec;          /* Zahl der Folgeeintraege (Stream + Namen) */
} FatXLoc;

typedef struct {
    uint32_t first_cluster; /* 0 = leere Datei */
    uint64_t size;
    uint64_t pos;
    uint32_t cur_cluster;   /* Cache: Cluster mit Index cur_index in der Kette */
    uint32_t cur_index;
    uint64_t dirent_lba;    /* Lage des Verzeichniseintrags (fuer Groesse/Startcluster) */
    uint32_t dirent_off;
    int      writable;
    int      append;
    int      volume;        /* Index des Volumes */
    int      contiguous;    /* exFAT: Cluster liegen lueckenlos hintereinander (keine FAT-Kette) */
    uint64_t valid;         /* exFAT: gueltige Laenge, dahinter liest man Nullen */
    FatXLoc  xloc;          /* exFAT: Eintragssatz der Datei */
} FatFile;

typedef struct {
    char     name[256];     /* langer Name, sonst "NAME.EXT" */
    uint64_t size;
    int      is_dir;
    uint64_t mtime;         /* Sekunden seit 1970 (Zeit wie in der RTC), 0 = unbekannt */
} FatDirEntry;

typedef struct {
    char     device[16];    /* z.B. "usb0" */
    int      part;          /* 0 = ganzes Geraet, sonst Partitionsnummer */
    char     label[16];
    int      fat_bits;      /* 12, 16 oder 32; 0 = exFAT */
    uint64_t sectors;       /* Groesse des Volumes */
    int      readonly;
    int      is_data;       /* das eigene Datenvolume (FAT32, Label MINIKERNEL) */
    int      dirty;         /* nicht sauber getrennt: deshalb nur lesbar */
} FatVolumeInfo;

/* Volume, das keine unterstuetzte FAT-Variante ist (z.B. NTFS, ext4) */
typedef struct {
    char     device[16];
    int      part;
    char     fstype[16];
    uint64_t sectors;
} FatForeignInfo;

/* 1 = alle Volumes ausser MINIKERNEL nur lesbar einbinden (vor fat_scan aufrufen) */
void fat_set_default_readonly(int readonly);

/* Sucht auf allen Blockgeraeten (ganzes Geraet und jede MBR/GPT-Partition) nach Volumes; schon bekannte werden
 * uebersprungen. Liefert die Zahl der Volumes. Braucht blk_init(). */
int fat_scan(void);

int fat_volume_count(void);
int fat_volume_info(int volume, FatVolumeInfo *out);   /* 0 oder ERR_NOENT */
int fat_foreign_count(void);
int fat_foreign_info(int index, FatForeignInfo *out);
int fat_find_data_volume(void);                        /* Index des Volumes MINIKERNEL oder -1 */

uint32_t fat_free_clusters(int volume);   /* zaehlt die freien Cluster (liest die ganze FAT bzw. Bitmap) */
uint32_t fat_total_clusters(int volume);
uint32_t fat_cluster_bytes(int volume);

/* Rueckgabe: 0 bzw. Anzahl Bytes, oder negativer Fehlercode (ERR_* aus syscall.h) */
int     fat_open(int volume, const char *path, int flags, FatFile *f);
int64_t fat_read(FatFile *f, void *buf, uint64_t len);
int64_t fat_write(FatFile *f, const void *buf, uint64_t len);
int64_t fat_seek(FatFile *f, int64_t offset, int whence);   /* whence 0 = Anfang, 1 = aktuell, 2 = Ende; nur bis zum Dateiende */
int     fat_rename(int volume, const char *from, const char *to);   /* Datei oder Verzeichnis; Ziel darf nicht existieren */
int     fat_stat(int volume, const char *path, FatDirEntry *out);   /* name bleibt leer */
int     fat_mkdir(int volume, const char *path);
int     fat_unlink(int volume, const char *path);                   /* Datei oder leeres Verzeichnis */
int     fat_readdir(int volume, const char *path, unsigned index, FatDirEntry *out); /* 0 oder ERR_NOENT am Ende */

#endif
