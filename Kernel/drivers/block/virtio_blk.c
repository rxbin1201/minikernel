#include "drivers/block/blk.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/io.h"
#include "lib/kprintf.h"
#include "drivers/pci.h"
#include "mm/pmm.h"
#include "core/sched.h"
#include "lib/string.h"

/* virtio 0.9.5 "legacy" ueber PCI-I/O-Ports (BAR0). QEMU: -device virtio-blk-pci,...,disable-modern=on */

#define VIRTIO_VENDOR       0x1AF4
#define VIRTIO_BLK_LEGACY   0x1001

#define REG_DEVICE_FEATURES 0x00
#define REG_GUEST_FEATURES  0x04
#define REG_QUEUE_PFN       0x08
#define REG_QUEUE_SIZE      0x0C
#define REG_QUEUE_SELECT    0x0E
#define REG_QUEUE_NOTIFY    0x10
#define REG_STATUS          0x12
#define REG_ISR             0x13
#define REG_CONFIG          0x14 /* virtio-blk: capacity (u64, in 512-Byte-Sektoren) */

#define STATUS_ACK          1
#define STATUS_DRIVER       2
#define STATUS_DRIVER_OK    4

#define DESC_NEXT           1
#define DESC_WRITE          2 /* Geraet schreibt in diesen Puffer */
#define AVAIL_NO_INTERRUPT  1

#define BLK_T_IN            0
#define BLK_T_OUT           1

#define BOUNCE_SECTORS      64 /* max. Sektoren pro Anfrage (32 KiB) */
#define PAGE                4096ULL

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) VringDesc;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) VringAvail;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) VringUsedElem;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    VringUsedElem ring[];
} __attribute__((packed)) VringUsed;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) BlkReqHeader;

static struct {
    int      ready;
    uint16_t io;
    uint16_t qsize;
    volatile VringDesc  *desc;
    volatile VringAvail *avail;
    volatile VringUsed  *used;
    uint16_t last_used;
    uint64_t capacity;

    volatile BlkReqHeader *hdr;   /* Anfragekopf + Statusbyte, DMA-Speicher */
    volatile uint8_t      *status;
    uint8_t               *bounce; /* Datenpuffer fuer DMA (physisch = virtuell, PMM-Frames) */
} dev;

static Mutex lock = MUTEX_INIT;

static inline void barrier(void)
{
    __asm__ __volatile__("mfence" : : : "memory");
}

static int virtio_init(void)
{
    PciDevice pci;
    if (pci_find(VIRTIO_VENDOR, VIRTIO_BLK_LEGACY, &pci) != 0)
        return -1; /* nicht vorhanden: kein Fehler */
    if (!(pci.bar[0] & 1)) {
        kprintf("blk: BAR0 ist kein I/O-Bereich\n");
        return -1;
    }
    pci_enable(&pci, 1, 0, 1);
    pci_set_driver(&pci, "virtio-blk");
    dev.io = pci.bar[0] & ~3u;

    outb(dev.io + REG_STATUS, 0); /* Reset */
    outb(dev.io + REG_STATUS, STATUS_ACK);
    outb(dev.io + REG_STATUS, STATUS_ACK | STATUS_DRIVER);
    (void)inl(dev.io + REG_DEVICE_FEATURES);
    outl(dev.io + REG_GUEST_FEATURES, 0); /* keine optionalen Features */

    outw(dev.io + REG_QUEUE_SELECT, 0);
    dev.qsize = inw(dev.io + REG_QUEUE_SIZE);
    if (dev.qsize == 0 || (dev.qsize & (dev.qsize - 1))) {
        kprintf("blk: ungueltige Queue-Groesse %u\n", dev.qsize);
        return -1;
    }

    /* Legacy-Layout: Descriptors, Avail-Ring, dann (auf 4 KiB ausgerichtet) der Used-Ring */
    uint64_t desc_bytes  = 16ULL * dev.qsize;
    uint64_t avail_bytes = 6 + 2ULL * dev.qsize;
    uint64_t used_off    = (desc_bytes + avail_bytes + PAGE - 1) & ~(PAGE - 1);
    uint64_t used_bytes  = 6 + 8ULL * dev.qsize;
    uint64_t total       = (used_off + used_bytes + PAGE - 1) & ~(PAGE - 1);

    uint64_t ring = pmm_alloc_frames(total / PAGE);
    uint64_t req  = pmm_alloc_frame();
    uint64_t data = pmm_alloc_frames((BOUNCE_SECTORS * BLK_SECTOR_SIZE) / PAGE);
    if (!ring || !req || !data) {
        kprintf("blk: kein Speicher fuer die Queue\n");
        return -1;
    }
    memset((void *)ring, 0, total);
    memset((void *)req, 0, PAGE);

    dev.desc  = (volatile VringDesc *)ring;
    dev.avail = (volatile VringAvail *)(ring + desc_bytes);
    dev.used  = (volatile VringUsed *)(ring + used_off);
    dev.hdr    = (volatile BlkReqHeader *)req;
    dev.status = (volatile uint8_t *)(req + 64);
    dev.bounce = (uint8_t *)data;
    dev.avail->flags = AVAIL_NO_INTERRUPT; /* wir pollen */

    outl(dev.io + REG_QUEUE_PFN, (uint32_t)(ring / PAGE));
    outb(dev.io + REG_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_DRIVER_OK);

    dev.capacity = inl(dev.io + REG_CONFIG) | ((uint64_t)inl(dev.io + REG_CONFIG + 4) << 32);
    dev.ready = 1;
    return 0;
}

