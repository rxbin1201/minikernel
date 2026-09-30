#include "part.h"
#include "kprintf.h"
#include "string.h"

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return le32(p) | ((uint64_t)le32(p + 4) << 32); }

static int scan_gpt(BlkDev *d, PartInfo *out, int max)
{
    uint8_t hdr[BLK_SECTOR_SIZE];
    if (blk_read(d, 1, 1, hdr) != 0 || memcmp(hdr, "EFI PART", 8) != 0)
        return -1;

    uint64_t entries_lba = le64(hdr + 72);
    uint32_t count = le32(hdr + 80);
    uint32_t size = le32(hdr + 84);
    if (size < 128 || size > BLK_SECTOR_SIZE || (BLK_SECTOR_SIZE % size) != 0)
        return -1;
    if (count > 128)
        count = 128;

    int n = 0;
    uint8_t sec[BLK_SECTOR_SIZE];
    for (uint32_t i = 0; i < count && n < max; i++) {
        uint32_t per_sector = BLK_SECTOR_SIZE / size;
        if (i % per_sector == 0 && blk_read(d, entries_lba + i / per_sector, 1, sec) != 0)
            return n;
        const uint8_t *e = sec + (i % per_sector) * size;

        int used = 0; /* Typ-GUID nur Nullen = unbenutzter Eintrag */
        for (int k = 0; k < 16; k++)
            used |= e[k];
        if (!used)
            continue;

        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (last < first || last >= d->sectors)
            continue;
        out[n].start = first;
        out[n].sectors = last - first + 1;
        out[n].gpt = 1;
        out[n].mbr_type = 0;
        out[n].index = (int)i + 1;
        n++;
    }
    return n;
}

int part_scan(BlkDev *d, PartInfo *out, int max)
{
    uint8_t mbr[BLK_SECTOR_SIZE];
    if (blk_read(d, 0, 1, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA)
        return 0;

    int n = 0;
    for (int i = 0; i < 4 && n < max; i++) {
        const uint8_t *e = mbr + 446 + 16 * i;
        uint8_t type = e[4];
        uint32_t start = le32(e + 8), count = le32(e + 12);
        if (type == 0xEE) { /* Schutz-MBR: die eigentliche Tabelle ist die GPT */
            int g = scan_gpt(d, out, max);
            return g < 0 ? 0 : g;
        }
        if (type == 0 || type == 0x05 || type == 0x0F || count == 0)
            continue; /* leer oder erweiterte Partition (nicht unterstuetzt) */
        out[n].start = start;
        out[n].sectors = count;
        out[n].gpt = 0;
        out[n].mbr_type = type;
        out[n].index = i + 1;
        n++;
    }
    return n;
}
