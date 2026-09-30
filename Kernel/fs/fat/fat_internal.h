#ifndef FS_FAT_FAT_INTERNAL_H
#define FS_FAT_FAT_INTERNAL_H

/* Interne Schnittstelle des FAT/exFAT-Treibers (oeffentlich: fs/fat/fat32.h). Die Funktionen arbeiten auf dem aktiven
 * Volume (fat_cur, Makro vol, gewaehlt mit fat_activate) und werden mit gehaltener fat_lock aufgerufen.
 *
 *   volume.c  Volumes aktivieren, erkennen, einbinden; Auskuenfte
 *   table.c   FAT, exFAT-Belegungs-Bitmap, Cluster belegen und freigeben
 *   dir.c     Verzeichnisse lesen (Rohdaten, lange Namen), Pfade aufloesen
 *   names.c   neue Namen (Kurzname, Alias mit ~N, LFN-Eintraege)
 *   exfat.c   exFAT-Eintragssaetze und -Operationen
 *   file.c    Dateien und Verzeichnisse (oeffentliche Funktionen) */

#include "fs/fat/fat32.h"
#include "drivers/block/blk.h"
#include "core/sched.h"

#define SECTOR        512
#define CLUSTER_MASK  0x0FFFFFFFu
#define ATTR_LABEL    0x08
#define ATTR_DIR      0x10
#define ATTR_ARCHIVE  0x20
#define ATTR_LFN      0x0F
#define FAT_DATE      0x5C21 /* 2026-01-01: es gibt keine Uhr, aber 0 waere ein ungueltiges Datum */
#define DATA_LABEL    "MINIKERNEL"
#define LFN_PARTS     20     /* 20 * 13 = 260 Zeichen (FAT erlaubt 255) */
#define XSET_MAX      20     /* exFAT: Eintragssatz aus hoechstens 1 + 1 + 17 Eintraegen */

typedef struct {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntres;         /* Bit 3: Basisname klein, Bit 4: Erweiterung klein (Windows) */
    uint8_t  crt_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_lo;
    uint32_t size;
} __attribute__((packed)) DirEntry;

typedef struct {
    uint64_t lba;
    uint32_t off;
} DirLoc;

typedef struct {
    int      used;
    BlkDev  *dev;
    uint64_t base;          /* erster Sektor des Volumes auf dem Geraet (0 = ganzes Geraet, sonst Partition) */
    int      part;          /* Partitionsnummer, 0 = ganzes Geraet */
    int      fat_bits;      /* 12, 16 oder 32 (bei exFAT 32 fuer die FAT-Zugriffe) */
    int      readonly;
    int      dirty;         /* Volume wurde nicht sauber getrennt */
    int      is_data;
    int      exfat;
    uint64_t sectors;
    uint32_t spc;           /* Sektoren pro Cluster */
    uint32_t nfats;
    uint32_t fat_size;      /* Sektoren pro FAT */
    uint32_t root_cluster;  /* FAT32/exFAT: Startcluster der Wurzel; FAT12/16: 0 (feste Wurzel-Region) */
    uint32_t root_sectors;  /* FAT12/16: Groesse der festen Wurzel */
    uint64_t root_lba;      /* FAT12/16: Beginn der festen Wurzel */
    uint32_t clusters;      /* Anzahl Datencluster */
    uint64_t fat_lba;
    uint64_t data_lba;
    uint32_t eoc_min;       /* ab diesem Wert endet eine Kette */
    uint32_t free_hint;
    char     label[12];
    uint64_t fsinfo_lba;    /* 0 = kein (gueltiger) FSInfo-Sektor */
    uint32_t free_count;    /* freie Cluster (nur wenn free_known) */
    int      free_known;
    int      fsinfo_dirty;
    int      wrote;         /* seit dem letzten Flush wurde geschrieben */
    uint16_t xflags;        /* exFAT: VolumeFlags */
    uint32_t xbm_first;     /* exFAT: erster Cluster der Belegungs-Bitmap (liegt zusammenhaengend) */
    uint64_t xbm_bytes;
} FatVolume;

