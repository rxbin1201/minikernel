#include "fs/fat32.h"
#include "drivers/block/blk.h"
#include "drivers/block/part.h"
#include "lib/kprintf.h"
#include "core/sched.h"
#include "lib/string.h"
#include "core/syscall.h" /* ERR_* */
#include "drivers/rtc.h"
#include "lib/utf8.h"

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

static FatVolume  volumes[FAT_MAX_VOLUMES];
static int        volume_count;
static FatVolume *V;                      /* Volume, mit dem gerade gearbeitet wird */
#define vol (*V)

static FatForeignInfo foreign[FAT_MAX_FOREIGN];
static BlkDev        *foreign_dev[FAT_MAX_FOREIGN];
static uint64_t       foreign_base[FAT_MAX_FOREIGN];
static int            foreign_count;

static int   default_readonly;
static Mutex lock = MUTEX_INIT;

/* Ein-Sektor-Caches fuer die FAT (Write-Back: Aenderungen werden gesammelt und spaetestens am Ende jedes
 * Vorgangs in alle FAT-Kopien geschrieben) und die exFAT-Bitmap (Write-Through) des aktiven Volumes */
static uint8_t  fat_buf[SECTOR];
static uint64_t fat_buf_lba = ~0ULL;
static int      fat_buf_dirty;
static uint8_t  xbm_buf[SECTOR];
static uint64_t xbm_buf_sec = ~0ULL;

void fat_set_default_readonly(int readonly)
{
    default_readonly = readonly != 0;
}

/* Macht ein Volume zum aktuellen. Aufruf mit gehaltener Sperre. */
static void fat_cache_drop(void);

static int activate(int v)
{
    if (v < 0 || v >= volume_count || !volumes[v].used)
        return ERR_NOENT;
    if (V != &volumes[v]) {
        if (V)
            fat_cache_drop(); /* gesammelte FAT-Aenderungen gehoeren zum bisherigen Volume */
        V = &volumes[v];
        fat_buf_lba = ~0ULL;
        xbm_buf_sec = ~0ULL;
    }
    return 0;
}

/* Alle Zugriffe des Dateisystems sind relativ zum Volume-Anfang */
static int vread(uint64_t lba, uint32_t count, void *buf)
{
    return blk_read(vol.dev, vol.base + lba, count, buf);
}