/* Eine Anfrage mit hoechstens BOUNCE_SECTORS Sektoren; Aufruf mit gehaltenem Lock. */
static int request(uint32_t type, uint64_t lba, uint32_t count, void *buf)
{
    uint64_t bytes = (uint64_t)count * BLK_SECTOR_SIZE;

    if (type == BLK_T_OUT)
        memcpy(dev.bounce, buf, bytes);

    dev.hdr->type = type;
    dev.hdr->reserved = 0;
    dev.hdr->sector = lba;
    *dev.status = 0xFF;

    dev.desc[0].addr = (uint64_t)dev.hdr;
    dev.desc[0].len = sizeof(BlkReqHeader);
    dev.desc[0].flags = DESC_NEXT;
    dev.desc[0].next = 1;

    dev.desc[1].addr = (uint64_t)dev.bounce;
    dev.desc[1].len = (uint32_t)bytes;
    dev.desc[1].flags = DESC_NEXT | (type == BLK_T_IN ? DESC_WRITE : 0);
    dev.desc[1].next = 2;

    dev.desc[2].addr = (uint64_t)dev.status;
    dev.desc[2].len = 1;
    dev.desc[2].flags = DESC_WRITE;
    dev.desc[2].next = 0;

    barrier();
    uint16_t slot = dev.avail->idx & (dev.qsize - 1);
    dev.avail->ring[slot] = 0; /* Kopf der Descriptor-Kette */
    barrier();
    dev.avail->idx = dev.avail->idx + 1;
    barrier();
    outw(dev.io + REG_QUEUE_NOTIFY, 0);

    /* Auf Abschluss pollen; zwischendurch anderen Threads Zeit geben */
    for (uint64_t spins = 0; dev.used->idx == dev.last_used; spins++) {
        if (spins > 50000000ULL) {
            kprintf("blk: Timeout bei LBA %lu\n", lba);
            return -1;
        }
        if (spins % 64 == 63)
            thread_yield();
        else
            __asm__ __volatile__("pause");
    }
    dev.last_used++;
    barrier();
    (void)inb(dev.io + REG_ISR); /* Interrupt-Status loeschen */

    if (*dev.status != 0) {
        kprintf("blk: Geraetefehler %u bei LBA %lu\n", *dev.status, lba);
        return -1;
    }
    if (type == BLK_T_IN)
        memcpy(buf, dev.bounce, bytes);
    return 0;
}

static int transfer(uint32_t type, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev.ready || lba + count > dev.capacity)
        return -1;
    mutex_lock(&lock);
    int r = 0;
    uint8_t *p = buf;
    while (count && r == 0) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        r = request(type, lba, n, p);
        lba += n;
        p += (uint64_t)n * BLK_SECTOR_SIZE;
        count -= n;
    }
    mutex_unlock(&lock);
    return r;
}

static int vread(BlkDev *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    return transfer(BLK_T_IN, lba, count, buf);
}

static int vwrite(BlkDev *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    return transfer(BLK_T_OUT, lba, count, (void *)buf);
}

void virtio_blk_probe(void)
{
    if (virtio_init() != 0)
        return;
    BlkDev bd;
    memset(&bd, 0, sizeof(bd));
    memcpy(bd.name, "vblk0", 6);
    memcpy(bd.model, "QEMU virtio-blk", 16);
    bd.sectors = dev.capacity;
    bd.read = vread;
    bd.write = vwrite;
    blk_register(&bd);
}