typedef struct {
    uint32_t cluster;   /* aktueller Cluster (0 = am Ende der Kette) */
    uint32_t last_cluster;
    uint32_t sector;    /* Sektor im Cluster bzw. in der festen Wurzel */
    uint32_t index;     /* Eintrag im Sektor (0..15) */
    int      fixed_root; /* FAT12/16: feste Wurzel-Region statt Clusterkette */
    int      contig;     /* exFAT: Verzeichnis liegt lueckenlos im Speicher */
    uint32_t remaining;  /* dann: noch nicht gelesene Cluster */
    uint32_t ord;        /* Zahl der bisher gelieferten Eintraege */
    uint32_t xfirst;     /* exFAT: das Verzeichnis selbst (fuer FatXLoc) */
    int      xcontig;
    uint64_t xsize;
    uint8_t  buf[SECTOR];
    int      loaded;
} DirIter;

typedef struct {
    DirEntry e;              /* der Kurznamen-Eintrag */
    DirLoc   loc;
    int      has_long;
    char     lname[256];     /* langer Name (UTF-8) */
    DirLoc   lfn_locs[LFN_PARTS]; /* wo die LFN-Eintraege liegen (zum Loeschen) */
    int      lfn_count;
} DirItem;

/* ---------- Zustand (volume.c) ---------- */

extern FatVolume *fat_cur;     /* Volume, mit dem gerade gearbeitet wird */
#define vol (*fat_cur)
extern Mutex      fat_lock;
extern uint64_t   fat_buf_lba; /* Sektor im FAT-Cache (table.c), ~0 = leer */
extern uint64_t   xbm_buf_sec; /* Sektor im exFAT-Bitmap-Cache (table.c), ~0 = leer */

/* ---------- Kleine Helfer ---------- */

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t le64(const uint8_t *p) { return le32(p) | ((uint64_t)le32(p + 4) << 32); }
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static inline void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static inline char upper_c(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static inline int valid_cluster(uint32_t c) { return c >= 2 && c < vol.clusters + 2; }
static inline int is_eoc(uint32_t v) { return v >= vol.eoc_min; }
static inline uint64_t cluster_lba(uint32_t c) { return vol.data_lba + (uint64_t)(c - 2) * vol.spc; }
static inline uint32_t cluster_bytes(void) { return vol.spc * SECTOR; }

/* ---------- volume.c ---------- */

int  fat_activate(int v);
int  fat_vread(uint64_t lba, uint32_t count, void *buf);
int  fat_vwrite(uint64_t lba, uint32_t count, const void *buf);
void fat_copy_str(char *dst, size_t max, const char *src);

/* ---------- table.c ---------- */

void     fat_cache_drop(void);
int      fat_get(uint32_t cluster, uint32_t *value);
int      fat_set(uint32_t cluster, uint32_t value);
int      fat_xbm_put(uint32_t cluster, int used);
uint32_t exfat_count_free(void);
uint32_t fat_alloc_cluster(uint32_t prev);
void     fat_free_chain(uint32_t c);
int      fat_zero_cluster(uint32_t c);
void     fat_fsinfo_sync(void);

/* ---------- dir.c ---------- */

void     fat_it_init(DirIter *it, uint32_t start_cluster);
int      fat_it_next(DirIter *it, DirEntry **e, DirLoc *loc);
uint32_t fat_entry_cluster(const DirEntry *e);
int      fat_entry_write(const DirLoc *loc, const DirEntry *e);
int      fat_entry_read(const DirLoc *loc, DirEntry *e);
int      fat_entry_delete(const DirLoc *loc);
uint8_t  fat_lfn_checksum(const uint8_t name11[11]);
void     fat_short_display(const DirEntry *e, char *out);
int      fat_iequal(const char *a, const char *b);
int      fat_dir_next_item(DirIter *it, DirItem *item);
int      fat_dir_lookup(uint32_t dir_cluster, const char *name, DirItem *out);
int      fat_resolve_parent(const char *path, uint32_t *parent, char comp[256], int *is_root);
int      fat_path_inside(const char *from, const char *to);

/* ---------- names.c ---------- */

int fat_name_valid(const char *s, size_t len);
int fat_dir_add_name(uint32_t dir_cluster, const char *comp, uint8_t attr, uint32_t first_cluster, uint32_t size,
                     DirLoc *loc_out);

/* ---------- exfat.c ---------- */

void exfat_scan_root(void);
int  exfat_update_set(const FatXLoc *l, uint32_t first, uint64_t size, uint64_t valid, int contig, int touch);
int  exfat_make_chain(uint32_t first, uint32_t n);
int  exfat_open(const char *path, int flags, FatFile *f);
int  exfat_readdir(const char *path, unsigned index, FatDirEntry *out);
int  exfat_stat(const char *path, FatDirEntry *out);
int  exfat_mkdir(const char *path);
int  exfat_unlink(const char *path);
int  exfat_rename(const char *from, const char *to);

#endif