static int vwrite(uint64_t lba, uint32_t count, const void *buf)
{
    if (vol.readonly)
        return -1; /* letzte Sicherung: auf schreibgeschuetzte Volumes geht nie ein Schreibzugriff */
    vol.wrote = 1;
    return blk_write(vol.dev, vol.base + lba, count, buf);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return le32(p) | ((uint64_t)le32(p + 4) << 32); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static inline int valid_cluster(uint32_t c) { return c >= 2 && c < vol.clusters + 2; }
static inline int is_eoc(uint32_t v) { return v >= vol.eoc_min; }
static inline uint64_t cluster_lba(uint32_t c) { return vol.data_lba + (uint64_t)(c - 2) * vol.spc; }
static inline uint32_t cluster_bytes(void) { return vol.spc * SECTOR; }

/* ---------- FAT ---------- */

static int fat_store(uint64_t lba, const uint8_t *sec);

/* Geaenderten FAT-Sektor in alle FAT-Kopien schreiben */
static int fat_flush(void)
{
    if (!fat_buf_dirty)
        return 0;
    if (fat_store(fat_buf_lba, fat_buf) != 0)
        return -1;
    fat_buf_dirty = 0;
    return 0;
}

static void fat_cache_drop(void)
{
    fat_flush();
    fat_buf_lba = ~0ULL;
}

static int fat_load(uint64_t lba)
{
    if (fat_buf_lba == lba)
        return 0;
    if (fat_flush() != 0)
        return -1;
    if (vread(lba, 1, fat_buf) != 0) {
        fat_buf_lba = ~0ULL;
        return -1;
    }
    fat_buf_lba = lba;
    return 0;
}

static int fat_get(uint32_t cluster, uint32_t *value)
{
    if (vol.fat_bits == 32) {
        if (fat_load(vol.fat_lba + cluster / (SECTOR / 4)) != 0)
            return -1;
        *value = le32(fat_buf + (cluster % (SECTOR / 4)) * 4) & CLUSTER_MASK;
        return 0;
    }
    if (vol.fat_bits == 16) {
        uint32_t off = cluster * 2;
        if (fat_load(vol.fat_lba + off / SECTOR) != 0)
            return -1;
        *value = le16(fat_buf + off % SECTOR);
        return 0;
    }
    /* FAT12: 12-Bit-Eintraege, zwei Eintraege in drei Bytes; ein Eintrag kann ueber eine Sektorgrenze gehen */
    uint32_t off = cluster + cluster / 2;
    if (fat_load(vol.fat_lba + off / SECTOR) != 0)
        return -1;
    uint32_t b0 = fat_buf[off % SECTOR];
    off++;
    if (fat_load(vol.fat_lba + off / SECTOR) != 0)
        return -1;
    uint32_t v = b0 | ((uint32_t)fat_buf[off % SECTOR] << 8);
    *value = (cluster & 1) ? v >> 4 : v & 0xFFF;
    return 0;
}

/* Schreibt einen FAT-Sektor in alle FAT-Kopien */
static int fat_store(uint64_t lba, const uint8_t *sec)
{
    for (uint32_t i = 0; i < vol.nfats; i++)
        if (vwrite(lba + (uint64_t)i * vol.fat_size, 1, sec) != 0)
            return -1;
    return 0;
}

static uint32_t eoc_mark(void)
{
    return vol.exfat ? 0xFFFFFFFFu : vol.fat_bits == 32 ? CLUSTER_MASK : vol.fat_bits == 16 ? 0xFFFFu : 0xFFFu;
}

static int fat_set(uint32_t cluster, uint32_t value)
{
    if (vol.readonly)
        return -1;
    if (vol.fat_bits == 32) { /* im Cache aendern, geschrieben wird gesammelt (fat_flush) */
        if (fat_load(vol.fat_lba + cluster / (SECTOR / 4)) != 0)
            return -1;
        uint8_t *e = fat_buf + (cluster % (SECTOR / 4)) * 4;
        if (vol.exfat)
            put32(e, value);
        else
            put32(e, (le32(e) & ~CLUSTER_MASK) | (value & CLUSTER_MASK)); /* obere 4 Bit bleiben erhalten */
        fat_buf_dirty = 1;
        return 0;
    }
    if (vol.fat_bits == 16) {
        uint32_t off = cluster * 2;
        if (fat_load(vol.fat_lba + off / SECTOR) != 0)
            return -1;
        put16(fat_buf + off % SECTOR, (uint16_t)value);
        fat_buf_dirty = 1;
        return 0;
    }
    fat_cache_drop(); /* FAT12 (Eintraege koennen zwei Sektoren beruehren): direkt schreiben */
    uint32_t off = cluster + cluster / 2;
    uint64_t lba = vol.fat_lba + off / SECTOR;
    uint32_t o = off % SECTOR;
    uint8_t s0[SECTOR], s1[SECTOR];
    int span = o == SECTOR - 1;
    if (vread(lba, 1, s0) != 0 || (span && vread(lba + 1, 1, s1) != 0))
        return -1;
    uint8_t *p0 = &s0[o], *p1 = span ? &s1[0] : &s0[o + 1];
    if (cluster & 1) {
        *p0 = (uint8_t)((*p0 & 0x0F) | ((value & 0x0F) << 4));
        *p1 = (uint8_t)(value >> 4);
    } else {
        *p0 = (uint8_t)value;
        *p1 = (uint8_t)((*p1 & 0xF0) | ((value >> 8) & 0x0F));
    }
    if (fat_store(lba, s0) != 0 || (span && fat_store(lba + 1, s1) != 0))
        return -1;
    return 0;
}

/* ---------- exFAT: Belegungs-Bitmap ---------- */

static int xbm_load(uint64_t sec)
{
    if (xbm_buf_sec == sec)
        return 0;
    if (vread(cluster_lba(vol.xbm_first) + sec, 1, xbm_buf) != 0)
        return -1;
    xbm_buf_sec = sec;
    return 0;
}

static int xbm_put(uint32_t cluster, int used)
{
    uint64_t bit = cluster - 2, byte = bit / 8, sec = byte / SECTOR;
    if (byte >= vol.xbm_bytes || xbm_load(sec) != 0)
        return -1;
    if (used)
        xbm_buf[byte % SECTOR] |= (uint8_t)(1u << (bit % 8));
    else
        xbm_buf[byte % SECTOR] &= (uint8_t)~(1u << (bit % 8));
    return vwrite(cluster_lba(vol.xbm_first) + sec, 1, xbm_buf);
}

/* Sucht ein freies Bit ab dem Suchhinweis, belegt es und liefert den Cluster (0 = voll) */
static uint32_t xbm_alloc(void)
{
    uint32_t total = vol.clusters;
    uint32_t start = vol.free_hint >= 2 && vol.free_hint < total + 2 ? vol.free_hint - 2 : 0;
    for (uint32_t k = 0; k < total;) {
        uint32_t i = start + k;
        if (i >= total)
            i -= total;
        uint64_t byte = i / 8;
        if (byte >= vol.xbm_bytes || xbm_load(byte / SECTOR) != 0)
            return 0;
        uint8_t b = xbm_buf[byte % SECTOR];
        if (b & (1u << (i % 8))) {
            k += (b == 0xFF && i % 8 == 0) ? 8 : 1;
            continue;
        }
        if (xbm_put(i + 2, 1) != 0)
            return 0;
        vol.free_hint = i + 3;
        return i + 2;
    }
    return 0;
}

static uint32_t x_count_free(void)
{
    uint32_t free = 0;
    for (uint32_t i = 0; i < vol.clusters; i++) {
        uint64_t byte = i / 8;
        if (byte >= vol.xbm_bytes || xbm_load(byte / SECTOR) != 0)
            break;
        if (!(xbm_buf[byte % SECTOR] & (1u << (i % 8))))
            free++;
    }
    return free;
}

/* ---------- Cluster belegen und freigeben ---------- */

/* Belegt einen freien Cluster (Ende der Kette) und haengt ihn hinter `prev` (0 = kein Vorgaenger). 0 = voll/Fehler. */
static uint32_t alloc_cluster(uint32_t prev)
{
    if (vol.readonly)
        return 0;
    if (vol.exfat) {
        uint32_t c = xbm_alloc();
        if (!c)
            return 0;
        if (fat_set(c, 0xFFFFFFFFu) != 0 || (prev && fat_set(prev, c) != 0)) {
            xbm_put(c, 0);
            return 0;
        }
        return c;
    }
    uint32_t total = vol.clusters + 2;
    for (uint32_t n = 0; n < vol.clusters; n++) {
        uint32_t c = vol.free_hint + n;
        if (c >= total)
            c = 2 + (c - total);
        uint32_t v;
        if (fat_get(c, &v) != 0)
            return 0;
        if (v != 0)
            continue;
        if (fat_set(c, eoc_mark()) != 0)
            return 0;
        if (prev && fat_set(prev, c) != 0)
            return 0;
        vol.free_hint = c + 1;
        if (vol.free_known) {
            vol.free_count--;
            vol.fsinfo_dirty = 1;
        }
        return c;
    }
    return 0;
}

/* Gibt eine Kette frei (bei exFAT auch in der Bitmap) */
static void free_chain(uint32_t c)
{
    uint32_t guard = vol.clusters + 2;
    while (valid_cluster(c) && guard--) {
        uint32_t next;
        if (fat_get(c, &next) != 0 || fat_set(c, 0) != 0)
            return;
        if (vol.exfat) {
            xbm_put(c, 0);
        } else if (vol.free_known) {
            vol.free_count++;
            vol.fsinfo_dirty = 1;
        }
        if (is_eoc(next) || next == 0)
            break;
        c = next;
    }
    vol.free_hint = 2;
}

static int zero_cluster(uint32_t c)
{
    uint8_t zeros[SECTOR];
    memset(zeros, 0, sizeof(zeros));
    for (uint32_t s = 0; s < vol.spc; s++)
        if (vwrite(cluster_lba(c) + s, 1, zeros) != 0)
            return -1;
    return 0;
}

/* Schreibt Freizaehler und Suchhinweis in den FSInfo-Sektor (rein informativ, aber fsck.fat vergleicht ihn). */
static void fsinfo_sync(void)
{
    fat_flush();
    if (vol.readonly)
        return;
    if (vol.fsinfo_dirty && vol.fsinfo_lba && vol.free_known) {
        uint8_t sec[SECTOR];
        if (vread(vol.fsinfo_lba, 1, sec) == 0) {
            memcpy(sec + 488, &vol.free_count, 4);
            memcpy(sec + 492, &vol.free_hint, 4);
            if (vwrite(vol.fsinfo_lba, 1, sec) == 0)
                vol.fsinfo_dirty = 0;
        }
    }
    if (vol.wrote) { /* Daten sollen wirklich auf dem Medium sein, nicht nur im Cache der Platte */
        blk_flush(vol.dev);
        vol.wrote = 0;
    }
}

/* ---------- Verzeichnisse: Iterator ueber die Rohdaten ---------- */

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

/* start_cluster 0 = Wurzel */
static void it_init(DirIter *it, uint32_t start_cluster)
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
static int it_next(DirIter *it, DirEntry **e, DirLoc *loc)
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
        if (vread(lba, 1, it->buf) != 0)
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

static uint32_t entry_cluster(const DirEntry *e)
{
    return ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
}

/* Liest-aendert-schreibt einen Verzeichniseintrag (32 Bytes) */
static int entry_write(const DirLoc *loc, const DirEntry *e)
{
    uint8_t sec[SECTOR];
    if (vread(loc->lba, 1, sec) != 0)
        return -1;
    memcpy(sec + loc->off, e, 32);
    return vwrite(loc->lba, 1, sec);
}

static int entry_read(const DirLoc *loc, DirEntry *e)
{
    uint8_t sec[SECTOR];
    if (vread(loc->lba, 1, sec) != 0)
        return -1;
    memcpy(e, sec + loc->off, 32);
    return 0;
}

/* Markiert einen Eintrag als geloescht */
static int entry_delete(const DirLoc *loc)
{
    DirEntry e;
    if (entry_read(loc, &e) != 0)
        return -1;
    e.name[0] = 0xE5;
    return entry_write(loc, &e);
}

/* ---------- Verzeichnisse: Eintraege mit langen Namen ---------- */

typedef struct {
    DirEntry e;              /* der Kurznamen-Eintrag */
    DirLoc   loc;
    int      has_long;
    char     lname[256];     /* langer Name (UTF-8) */
    DirLoc   lfn_locs[LFN_PARTS]; /* wo die LFN-Eintraege liegen (zum Loeschen) */
    int      lfn_count;
} DirItem;

static uint8_t lfn_checksum(const uint8_t name11[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
    return sum;
}

/* "NAME.EXT" aus dem Kurznamen (mit den Klein-Flags von Windows) */
static void short_display(const DirEntry *e, char *out)
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
static int iequal(const char *a, const char *b)
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
static int dir_next_item(DirIter *it, DirItem *item)
{
    uint16_t u16[LFN_PARTS * 13];
    int valid = 0, expected = 0;
    uint8_t checksum = 0;
    item->lfn_count = 0;
    item->has_long = 0;

    for (;;) {
        DirEntry *e;
        DirLoc l;
        int r = it_next(it, &e, &l);
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
        if (valid && expected == 1 && checksum == lfn_checksum(e->name)) {
            int n = utf16_to_utf8(u16, LFN_PARTS * 13, item->lname, sizeof(item->lname));
            item->has_long = n > 0;
        }
        if (!item->has_long)
            item->lfn_count = 0; /* die LFN-Eintraege gehoerten nicht zu diesem Eintrag */
        return 1;
    }
}

/* Sucht einen Namen (kurz oder lang, Gross-/Kleinschreibung egal) in einem Verzeichnis. 1 = gefunden, 0 = nicht, -1 = Fehler */
static int dir_lookup(uint32_t dir_cluster, const char *name, DirItem *out)
{
    DirIter it;
    it_init(&it, dir_cluster);
    int r;
    while ((r = dir_next_item(&it, out)) == 1) {
        char sname[16];
        short_display(&out->e, sname);
        if ((out->has_long && iequal(out->lname, name)) || iequal(sname, name))
            return 1;
    }
    return r < 0 ? -1 : 0;
}

/* ---------- Neue Namen: Kurzname, Alias mit ~N, LFN-Eintraege ---------- */

/* Zeichen, die in einem langen Namen (FAT und exFAT) nicht vorkommen duerfen; nur ASCII wird angelegt */
static int name_valid(const char *s, size_t len)
{
    if (len == 0 || len > 255)
        return 0;
    if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
        return 0;
    if (s[len - 1] == ' ' || s[len - 1] == '.' || s[0] == ' ')
        return 0;
    if (!utf8_valid(s))
        return 0;
    const char *p = s;
    for (uint32_t cp; (cp = utf8_next(&p)) != 0;) {
        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || (cp < 0x80 && strchr("\\/:*?\"<>|", (int)cp)))
            return 0;
    }
    uint16_t units[256];
    return utf8_to_utf16(s, units, 255) > 0; /* hoechstens 255 UTF-16-Einheiten */
}

static int short_char_ok(char c) /* c ist GROSS */
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '$' || c == '~' ||
           c == '!' || c == '#' || c == '%' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '@' ||
           c == '^' || c == '{' || c == '}';
}

