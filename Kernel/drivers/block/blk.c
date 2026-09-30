#include "drivers/block/blk.h"
#include "lib/kprintf.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "lib/string.h"

static BlkDev *devices[BLK_MAX_DEVICES];
static int device_count;
static volatile unsigned generation;

unsigned blk_generation(void)
{
    return generation;
}

int blk_register(BlkDev *d)
{
    if (device_count >= BLK_MAX_DEVICES)
        return -1;
    BlkDev *copy = kmalloc(sizeof(*copy));
    if (!copy)
        return -1;
    *copy = *d;
    devices[device_count++] = copy;
    generation++;
    return 0;
}

int blk_count(void)
{
    return device_count;
}

BlkDev *blk_get(int index)
{
    return index >= 0 && index < device_count ? devices[index] : 0;
}

int blk_init(void)
{
    device_count = 0;
    nvme_probe();
    ahci_probe();
    virtio_blk_probe();

    kprintf("blk: %d Geraet(e)\n", device_count);
    for (int i = 0; i < device_count; i++) {
        BlkDev *d = devices[i];
        kprintf("  %-8s %lu Sektoren (%lu MiB)  %s\n", d->name, d->sectors, d->sectors * BLK_SECTOR_SIZE / (1024 * 1024),
                d->model);
    }
    return device_count;
}

int blk_read(BlkDev *d, uint64_t lba, uint32_t count, void *buf)
{
    if (!d || lba + count < lba || lba + count > d->sectors)
        return -1;
    return count ? d->read(d, lba, count, buf) : 0;
}

int blk_write(BlkDev *d, uint64_t lba, uint32_t count, const void *buf)
{
    if (!d || lba + count < lba || lba + count > d->sectors)
        return -1;
    return count ? d->write(d, lba, count, buf) : 0;
}

int blk_flush(BlkDev *d)
{
    return d && d->flush ? d->flush(d) : 0;
}

void blk_flush_all(void)
{
    for (int i = 0; i < device_count; i++)
        blk_flush(devices[i]);
}

void *blk_dma_alloc(uint64_t bytes)
{
    uint64_t pages = (bytes + 4095) / 4096;
    uint64_t frame = pmm_alloc_frames(pages);
    if (!frame)
        return 0;
    memset((void *)frame, 0, pages * 4096); /* PMM-Frames sind identity-mapped: physisch = virtuell */
    return (void *)frame;
}

void blk_trim_model(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0))
        s[--n] = 0;
}
