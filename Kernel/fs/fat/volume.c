/* FAT/exFAT: Volumes aktivieren, erkennen und einbinden */

#include "drivers/block/part.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "core/syscall.h" /* ERR_* */
#include "fs/fat/fat_internal.h"

static FatVolume  fat_volumes[FAT_MAX_VOLUMES];
static int        fat_nvolumes;
FatVolume        *fat_cur;       /* Volume, mit dem gerade gearbeitet wird */

/* Volumes, die keine unterstuetzte FAT-Variante sind (fuer mount) */
static FatForeignInfo fat_foreign[FAT_MAX_FOREIGN];
static BlkDev        *fat_foreign_dev[FAT_MAX_FOREIGN];
static uint64_t       fat_foreign_base[FAT_MAX_FOREIGN];
static int            fat_nforeign;

static int fat_default_readonly;
Mutex      fat_lock = MUTEX_INIT;
uint64_t   fat_buf_lba = ~0ULL;  /* Caches in table.c; beim Wechsel des Volumes verworfen */
uint64_t   xbm_buf_sec = ~0ULL;

void fat_set_default_readonly(int readonly)
{
    fat_default_readonly = readonly != 0;
}

int fat_activate(int v)
{
    if (v < 0 || v >= fat_nvolumes || !fat_volumes[v].used)
        return ERR_NOENT;
    if (fat_cur != &fat_volumes[v]) {
        if (fat_cur)
            fat_cache_drop(); /* gesammelte FAT-Aenderungen gehoeren zum bisherigen Volume */
        fat_cur = &fat_volumes[v];
        fat_buf_lba = ~0ULL;
        xbm_buf_sec = ~0ULL;
    }
    return 0;
}

/* Alle Zugriffe des Dateisystems sind relativ zum Volume-Anfang */
int fat_vread(uint64_t lba, uint32_t count, void *buf)
{
    return blk_read(vol.dev, vol.base + lba, count, buf);
}