static char upper_c(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

/* Passt der Name in 8.3 (GROSS oder ganz klein pro Teil)? 0 = ja (out = Kurzname, ntres = Klein-Flags), -1 = braucht LFN */
static int short_fits(const char *s, size_t len, uint8_t out[11], uint8_t *ntres)
{
    memset(out, ' ', 11);
    *ntres = 0;
    for (size_t i = 0; i < len; i++)
        if ((uint8_t)s[i] >= 0x80)
            return -1; /* Nicht-ASCII braucht einen langen Namen */
    size_t dot = len;
    for (size_t i = len; i > 0; i--) {
        if (s[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }
    size_t name_len = dot, ext_len = dot < len ? len - dot - 1 : 0;
    if (name_len == 0 || name_len > 8 || ext_len > 3)
        return -1;

    int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;
    for (size_t i = 0; i < len; i++) {
        if (i == dot)
            continue;
        char c = s[i];
        int in_ext = i > dot;
        if (c >= 'a' && c <= 'z') {
            if (in_ext) ext_lower = 1; else base_lower = 1;
        } else if (c >= 'A' && c <= 'Z') {
            if (in_ext) ext_upper = 1; else base_upper = 1;
        }
        c = upper_c(c);
        if (!short_char_ok(c))
            return -1;
        if (in_ext)
            out[8 + (i - dot - 1)] = (uint8_t)c;
        else
            out[i] = (uint8_t)c;
    }
    if ((base_lower && base_upper) || (ext_lower && ext_upper))
        return -1;
    *ntres = (uint8_t)((base_lower ? 0x08 : 0) | (ext_lower ? 0x10 : 0));
    return 0;
}

/* Gibt es im Verzeichnis schon einen Eintrag mit diesem Kurznamen? 1 = ja, 0 = nein, -1 = Fehler */
static int alias_taken(uint32_t dir, const uint8_t n11[11])
{
    DirIter it;
    it_init(&it, dir);
    DirItem item;
    int r;
    while ((r = dir_next_item(&it, &item)) == 1)
        if (memcmp(item.e.name, n11, 11) == 0)
            return 1;
    return r < 0 ? -1 : 0;
}

/* Erzeugt einen eindeutigen Kurznamen fuer einen langen Namen (wie Windows: ERSTE6~1.TXT) */
static int make_alias(uint32_t dir, const char *s, size_t len, uint8_t out[11])
{
    size_t dot = len;
    for (size_t i = len; i > 0; i--) {
        if (s[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }
    char base[9], ext[4];
    int bn = 0, en = 0, lossy = 0;
    for (size_t i = 0; i < dot; i++) {
        char c = s[i];
        if (c == ' ' || c == '.') {
            lossy = 1;
            continue;
        }
        if ((uint8_t)c >= 0x80) { /* je Zeichen ein '_' (Folgebytes ueberspringen) */
            lossy = 1;
            if (((uint8_t)c & 0xC0) == 0x80)
                continue;
            c = '_';
        }
        c = upper_c(c);
        if (!short_char_ok(c)) {
            c = '_';
            lossy = 1;
        }
        if (bn < 8)
            base[bn++] = c;
        else
            lossy = 1;
    }
    for (size_t i = dot + 1; i < len && dot < len; i++) {
        char c = s[i];
        if (c == ' ') {
            lossy = 1;
            continue;
        }
        if ((uint8_t)c >= 0x80) {
            lossy = 1;
            if (((uint8_t)c & 0xC0) == 0x80)
                continue;
            c = '_';
        }
        c = upper_c(c);
        if (!short_char_ok(c)) {
            c = '_';
            lossy = 1;
        }
        if (en < 3)
            ext[en++] = c;
        else
            lossy = 1;
    }
    if (bn == 0) {
        base[bn++] = '_';
        lossy = 1;
    }

    for (int n = 0; n <= 999999; n++) {
        if (n == 0 && lossy)
            continue; /* ohne Tilde nur, wenn nichts abgeschnitten oder ersetzt wurde (z.B. nur gemischte Gross-/Kleinschreibung) */
        char tail[10];
        int tl = 0, keep = bn;
        if (n > 0) {
            ksnprintf(tail, sizeof(tail), "~%d", n);
            tl = (int)strlen(tail);
            keep = 8 - tl;
            if (keep > bn)
                keep = bn;
        }
        memset(out, ' ', 11);
        for (int i = 0; i < keep; i++)
            out[i] = (uint8_t)base[i];
        for (int i = 0; i < tl; i++)
            out[keep + i] = (uint8_t)tail[i];
        for (int i = 0; i < en; i++)
            out[8 + i] = (uint8_t)ext[i];
        int t = alias_taken(dir, out);
        if (t < 0)
            return ERR_IO;
        if (!t)
            return 0;
    }
    return ERR_EXIST;
}

/* Legt einen Eintrag samt LFN-Eintraegen an (erweitert das Verzeichnis bei Bedarf). 0 = ok */
static int dir_add_name(uint32_t dir_cluster, const char *comp, uint8_t attr, uint32_t first_cluster, uint32_t size,
                        DirLoc *loc_out)
{
    size_t len = strlen(comp);
    if (!name_valid(comp, len))
        return ERR_INVAL;
    uint16_t units[256];
    int nunits = utf8_to_utf16(comp, units, 255);
    if (nunits <= 0)
        return ERR_INVAL;

    uint8_t n11[11], ntres = 0;
    int lfn = short_fits(comp, len, n11, &ntres) != 0;
    if (lfn) {
        int r = make_alias(dir_cluster, comp, len, n11);
        if (r != 0)
            return r;
        ntres = 0;
    } else {
        int t = alias_taken(dir_cluster, n11);
        if (t < 0)
            return ERR_IO;
        if (t)
            return ERR_EXIST;
    }
    int parts = lfn ? (nunits + 12) / 13 : 0;
    int need = parts + 1;

    /* Eine Reihe aufeinanderfolgender freier Eintraege suchen (nach dem ersten 0x00 ist alles frei) */
    DirIter it;
    it_init(&it, dir_cluster);
    DirLoc locs[LFN_PARTS + 2];
    DirEntry *e;
    DirLoc l;
    int run = 0, r;
    while ((r = it_next(&it, &e, &l)) == 1) {
        if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
            locs[run++] = l;
            if (run == need)
                break;
        } else {
            run = 0;
        }
    }
    if (r < 0)
        return ERR_IO;
    if (run < need) { /* Verzeichnis ist voll: Cluster anhaengen (nicht moeglich in der festen FAT12/16-Wurzel) */
        if (it.fixed_root)
            return ERR_NOSPC;
        uint32_t last = it.last_cluster;
        while (run < need) {
            uint32_t c = alloc_cluster(last);
            if (!c)
                return ERR_NOSPC;
            if (zero_cluster(c) != 0)
                return ERR_IO;
            last = c;
            for (uint32_t j = 0; j < vol.spc * 16 && run < need; j++) {
                locs[run].lba = cluster_lba(c) + j / 16;
                locs[run].off = (j % 16) * 32;
                run++;
            }
        }
    }

    uint8_t sum = lfn_checksum(n11);
    for (int i = 0; i < parts; i++) { /* LFN-Eintraege, hoechster Teil zuerst */
        int k = parts - i;
        uint8_t raw[32];
        memset(raw, 0, sizeof(raw));
        raw[0] = (uint8_t)(k | (i == 0 ? 0x40 : 0));
        raw[11] = ATTR_LFN;
        raw[13] = sum;
        static const uint8_t offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int j = 0; j < 13; j++) {
            int idx = (k - 1) * 13 + j;
            uint16_t u = idx < nunits ? units[idx] : idx == nunits ? 0x0000 : 0xFFFF;
            put16(raw + offs[j], u);
        }
        if (entry_write(&locs[i], (const DirEntry *)raw) != 0)
            return ERR_IO;
    }

    DirEntry n;
    memset(&n, 0, sizeof(n));
    memcpy(n.name, n11, 11);
    n.attr = attr;
    n.ntres = ntres;
    uint16_t stamp_date, stamp_time;
    dos_now(&stamp_date, &stamp_time);
    n.crt_date = n.acc_date = n.wrt_date = stamp_date;
    n.crt_time = n.wrt_time = stamp_time;
    n.cluster_hi = first_cluster >> 16;
    n.cluster_lo = first_cluster & 0xFFFF;
    n.size = size;
    if (entry_write(&locs[parts], &n) != 0)
        return ERR_IO;
    if (loc_out)
        *loc_out = locs[parts];
    return 0;
}

/* ---------- Pfade (FAT) ---------- */

/* Zerlegt den Pfad: liefert das Verzeichnis (Cluster) der letzten Komponente und deren Text.
 * *is_root = 1, wenn der Pfad die Wurzel selbst ist. Fehler: negativer Code. */
static int resolve_parent(const char *path, uint32_t *parent, char comp[256], int *is_root)
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
        int r = dir_lookup(dir, name, &item);
        if (r < 0)
            return ERR_IO;
        if (r == 0)
            return ERR_NOENT;
        if (!(item.e.attr & ATTR_DIR))
            return ERR_NOTDIR;
        dir = entry_cluster(&item.e);
        if (dir == 0)
            dir = vol.root_cluster; /* ".." zeigt bei der Wurzel auf Cluster 0 */
        p = next;
    }
}

/* Liegt `to` unterhalb von `from` (oder ist gleich)? Dann darf ein Verzeichnis nicht dorthin verschoben werden. */
static int path_inside(const char *from, const char *to)
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

/* ---------- Volumes erkennen und einbinden ---------- */

static void trim_label(char *label)
{
    for (int i = 10; i >= 0 && (label[i] == ' ' || label[i] == 0); i--)
        label[i] = 0;
}

/* Volume-Label aus dem Wurzelverzeichnis (Windows schreibt es dorthin; das im BPB ist oft "NO NAME") */
static void read_root_label(void)
{
    DirIter it;
    it_init(&it, 0);
    DirEntry *e;
    DirLoc l;
    while (it_next(&it, &e, &l) == 1) {
        if (e->name[0] == 0x00)
            break;
        if (e->name[0] != 0xE5 && e->attr != ATTR_LFN && (e->attr & ATTR_LABEL)) {
            memcpy(vol.label, e->name, 11);
            vol.label[11] = 0;
            trim_label(vol.label);
            return;
        }
    }
}

static void copy_str(char *dst, size_t max, const char *src)
{
    size_t i = 0;
    for (; src[i] && i < max - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

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
    int r = it_next(it, &e, &l);
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
        if (iequal(out->name, name))
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
static void x_scan_root(void)
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
        if (it_next(&it, &e, &lc) != 1)
            return -1;
        if (i >= l->ord) {
            if (entry_write(&lc, (const DirEntry *)(set + (i - l->ord) * 32)) != 0)
                return -1;
            if (i - l->ord == (uint32_t)l->nsec)
                return 0;
        }
    }
}

/* Aendert Startcluster, Groesse und Flags eines Satzes (Pruefsumme wird neu berechnet) */
static int x_update_set(const FatXLoc *l, uint32_t first, uint64_t size, uint64_t valid, int contig, int touch)
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
static int x_make_chain(uint32_t first, uint32_t n)
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
            xbm_put(first + i, 0);
        vol.free_hint = 2;
    } else {
        free_chain(first);
    }
}

