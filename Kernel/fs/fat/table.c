/* FAT/exFAT: Zuordnungstabelle (FAT), exFAT-Belegungs-Bitmap, Cluster belegen und freigeben */

#include "lib/string.h"
#include "fs/fat/fat_internal.h"

static int fat_store(uint64_t lba, const uint8_t *sec);

/* Ein-Sektor-Caches fuer die FAT (Write-Back: Aenderungen werden gesammelt und spaetestens am Ende jedes
 * Vorgangs in alle FAT-Kopien geschrieben) und die exFAT-Bitmap (Write-Through) des aktiven Volumes */
static uint8_t  fat_buf[SECTOR];
static int      fat_buf_dirty;
static uint8_t  xbm_buf[SECTOR];

/* ---------- FAT ---------- */

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

void fat_cache_drop(void)
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
    if (fat_vread(lba, 1, fat_buf) != 0) {
        fat_buf_lba = ~0ULL;
        return -1;
    }
    fat_buf_lba = lba;
    return 0;
}

int fat_get(uint32_t cluster, uint32_t *value)
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
        if (fat_vwrite(lba + (uint64_t)i * vol.fat_size, 1, sec) != 0)
            return -1;
    return 0;
}

static uint32_t eoc_mark(void)
{
    return vol.exfat ? 0xFFFFFFFFu : vol.fat_bits == 32 ? CLUSTER_MASK : vol.fat_bits == 16 ? 0xFFFFu : 0xFFFu;
}

int fat_set(uint32_t cluster, uint32_t value)
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
    if (fat_vread(lba, 1, s0) != 0 || (span && fat_vread(lba + 1, 1, s1) != 0))
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
    if (fat_vread(cluster_lba(vol.xbm_first) + sec, 1, xbm_buf) != 0)
        return -1;
    xbm_buf_sec = sec;
    return 0;
}

int fat_xbm_put(uint32_t cluster, int used)
{
    uint64_t bit = cluster - 2, byte = bit / 8, sec = byte / SECTOR;
    if (byte >= vol.xbm_bytes || xbm_load(sec) != 0)
        return -1;
    if (used)
        xbm_buf[byte % SECTOR] |= (uint8_t)(1u << (bit % 8));
    else
        xbm_buf[byte % SECTOR] &= (uint8_t)~(1u << (bit % 8));
    return fat_vwrite(cluster_lba(vol.xbm_first) + sec, 1, xbm_buf);
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
        if (fat_xbm_put(i + 2, 1) != 0)
            return 0;
        vol.free_hint = i + 3;
        return i + 2;
    }
    return 0;
}

uint32_t exfat_count_free(void)
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
uint32_t fat_alloc_cluster(uint32_t prev)
{
    if (vol.readonly)
        return 0;
    if (vol.exfat) {
        uint32_t c = xbm_alloc();
        if (!c)
            return 0;
        if (fat_set(c, 0xFFFFFFFFu) != 0 || (prev && fat_set(prev, c) != 0)) {
            fat_xbm_put(c, 0);
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
void fat_free_chain(uint32_t c)
{
    uint32_t guard = vol.clusters + 2;
    while (valid_cluster(c) && guard--) {
        uint32_t next;
        if (fat_get(c, &next) != 0 || fat_set(c, 0) != 0)
            return;
        if (vol.exfat) {
            fat_xbm_put(c, 0);
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

int fat_zero_cluster(uint32_t c)
{
    uint8_t zeros[SECTOR];
    memset(zeros, 0, sizeof(zeros));
    for (uint32_t s = 0; s < vol.spc; s++)
        if (fat_vwrite(cluster_lba(c) + s, 1, zeros) != 0)
            return -1;
    return 0;
}

/* Schreibt Freizaehler und Suchhinweis in den FSInfo-Sektor (rein informativ, aber fsck.fat vergleicht ihn). */
void fat_fsinfo_sync(void)
{
    fat_flush();
    if (vol.readonly)
        return;
    if (vol.fsinfo_dirty && vol.fsinfo_lba && vol.free_known) {
        uint8_t sec[SECTOR];
        if (fat_vread(vol.fsinfo_lba, 1, sec) == 0) {
            memcpy(sec + 488, &vol.free_count, 4);
            memcpy(sec + 492, &vol.free_hint, 4);
            if (fat_vwrite(vol.fsinfo_lba, 1, sec) == 0)
                vol.fsinfo_dirty = 0;
        }
    }
    if (vol.wrote) { /* Daten sollen wirklich auf dem Medium sein, nicht nur im Cache der Platte */
        blk_flush(vol.dev);
        vol.wrote = 0;
    }
}