int fat_vwrite(uint64_t lba, uint32_t count, const void *buf)
{
    if (vol.readonly)
        return -1; /* letzte Sicherung: auf schreibgeschuetzte Volumes geht nie ein Schreibzugriff */
    vol.wrote = 1;
    return blk_write(vol.dev, vol.base + lba, count, buf);
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
    fat_it_init(&it, 0);
    DirEntry *e;
    DirLoc l;
    while (fat_it_next(&it, &e, &l) == 1) {
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

void fat_copy_str(char *dst, size_t max, const char *src)
{
    size_t i = 0;
    for (; src[i] && i < max - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

/* ---------- Volumes untersuchen und einbinden ---------- */

/* Erkennt Dateisysteme, die wir nicht koennen, damit `mount` sagen kann, was auf dem Datentraeger ist */
static void sniff_other(BlkDev *d, uint64_t base, const uint8_t *bs, char fstype[16])
{
    if (bs[510] == 0x55 && bs[511] == 0xAA) {
        if (memcmp(bs + 3, "EXFAT   ", 8) == 0) {
            fat_copy_str(fstype, 16, "exFAT");
            return;
        }
        if (memcmp(bs + 3, "NTFS    ", 8) == 0) {
            fat_copy_str(fstype, 16, "NTFS");
            return;
        }
        if (memcmp(bs + 3, "-FVE-FS-", 8) == 0) {
            fat_copy_str(fstype, 16, "BitLocker");
            return;
        }
    }
    uint8_t s2[SECTOR]; /* ext2/3/4: Superblock ab Byte 1024, Magic 0xEF53 an Offset 56 darin */
    if (blk_read(d, base + 2, 1, s2) == 0 && s2[56] == 0x53 && s2[57] == 0xEF)
        fat_copy_str(fstype, 16, "ext2/3/4");
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
        exfat_scan_root();
        /* nicht sauber getrennt (VolumeDirty), TexFAT (2 FATs) und fehlende Bitmap: nur lesen */
        vol.dirty = (vol.xflags & 2) != 0;
        vol.is_data = 0;
        vol.readonly = vol.dirty || fat_default_readonly || vol.nfats != 1 || (vol.xflags & 1) || !vol.xbm_first ||
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
    vol.readonly = vol.dirty || (!vol.is_data && fat_default_readonly);

    /* Freizaehler und Suchhinweis aus dem FSInfo-Sektor */
    vol.free_known = 0;
    if (vol.fsinfo_lba) {
        uint8_t fi[SECTOR];
        if (fat_vread(vol.fsinfo_lba, 1, fi) == 0 && le32(fi) == 0x41615252 && le32(fi + 484) == 0x61417272 &&
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
    for (int i = 0; i < fat_nvolumes; i++)
        if (fat_volumes[i].dev == d && fat_volumes[i].base == base)
            return 1;
    for (int i = 0; i < fat_nforeign; i++)
        if (fat_foreign_dev[i] == d && fat_foreign_base[i] == base)
            return 1;
    return 0;
}

int fat_scan(void)
{
    mutex_lock(&fat_lock);
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
                if (fat_nvolumes >= FAT_MAX_VOLUMES)
                    continue;
                cand.dev = d;
                cand.base = base;
                cand.part = c < 0 ? 0 : parts[c].index;
                if (fat_cur)
                    fat_cache_drop();
                fat_volumes[fat_nvolumes] = cand;
                fat_cur = &fat_volumes[fat_nvolumes];
                fat_buf_lba = ~0ULL;
                xbm_buf_sec = ~0ULL;
                finish_volume();
                fat_nvolumes++;
                char type[8];
                if (vol.exfat)
                    ksnprintf(type, sizeof(type), "exFAT");
                else
                    ksnprintf(type, sizeof(type), "FAT%d", vol.fat_bits);
                kprintf("fat: %s%s: %s '%s'%s%s\n", d->name, c < 0 ? "" : " (Partition)", type, vol.label,
                        vol.is_data ? " -> /disk" : "",
                        vol.dirty ? " (nicht sauber getrennt: nur lesbar)" : vol.readonly ? " (nur lesbar)" : "");
            } else if (fstype[0] && (c >= 0 || np == 0) && fat_nforeign < FAT_MAX_FOREIGN) {
                FatForeignInfo *f = &fat_foreign[fat_nforeign];
                fat_copy_str(f->device, sizeof(f->device), d->name);
                f->part = c < 0 ? 0 : parts[c].index;
                fat_copy_str(f->fstype, sizeof(f->fstype), fstype);
                f->sectors = c < 0 ? d->sectors : parts[c].sectors;
                fat_foreign_dev[fat_nforeign] = d;
                fat_foreign_base[fat_nforeign] = base;
                fat_nforeign++;
                kprintf("fat: %s%s: %s (nicht unterstuetzt)\n", d->name, c < 0 ? "" : " (Partition)", fstype);
            }
        }
    }
    int n = fat_nvolumes;
    mutex_unlock(&fat_lock);
    return n;
}

int fat_volume_count(void) { return fat_nvolumes; }

int fat_foreign_count(void) { return fat_nforeign; }

int fat_volume_info(int v, FatVolumeInfo *out)
{
    if (v < 0 || v >= fat_nvolumes)
        return ERR_NOENT;
    const FatVolume *x = &fat_volumes[v];
    fat_copy_str(out->device, sizeof(out->device), x->dev->name);
    out->part = x->part;
    fat_copy_str(out->label, sizeof(out->label), x->label);
    out->fat_bits = x->exfat ? 0 : x->fat_bits;
    out->sectors = x->sectors;
    out->readonly = x->readonly;
    out->is_data = x->is_data;
    out->dirty = x->dirty;
    return 0;
}

int fat_foreign_info(int i, FatForeignInfo *out)
{
    if (i < 0 || i >= fat_nforeign)
        return ERR_NOENT;
    *out = fat_foreign[i];
    return 0;
}

int fat_find_data_volume(void)
{
    for (int i = 0; i < fat_nvolumes; i++)
        if (fat_volumes[i].is_data)
            return i;
    return -1;
}

uint32_t fat_total_clusters(int v) { return v >= 0 && v < fat_nvolumes ? fat_volumes[v].clusters : 0; }

uint32_t fat_cluster_bytes(int v)  { return v >= 0 && v < fat_nvolumes ? fat_volumes[v].spc * SECTOR : 0; }

uint32_t fat_free_clusters(int v)
{
    mutex_lock(&fat_lock);
    uint32_t free = 0;
    if (fat_activate(v) == 0) {
        if (vol.exfat && vol.xbm_first) {
            free = exfat_count_free();
        } else if (vol.free_known) { /* Zaehler, den Belegen/Freigeben mitfuehren: nicht jedesmal die ganze FAT lesen */
            free = vol.free_count;
        } else { /* einmal durchzaehlen, danach fuehren ihn Belegen/Freigeben mit */
            int ok = 1;
            for (uint32_t c = 2; c < vol.clusters + 2; c++) {
                uint32_t val;
                if (fat_get(c, &val) != 0) {
                    ok = 0;
                    break;
                }
                if (val == 0)
                    free++;
            }
            if (ok) {
                vol.free_count = free;
                vol.free_known = 1;
            }
        }
    }
    mutex_unlock(&fat_lock);
    return free;
}