/* Haengt einen Cluster an ein Verzeichnis an. `last` = letzter Cluster der Kette. 0 = Fehler */
static uint32_t x_dir_grow(XDirRef *d, uint32_t last)
{
    uint32_t cb = cluster_bytes();
    if (d->contig) { /* zusammenhaengend gespeichert: erst in eine FAT-Kette umwandeln */
        uint32_t n = (uint32_t)((d->size + cb - 1) / cb);
        if (n == 0 || x_make_chain(d->first, n) != 0)
            return 0;
        last = d->first + n - 1;
        d->contig = 0;
    }
    uint32_t c = alloc_cluster(last);
    if (!c)
        return 0;
    if (zero_cluster(c) != 0)
        return 0;
    if (!d->is_root) {
        d->size += cb;
        if (x_update_set(&d->self, d->first, d->size, d->size, 0, 0) != 0)
            return 0;
    }
    return c;
}

/* Legt einen Eintragssatz im Verzeichnis an. 0 = ok */
static int x_add(XDirRef *d, const char *name, uint16_t attr, uint32_t first, uint64_t size, uint64_t valid,
                 int contig, FatXLoc *out)
{
    size_t len = strlen(name);
    if (!name_valid(name, len))
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
    while ((r = it_next(&it, &e, &l)) == 1) {
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
        if (entry_write(&locs[i], (const DirEntry *)(set + i * 32)) != 0)
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

static int x_open(const char *path, int flags, FatFile *f)
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
            if (x_update_set(&f->xloc, 0, 0, 0, 0, 1) != 0)
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

static int x_readdir(const char *path, unsigned index, FatDirEntry *out)
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
        copy_str(out->name, sizeof(out->name), item.name);
        out->size = item.size;
        out->is_dir = item.is_dir;
        out->mtime = item.mtime;
        return 0;
    }
    return r < 0 ? ERR_IO : ERR_NOENT;
}

static int x_stat(const char *path, FatDirEntry *out)
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

static int x_mkdir(const char *path)
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
    if (!name_valid(comp, strlen(comp)))
        return ERR_INVAL;

    uint32_t c = alloc_cluster(0);
    if (!c)
        return ERR_NOSPC;
    if (zero_cluster(c) != 0) {
        free_chain(c);
        return ERR_IO;
    }
    FatXLoc loc;
    r = x_add(&d, comp, ATTR_DIR, c, cluster_bytes(), cluster_bytes(), 0, &loc);
    if (r != 0)
        free_chain(c);
    return r;
}

static int x_unlink(const char *path)
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

static int x_rename(const char *from, const char *to)
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
    if (item.is_dir && path_inside(from, to))
        return ERR_INVAL;

    FatXLoc loc;
    r = x_add(&d2, c2, item.attr, item.first, item.size, item.valid, item.contig, &loc);
    if (r != 0)
        return r;
    return x_delete_set(&item.loc) == 0 ? 0 : ERR_IO;
}

/* Erkennt Dateisysteme, die wir nicht koennen, damit `mount` sagen kann, was auf dem Datentraeger ist */
static void sniff_other(BlkDev *d, uint64_t base, const uint8_t *bs, char fstype[16])
{
    if (bs[510] == 0x55 && bs[511] == 0xAA) {
        if (memcmp(bs + 3, "EXFAT   ", 8) == 0) {
            copy_str(fstype, 16, "exFAT");
            return;
        }
        if (memcmp(bs + 3, "NTFS    ", 8) == 0) {
            copy_str(fstype, 16, "NTFS");
            return;
        }
        if (memcmp(bs + 3, "-FVE-FS-", 8) == 0) {
            copy_str(fstype, 16, "BitLocker");
            return;
        }
    }
    uint8_t s2[SECTOR]; /* ext2/3/4: Superblock ab Byte 1024, Magic 0xEF53 an Offset 56 darin */
    if (blk_read(d, base + 2, 1, s2) == 0 && s2[56] == 0x53 && s2[57] == 0xEF)
        copy_str(fstype, 16, "ext2/3/4");
}

/* Untersucht (dev, base). 1 = unterstuetzte FAT-Variante (in *out), 0 = nicht (fstype nennt ein erkanntes Fremdformat) */
static int classify(BlkDev *d, uint64_t base, FatVolume *out, char fstype[16])
{
    fstype[0] = 0;
    uint8_t bs[SECTOR];
    if (blk_read(d, base, 1, bs) != 0)
        return 0;

    if (bs[510] == 0x55 && bs[511] == 0xAA && memcmp(bs + 3, "EXFAT   ", 8) == 0) {
        uint32_t fat_off = le32(bs + 80), fat_len = le32(bs + 84), heap = le32(bs + 88), ccount = le32(bs + 92);
        uint32_t root = le32(bs + 96);
        int sector_shift = bs[108], cluster_shift = bs[109];
        if (sector_shift == 9 && cluster_shift <= 16 && fat_len && ccount && fat_off && heap >= fat_off + fat_len &&
            root >= 2 && root < ccount + 2) {
            memset(out, 0, sizeof(*out));
            out->used = 1;
            out->exfat = 1;
            out->fat_bits = 32; /* die FAT hat 32-Bit-Eintraege wie bei FAT32 */
            out->sectors = le64(bs + 72);
            out->spc = 1u << cluster_shift;
            out->nfats = bs[110];
            out->fat_size = fat_len;
            out->clusters = ccount;
            out->fat_lba = fat_off;
            out->data_lba = heap;
            out->root_cluster = root;
            out->eoc_min = 0x0FFFFFF8u;
            out->free_hint = 2;
            out->xflags = le16(bs + 106);
            return 1;
        }
        sniff_other(d, base, bs, fstype); /* kaputt oder mit 4-KiB-Sektoren: nur melden */
        return 0;
    }

    uint32_t spc = bs[13], reserved = le16(bs + 14), nfats = bs[16], root_entries = le16(bs + 17);
    uint32_t fatsz = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
    uint32_t total = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
    int plausible = bs[510] == 0x55 && bs[511] == 0xAA && (bs[0] == 0xEB || bs[0] == 0xE9) && le16(bs + 11) == SECTOR &&
                    spc && !(spc & (spc - 1)) && reserved && nfats && fatsz && total;
    if (!plausible) {
        sniff_other(d, base, bs, fstype);
        return 0;
    }

    uint32_t root_sectors = (root_entries * 32 + SECTOR - 1) / SECTOR;
    uint64_t data_lba = reserved + (uint64_t)nfats * fatsz + root_sectors;
    if (total <= data_lba)
        return 0;
    uint32_t clusters = (uint32_t)((total - data_lba) / spc);
    int bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    if ((bits == 32) != (root_entries == 0) || (bits == 32 && le16(bs + 22) != 0))
        return 0; /* Felder passen nicht zur Variante */
    if ((uint64_t)fatsz * SECTOR < ((uint64_t)clusters + 2) * (uint32_t)bits / 8)
        return 0; /* FAT zu klein fuer alle Cluster */

    memset(out, 0, sizeof(*out));
    out->used = 1;
    out->fat_bits = bits;
    out->sectors = total;
    out->spc = spc;
    out->nfats = nfats;
    out->fat_size = fatsz;
    out->clusters = clusters;
    out->fat_lba = reserved;
    out->data_lba = data_lba;
    out->root_sectors = root_sectors;
    out->root_lba = reserved + (uint64_t)nfats * fatsz;
    out->eoc_min = bits == 32 ? 0x0FFFFFF8u : bits == 16 ? 0xFFF8u : 0xFF8u;
    out->free_hint = 2;
    if (bits == 32) {
        out->root_cluster = le32(bs + 44);
        out->fsinfo_lba = le16(bs + 48);
    }
    memcpy(out->label, bs + (bits == 32 ? 71 : 43), 11);
    out->label[11] = 0;
    trim_label(out->label);
    return 1;
}

/* Schliesst das Einbinden ab: Label, Dirty-Flag, Schreibrecht, FSInfo/Freizaehler */
static void finish_volume(void)
{
    if (vol.exfat) {
        x_scan_root();
        /* nicht sauber getrennt (VolumeDirty), TexFAT (2 FATs) und fehlende Bitmap: nur lesen */
        vol.dirty = (vol.xflags & 2) != 0;
        vol.is_data = 0;
        vol.readonly = vol.dirty || default_readonly || vol.nfats != 1 || (vol.xflags & 1) || !vol.xbm_first ||
                       vol.xbm_bytes * 8 < vol.clusters;
        vol.fsinfo_lba = 0;
        return;
    }

    char bpb_label[12];
    memcpy(bpb_label, vol.label, sizeof(bpb_label));
    read_root_label();
    if (!vol.label[0])
        memcpy(vol.label, bpb_label, sizeof(vol.label));

    vol.is_data = vol.fat_bits == 32 && (strcmp(vol.label, DATA_LABEL) == 0 || strcmp(bpb_label, DATA_LABEL) == 0);
    if (vol.is_data && strcmp(vol.label, DATA_LABEL) != 0)
        memcpy(vol.label, bpb_label, sizeof(vol.label));

    /* Dirty-Flag: Bit 27 bzw. 15 in FAT[1] ist bei sauber getrenntem Volume gesetzt */
    uint32_t v1;
    if (vol.fat_bits >= 16 && fat_get(1, &v1) == 0)
        vol.dirty = vol.fat_bits == 32 ? !(v1 & 0x08000000u) : !(v1 & 0x8000u);
    vol.readonly = vol.dirty || (!vol.is_data && default_readonly);

    /* Freizaehler und Suchhinweis aus dem FSInfo-Sektor */
    vol.free_known = 0;
    if (vol.fsinfo_lba) {
        uint8_t fi[SECTOR];
        if (vread(vol.fsinfo_lba, 1, fi) == 0 && le32(fi) == 0x41615252 && le32(fi + 484) == 0x61417272 &&
            le32(fi + 508) == 0xAA550000) {
            uint32_t fc = le32(fi + 488), hint = le32(fi + 492);
            if (fc <= vol.clusters) {
                vol.free_count = fc;
                vol.free_known = 1;
            }
            if (hint >= 2 && hint < vol.clusters + 2)
                vol.free_hint = hint;
        } else {
            vol.fsinfo_lba = 0; /* ungueltig: nicht anfassen */
        }
    }
    if (!vol.free_known && vol.is_data && vol.fat_bits == 32) { /* eigenes Volume: einmal die FAT durchzaehlen */
        uint32_t free = 0;
        for (uint32_t c = 2; c < vol.clusters + 2; c++) {
            uint32_t v;
            if (fat_get(c, &v) != 0)
                break;
            if (v == 0)
                free++;
        }
        vol.free_count = free;
        vol.free_known = 1;
        vol.fsinfo_dirty = vol.fsinfo_lba != 0;
    }
}

static int known(BlkDev *d, uint64_t base)
{
    for (int i = 0; i < volume_count; i++)
        if (volumes[i].dev == d && volumes[i].base == base)
            return 1;
    for (int i = 0; i < foreign_count; i++)
        if (foreign_dev[i] == d && foreign_base[i] == base)
            return 1;
    return 0;
}

int fat_scan(void)
{
    mutex_lock(&lock);
    for (int i = 0; i < blk_count(); i++) {
        BlkDev *d = blk_get(i);
        PartInfo parts[PART_MAX];
        int np = part_scan(d, parts, PART_MAX);

        for (int c = -1; c < np; c++) { /* -1: das ganze Geraet */
            uint64_t base = c < 0 ? 0 : parts[c].start;
            if (known(d, base))
                continue;

            FatVolume cand;
            char fstype[16];
            if (classify(d, base, &cand, fstype)) {
                if (volume_count >= FAT_MAX_VOLUMES)
                    continue;
                cand.dev = d;
                cand.base = base;
                cand.part = c < 0 ? 0 : parts[c].index;
                if (V)
                    fat_cache_drop();
                volumes[volume_count] = cand;
                V = &volumes[volume_count];
                fat_buf_lba = ~0ULL;
                xbm_buf_sec = ~0ULL;
                finish_volume();
                volume_count++;
                char type[8];
                if (vol.exfat)
                    ksnprintf(type, sizeof(type), "exFAT");
                else
                    ksnprintf(type, sizeof(type), "FAT%d", vol.fat_bits);
                kprintf("fat: %s%s: %s '%s'%s%s\n", d->name, c < 0 ? "" : " (Partition)", type, vol.label,
                        vol.is_data ? " -> /disk" : "",
                        vol.dirty ? " (nicht sauber getrennt: nur lesbar)" : vol.readonly ? " (nur lesbar)" : "");
            } else if (fstype[0] && (c >= 0 || np == 0) && foreign_count < FAT_MAX_FOREIGN) {
                FatForeignInfo *f = &foreign[foreign_count];
                copy_str(f->device, sizeof(f->device), d->name);
                f->part = c < 0 ? 0 : parts[c].index;
                copy_str(f->fstype, sizeof(f->fstype), fstype);
                f->sectors = c < 0 ? d->sectors : parts[c].sectors;
                foreign_dev[foreign_count] = d;
                foreign_base[foreign_count] = base;
                foreign_count++;
                kprintf("fat: %s%s: %s (nicht unterstuetzt)\n", d->name, c < 0 ? "" : " (Partition)", fstype);
            }
        }
    }
    int n = volume_count;
    mutex_unlock(&lock);
    return n;
}

int fat_volume_count(void) { return volume_count; }
int fat_foreign_count(void) { return foreign_count; }

int fat_volume_info(int v, FatVolumeInfo *out)
{
    if (v < 0 || v >= volume_count)
        return ERR_NOENT;
    const FatVolume *x = &volumes[v];
    copy_str(out->device, sizeof(out->device), x->dev->name);
    out->part = x->part;
    copy_str(out->label, sizeof(out->label), x->label);
    out->fat_bits = x->exfat ? 0 : x->fat_bits;
    out->sectors = x->sectors;
    out->readonly = x->readonly;
    out->is_data = x->is_data;
    out->dirty = x->dirty;
    return 0;
}

int fat_foreign_info(int i, FatForeignInfo *out)
{
    if (i < 0 || i >= foreign_count)
        return ERR_NOENT;
    *out = foreign[i];
    return 0;
}

int fat_find_data_volume(void)
{
    for (int i = 0; i < volume_count; i++)
        if (volumes[i].is_data)
            return i;
    return -1;
}

uint32_t fat_total_clusters(int v) { return v >= 0 && v < volume_count ? volumes[v].clusters : 0; }
uint32_t fat_cluster_bytes(int v)  { return v >= 0 && v < volume_count ? volumes[v].spc * SECTOR : 0; }

uint32_t fat_free_clusters(int v)
{
    mutex_lock(&lock);
    uint32_t free = 0;
    if (activate(v) == 0) {
        if (vol.exfat && vol.xbm_first) {
            free = x_count_free();
        } else {
            for (uint32_t c = 2; c < vol.clusters + 2; c++) {
                uint32_t val;
                if (fat_get(c, &val) != 0)
                    break;
                if (val == 0)
                    free++;
            }
        }
    }
    mutex_unlock(&lock);
    return free;
}

/* ---------- Dateien ---------- */

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
        if (n && x_make_chain(f->first_cluster, n) != 0)
            return ERR_IO;
        f->contiguous = 0;
    }
    if (f->first_cluster == 0) {
        if (!alloc)
            return -1;
        uint32_t c = alloc_cluster(0);
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
            next = alloc_cluster(f->cur_cluster);
            if (!next)
                return ERR_NOSPC;
        }
        f->cur_cluster = next;
        f->cur_index++;
    }
    *out = f->cur_cluster;
    return 0;
}

static int file_update_entry(const FatFile *f)
{
    if (vol.exfat)
        return x_update_set(&f->xloc, f->first_cluster, f->size, f->size, f->contiguous && f->first_cluster, 1) == 0
                   ? 0 : ERR_IO;
    DirLoc loc = {f->dirent_lba, f->dirent_off};
    DirEntry e;
    if (entry_read(&loc, &e) != 0)
        return ERR_IO;
    e.cluster_hi = f->first_cluster >> 16;
    e.cluster_lo = f->first_cluster & 0xFFFF;
    e.size = (uint32_t)f->size;
    uint16_t stamp_date, stamp_time;
    dos_now(&stamp_date, &stamp_time);
    e.wrt_date = e.acc_date = stamp_date;
    e.wrt_time = stamp_time;
    e.attr |= ATTR_ARCHIVE;
    return entry_write(&loc, &e) == 0 ? 0 : ERR_IO;
}

int fat_open(int v, const char *path, int flags, FatFile *f)
{
    mutex_lock(&lock);
    int ret = activate(v);
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
        ret = x_open(path, flags, f);
        goto out;
    }

    uint32_t parent;
    char comp[256];
    int is_root;
    DirItem item;

    ret = resolve_parent(path, &parent, comp, &is_root);
    if (ret == 0 && is_root)
        ret = ERR_ISDIR;
    if (ret != 0)
        goto out;

    int r = dir_lookup(parent, comp, &item);
    if (r < 0) {
        ret = ERR_IO;
        goto out;
    }

    if (r == 1) {
        if (item.e.attr & ATTR_DIR) {
            ret = ERR_ISDIR;
            goto out;
        }
        f->first_cluster = entry_cluster(&item.e);
        f->size = item.e.size;
        f->dirent_lba = item.loc.lba;
        f->dirent_off = item.loc.off;
        if ((flags & FAT_O_TRUNC) && f->writable) {
            uint32_t old = f->first_cluster;
            f->first_cluster = 0;
            f->size = 0;
            ret = file_update_entry(f);
            if (ret == 0)
                free_chain(old); /* erst den Eintrag, dann die Cluster */
            goto out;
        }
    } else {
        if (!(flags & FAT_O_CREAT)) {
            ret = ERR_NOENT;
            goto out;
        }
        DirLoc loc;
        ret = dir_add_name(parent, comp, ATTR_ARCHIVE, 0, 0, &loc);
        if (ret != 0)
            goto out;
        f->dirent_lba = loc.lba;
        f->dirent_off = loc.off;
    }
    ret = 0;
out:
    if (V && vol.used)
        fsinfo_sync();
    mutex_unlock(&lock);
    return ret;
}

int64_t fat_read(FatFile *f, void *buf, uint64_t len)
{
    mutex_lock(&lock);
    uint8_t *out = buf;
    uint64_t done = 0;
    int64_t ret = 0;

    if (activate(f->volume) != 0) {
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
            uint32_t sectors = (uint32_t)((len - done) / SECTOR);
            if (sectors > vol.spc - sec)
                sectors = vol.spc - sec;
            if (vread(lba, sectors, out + done) != 0) {
                ret = done ? (int64_t)done : ERR_IO;
                goto out;
            }
            chunk = (uint64_t)sectors * SECTOR;
        } else {
            uint8_t tmp[SECTOR];
            if (vread(lba, 1, tmp) != 0) {
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
    mutex_unlock(&lock);
    return ret;
}

int64_t fat_write(FatFile *f, const void *buf, uint64_t len)
{
    if (!f->writable)
        return ERR_BADF;
    if (len == 0)
        return 0;

    mutex_lock(&lock);
    if (activate(f->volume) != 0 || vol.readonly) {
        mutex_unlock(&lock);
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
            uint32_t sectors = (uint32_t)((len - done) / SECTOR);
            if (sectors > vol.spc - sec)
                sectors = vol.spc - sec;
            if (vwrite(lba, sectors, in + done) != 0) {
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
            if ((chunk < SECTOR) && vread(lba, 1, tmp) != 0) {
                err = ERR_IO;
                break;
            }
            memcpy(tmp + boff, in + done, chunk);
            if (vwrite(lba, 1, tmp) != 0) {
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
    fsinfo_sync();
    mutex_unlock(&lock);
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
    mutex_lock(&lock);
    int ret = activate(v);
    if (ret != 0)
        goto out;
    if (vol.readonly) {
        ret = ERR_ROFS;
        goto out;
    }
    if (vol.exfat) {
        ret = x_mkdir(path);
        goto out;
    }

    uint32_t parent;
    char comp[256];
    int is_root;
    DirItem item;

    ret = resolve_parent(path, &parent, comp, &is_root);
    if (ret == 0 && is_root)
        ret = ERR_EXIST;
    if (ret != 0)
        goto out;
    int r = dir_lookup(parent, comp, &item);
    if (r < 0) {
        ret = ERR_IO;
        goto out;
    }
    if (r == 1) {
        ret = ERR_EXIST;
        goto out;
    }
    if (!name_valid(comp, strlen(comp))) {
        ret = ERR_INVAL;
        goto out;
    }

    uint32_t c = alloc_cluster(0);
    if (!c) {
        ret = ERR_NOSPC;
        goto out;
    }
    if (zero_cluster(c) != 0) {
        free_chain(c);
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
    if (vwrite(cluster_lba(c), 1, sec) != 0) {
        free_chain(c);
        ret = ERR_IO;
        goto out;
    }

    ret = dir_add_name(parent, comp, ATTR_DIR, c, 0, 0);
    if (ret != 0)
        free_chain(c);
out:
    if (V && vol.used)
        fsinfo_sync();
    mutex_unlock(&lock);
    return ret;
}

/* Loescht den Kurznamen-Eintrag samt zugehoerigen LFN-Eintraegen */
static int delete_item(const DirItem *item)
{
    for (int i = 0; i < item->lfn_count; i++)
        if (entry_delete(&item->lfn_locs[i]) != 0)
            return ERR_IO;
    return entry_delete(&item->loc) == 0 ? 0 : ERR_IO;
}

int fat_unlink(int v, const char *path)
{
    mutex_lock(&lock);
    int ret = activate(v);
    if (ret != 0)
        goto out;
    if (vol.readonly) {
        ret = ERR_ROFS;
        goto out;
    }
    if (vol.exfat) {
        ret = x_unlink(path);
        goto out;
    }

    uint32_t parent;
    char comp[256];
    int is_root;
    DirItem item;

    ret = resolve_parent(path, &parent, comp, &is_root);
    if (ret == 0 && is_root)
        ret = ERR_INVAL;
    if (ret != 0)
        goto out;
    int r = dir_lookup(parent, comp, &item);
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
        it_init(&it, entry_cluster(&item.e));
        DirItem child;
        int rr;
        while ((rr = dir_next_item(&it, &child)) == 1) {
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
        free_chain(entry_cluster(&item.e));
out:
    if (V && vol.used)
        fsinfo_sync();
    mutex_unlock(&lock);
    return ret;
}

int fat_readdir(int v, const char *path, unsigned index, FatDirEntry *out)
{
    mutex_lock(&lock);
    int ret = activate(v);
    if (ret != 0)
        goto out;
    if (vol.exfat) {
        ret = x_readdir(path, index, out);
        goto out;
    }

    uint32_t parent, dir;
    char comp[256];
    int is_root;
    DirItem item;

    ret = resolve_parent(path, &parent, comp, &is_root);
    if (ret != 0)
        goto out;
    if (is_root) {
        dir = vol.root_cluster;
    } else {
        int r = dir_lookup(parent, comp, &item);
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
        dir = entry_cluster(&item.e);
    }

    DirIter it;
    it_init(&it, dir);
    unsigned seen = 0;
    int rr;
    ret = ERR_NOENT;
    while ((rr = dir_next_item(&it, &item)) == 1) {
        if (item.e.name[0] == '.') /* "." und ".." */
            continue;
        if (seen++ != index)
            continue;
        if (item.has_long)
            copy_str(out->name, sizeof(out->name), item.lname);
        else
            short_display(&item.e, out->name);
        out->size = item.e.size;
        out->is_dir = (item.e.attr & ATTR_DIR) != 0;
        out->mtime = dos_to_unix(item.e.wrt_date, item.e.wrt_time);
        ret = 0;
        break;
    }
    if (rr < 0)
        ret = ERR_IO;
out:
    mutex_unlock(&lock);
    return ret;
}

int fat_rename(int v, const char *from, const char *to)
{
    mutex_lock(&lock);
    int ret = activate(v);
    if (ret != 0)
        goto out;
    if (vol.readonly) {
        ret = ERR_ROFS;
        goto out;
    }
    if (vol.exfat) {
        ret = x_rename(from, to);
        goto out;
    }

    uint32_t p_from, p_to;
    char c_from[256], c_to[256];
    int root_from, root_to;
    DirItem item, other;

    ret = resolve_parent(from, &p_from, c_from, &root_from);
    if (ret == 0)
        ret = resolve_parent(to, &p_to, c_to, &root_to);
    if (ret == 0 && (root_from || root_to))
        ret = ERR_INVAL;
    if (ret != 0)
        goto out;

    int r = dir_lookup(p_from, c_from, &item);
    if (r < 0) {
        ret = ERR_IO;
        goto out;
    }
    if (r == 0) {
        ret = ERR_NOENT;
        goto out;
    }
    r = dir_lookup(p_to, c_to, &other);
    if (r < 0) {
        ret = ERR_IO;
        goto out;
    }
    if (r == 1) {
        ret = (item.loc.lba == other.loc.lba && item.loc.off == other.loc.off) ? 0 : ERR_EXIST; /* auf sich selbst: nichts zu tun */
        goto out;
    }
    if ((item.e.attr & ATTR_DIR) && path_inside(from, to)) { /* ein Verzeichnis nicht in sich selbst verschieben */
        ret = ERR_INVAL;
        goto out;
    }

    /* Erst den neuen Eintrag anlegen, dann den alten (samt LFN-Eintraegen) loeschen: bei einem Absturz dazwischen ist nichts verloren */
    ret = dir_add_name(p_to, c_to, item.e.attr, entry_cluster(&item.e), item.e.size, 0);
    if (ret != 0)
        goto out;
    if ((item.e.attr & ATTR_DIR) && p_from != p_to) { /* ".." des verschobenen Verzeichnisses anpassen */
        uint32_t dc = entry_cluster(&item.e);
        DirLoc dd = {cluster_lba(dc), 32};
        DirEntry e;
        if (valid_cluster(dc) && entry_read(&dd, &e) == 0) {
            uint32_t up = p_to == vol.root_cluster ? 0 : p_to;
            e.cluster_hi = up >> 16;
            e.cluster_lo = up & 0xFFFF;
            entry_write(&dd, &e);
        }
    }
    ret = delete_item(&item);
out:
    if (V && vol.used)
        fsinfo_sync();
    mutex_unlock(&lock);
    return ret;
}

int fat_stat(int v, const char *path, FatDirEntry *out)
{
    mutex_lock(&lock);
    int ret = activate(v);
    if (ret != 0)
        goto out;
    if (vol.exfat) {
        ret = x_stat(path, out);
        goto out;
    }

    uint32_t parent;
    char comp[256];
    int is_root;
    DirItem item;

    out->name[0] = 0;
    ret = resolve_parent(path, &parent, comp, &is_root);
    if (ret == 0 && is_root) {
        out->size = 0;
        out->is_dir = 1;
        out->mtime = 0;
    } else if (ret == 0) {
        int r = dir_lookup(parent, comp, &item);
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
    mutex_unlock(&lock);
    return ret;
}
