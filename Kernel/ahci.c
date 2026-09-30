#include "blk.h"
#include "apic.h"
#include "heap.h"
#include "kprintf.h"
#include "paging.h"
#include "pci.h"
#include "sched.h"
#include "string.h"

/* AHCI 1.3 (SATA), DMA ueber Kommandoliste, Polling. Pro Port mit ATA-Laufwerk wird ein BlkDev registriert. */

/* HBA-Register */
#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_CAP2   0x24
#define HBA_BOHC   0x28
#define GHC_AE     (1u << 31)
#define GHC_IE     (1u << 1)
#define CAP_S64A   (1u << 31)
#define CAP2_BOH   (1u << 0)
#define BOHC_BOS   (1u << 0) /* BIOS Owned Semaphore */
#define BOHC_OOS   (1u << 1) /* OS Owned Semaphore */

/* Port-Register (Basis 0x100 + 0x80 * n) */
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SERR  0x30
#define P_SACT  0x34
#define P_CI    0x38
#define CMD_ST   (1u << 0)
#define CMD_SUD  (1u << 1)
#define CMD_POD  (1u << 2)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define TFD_ERR  (1u << 0)
#define TFD_DRQ  (1u << 3)
#define TFD_BSY  (1u << 7)
#define IS_TFES  (1u << 30) /* Task File Error */
#define SIG_ATA  0x00000101

/* ATA-Kommandos */
#define ATA_IDENTIFY      0xEC
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_EXT     0xEA

#define BOUNCE_SECTORS 64

typedef struct {
    uint32_t flags;    /* CFL (Bit 0-4), Write (Bit 6), PRDTL (Bit 16-31) */
    uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t reserved[4];
} __attribute__((packed)) CmdHeader;

typedef struct {
    uint32_t dba, dbau, reserved, dbc; /* dbc: Bytes - 1 (Bit 0-21) */
} __attribute__((packed)) PrdtEntry;

typedef struct {
    uint8_t   cfis[64];
    uint8_t   acmd[16];
    uint8_t   reserved[48];
    PrdtEntry prdt[1];
} __attribute__((packed)) CmdTable;

typedef struct {
    volatile uint8_t *regs;       /* Port-Register */
    volatile CmdHeader *list;     /* Kommandoliste (32 Eintraege) */
    volatile CmdTable  *table;    /* Tabelle fuer Slot 0 */
    uint8_t *bounce;              /* DMA-Datenpuffer */
    uint64_t sectors;
    Mutex    lock;
} Port;

static inline uint32_t rd(volatile uint8_t *base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline void wr(volatile uint8_t *base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }

static int stop_port(volatile uint8_t *p)
{
    uint32_t cmd = rd(p, P_CMD);
    if (cmd & (CMD_ST | CMD_CR | CMD_FRE | CMD_FR)) {
        wr(p, P_CMD, cmd & ~CMD_ST);
        if (!WAIT_UNTIL(!(rd(p, P_CMD) & CMD_CR), 500))
            return -1;
        cmd = rd(p, P_CMD);
        wr(p, P_CMD, cmd & ~CMD_FRE);
        if (!WAIT_UNTIL(!(rd(p, P_CMD) & CMD_FR), 500))
            return -1;
    }
    return 0;
}

static int start_port(volatile uint8_t *p)
{
    if (!WAIT_UNTIL(!(rd(p, P_CMD) & CMD_CR), 500))
        return -1;
    wr(p, P_CMD, rd(p, P_CMD) | CMD_FRE);
    wr(p, P_CMD, rd(p, P_CMD) | CMD_ST);
    return 0;
}

/* Ein ATA-Kommando ueber Slot 0. bytes = 0: kein Datentransfer. Aufruf mit gehaltenem Port-Lock. */
static int run_command(Port *port, uint8_t ata, uint64_t lba, uint32_t count, int write, uint32_t bytes)
{
    volatile uint8_t *p = port->regs;

    memset((void *)port->table, 0, sizeof(CmdTable));
    uint8_t *f = (uint8_t *)port->table->cfis;
    f[0] = 0x27;  /* Register FIS Host->Device */
    f[1] = 0x80;  /* Command-Bit */
    f[2] = ata;
    f[4] = lba & 0xFF;
    f[5] = (lba >> 8) & 0xFF;
    f[6] = (lba >> 16) & 0xFF;
    f[7] = 0x40;  /* LBA-Modus */
    f[8] = (lba >> 24) & 0xFF;
    f[9] = (lba >> 32) & 0xFF;
    f[10] = (lba >> 40) & 0xFF;
    f[12] = count & 0xFF;
    f[13] = (count >> 8) & 0xFF;

    if (bytes) {
        port->table->prdt[0].dba = (uint32_t)(uint64_t)port->bounce;
        port->table->prdt[0].dbau = (uint32_t)((uint64_t)port->bounce >> 32);
        port->table->prdt[0].dbc = bytes - 1;
    }

    port->list[0].flags = 5 | (write ? (1u << 6) : 0) | ((bytes ? 1u : 0u) << 16);
    port->list[0].prdbc = 0;
    port->list[0].ctba = (uint32_t)(uint64_t)port->table;
    port->list[0].ctbau = (uint32_t)((uint64_t)port->table >> 32);

    if (!WAIT_UNTIL(!(rd(p, P_TFD) & (TFD_BSY | TFD_DRQ)), 1000)) {
        kprintf("ahci: Port belegt\n");
        return -1;
    }
    wr(p, P_IS, 0xFFFFFFFF);
    __asm__ __volatile__("mfence" : : : "memory");
    wr(p, P_CI, 1);

    /* Auf Abschluss warten (Platten koennen nach dem Aufwecken mehrere Sekunden brauchen) */
    int done = WAIT_UNTIL(!(rd(p, P_CI) & 1) || (rd(p, P_IS) & IS_TFES), 10000);
    if (!done) {
        kprintf("ahci: Timeout bei Kommando %#x LBA %lu\n", ata, lba);
        return -1;
    }
    if ((rd(p, P_IS) & IS_TFES) || (rd(p, P_TFD) & TFD_ERR)) {
        kprintf("ahci: Fehler bei Kommando %#x LBA %lu (TFD %#x)\n", ata, lba, rd(p, P_TFD));
        /* Port wieder in einen brauchbaren Zustand bringen */
        stop_port(p);
        wr(p, P_SERR, 0xFFFFFFFF);
        start_port(p);
        return -1;
    }
    return 0;
}

static int transfer(Port *port, int write, uint64_t lba, uint32_t count, void *buf)
{
    mutex_lock(&port->lock);
    int r = 0;
    uint8_t *b = buf;
    while (count && r == 0) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        if (write)
            memcpy(port->bounce, b, (uint64_t)n * BLK_SECTOR_SIZE);
        r = run_command(port, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, n, write, n * BLK_SECTOR_SIZE);
        if (r == 0 && !write)
            memcpy(b, port->bounce, (uint64_t)n * BLK_SECTOR_SIZE);
        lba += n;
        b += (uint64_t)n * BLK_SECTOR_SIZE;
        count -= n;
    }
    mutex_unlock(&port->lock);
    return r;
}

static int ahci_read(BlkDev *d, uint64_t lba, uint32_t count, void *buf)
{
    return transfer(d->priv, 0, lba, count, buf);
}

static int ahci_write(BlkDev *d, uint64_t lba, uint32_t count, const void *buf)
{
    return transfer(d->priv, 1, lba, count, (void *)buf);
}

static int ahci_flush(BlkDev *d)
{
    Port *port = d->priv;
    mutex_lock(&port->lock);
    int r = run_command(port, ATA_FLUSH_EXT, 0, 0, 0, 0);
    mutex_unlock(&port->lock);
    return r;
}

/* IDENTIFY DEVICE auswerten. 0 = brauchbares Laufwerk. */
static int identify(Port *port, BlkDev *bd)
{
    if (run_command(port, ATA_IDENTIFY, 0, 0, 0, 512) != 0)
        return -1;
    const uint16_t *w = (const uint16_t *)port->bounce;

    uint32_t sector_bytes = 512;
    if ((w[106] & 0xC000) == 0x4000 && (w[106] & (1u << 12)))
        sector_bytes = 2 * (w[117] | ((uint32_t)w[118] << 16)); /* logische Sektorgroesse in Woertern */
    if (sector_bytes != 512) {
        kprintf("ahci: logische Sektorgroesse %u wird nicht unterstuetzt\n", sector_bytes);
        return -1;
    }

    if (w[83] & (1u << 10)) /* LBA48 */
        bd->sectors = (uint64_t)w[100] | ((uint64_t)w[101] << 16) | ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
    else
        bd->sectors = w[60] | ((uint32_t)w[61] << 16);
    if (bd->sectors == 0)
        return -1;

    for (int i = 0; i < 20; i++) { /* Modell: Woerter 27-46, Bytes vertauscht */
        bd->model[2 * i] = (char)(w[27 + i] >> 8);
        bd->model[2 * i + 1] = (char)(w[27 + i] & 0xFF);
    }
    bd->model[40] = 0;
    blk_trim_model(bd->model);
    return 0;
}

static void probe_controller(const PciDevice *pci, int index, int *sata_count)
{
    uint64_t abar = pci_bar_mem(pci, 5);
    if (!abar) {
        kprintf("ahci: Controller %02x:%02x.%u ohne ABAR (BAR5)\n", pci->bus, pci->dev, pci->fn);
        return;
    }
    pci_enable(pci, 0, 1, 1);
    pci_set_driver(pci, "ahci");
    if (paging_map_mmio(abar, 0x2000) != 0) {
        kprintf("ahci: ABAR nicht mappbar\n");
        return;
    }
    volatile uint8_t *hba = (volatile uint8_t *)abar;

    /* Zugriff vom BIOS/UEFI uebernehmen (falls der Controller das anbietet) */
    if (rd(hba, HBA_CAP2) & CAP2_BOH) {
        wr(hba, HBA_BOHC, rd(hba, HBA_BOHC) | BOHC_OOS);
        WAIT_UNTIL(!(rd(hba, HBA_BOHC) & BOHC_BOS), 1000);
    }

    wr(hba, HBA_GHC, rd(hba, HBA_GHC) | GHC_AE);   /* AHCI-Modus */
    wr(hba, HBA_GHC, rd(hba, HBA_GHC) & ~GHC_IE);  /* wir pollen */
    uint32_t cap = rd(hba, HBA_CAP);
    uint32_t implemented = rd(hba, HBA_PI);
    kprintf("ahci: Controller %02x:%02x.%u @ %#lx, %u Ports, %s\n", pci->bus, pci->dev, pci->fn, abar,
            (cap & 31) + 1, (cap & CAP_S64A) ? "64-Bit-DMA" : "nur 32-Bit-DMA");

    for (int n = 0; n < 32; n++) {
        if (!(implemented & (1u << n)))
            continue;
        volatile uint8_t *p = hba + 0x100 + 0x80 * n;

        /* Laufende Kommandomaschine (vom Firmware-Treiber) stoppen, bevor wir eigene Puffer eintragen */
        if (stop_port(p) != 0) {
            kprintf("ahci: Port %d laesst sich nicht stoppen\n", n);
            continue;
        }

        uint32_t ssts = rd(p, P_SSTS);
        if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1 || rd(p, P_SIG) != SIG_ATA)
            continue; /* kein aktives SATA-Laufwerk */

        Port *port = kcalloc(1, sizeof(*port));
        uint8_t *mem = blk_dma_alloc(4096);
        uint8_t *bounce = blk_dma_alloc(BOUNCE_SECTORS * BLK_SECTOR_SIZE);
        if (!port || !mem || !bounce) {
            kprintf("ahci: kein Speicher fuer Port %d\n", n);
            continue;
        }
        if (!(cap & CAP_S64A) && ((uint64_t)mem >> 32 || (uint64_t)bounce >> 32)) {
            kprintf("ahci: DMA-Speicher oberhalb 4 GiB, Controller kann nur 32 Bit\n");
            continue;
        }

        port->regs = p;
        port->list = (volatile CmdHeader *)mem;                 /* 1 KiB */
        port->table = (volatile CmdTable *)(mem + 2048);        /* 128-Byte-ausgerichtet */
        port->bounce = bounce;
        port->lock = (Mutex)MUTEX_INIT;
        uint64_t fis = (uint64_t)mem + 1024;                    /* 256 Byte, 256-ausgerichtet */

        wr(p, P_CLB, (uint32_t)(uint64_t)mem);
        wr(p, P_CLBU, (uint32_t)((uint64_t)mem >> 32));
        wr(p, P_FB, (uint32_t)fis);
        wr(p, P_FBU, (uint32_t)(fis >> 32));
        wr(p, P_SERR, 0xFFFFFFFF);
        wr(p, P_IS, 0xFFFFFFFF);
        wr(p, P_IE, 0);
        wr(p, P_CMD, rd(p, P_CMD) | CMD_POD | CMD_SUD); /* Strom/Spin-up, falls unterstuetzt */
        if (start_port(p) != 0) {
            kprintf("ahci: Port %d startet nicht\n", n);
            continue;
        }

        BlkDev bd;
        memset(&bd, 0, sizeof(bd));
        if (identify(port, &bd) != 0) {
            kprintf("ahci: Port %d: IDENTIFY fehlgeschlagen\n", n);
            continue;
        }
        (void)index;
        bd.name[0] = 's'; bd.name[1] = 'a'; bd.name[2] = 't'; bd.name[3] = 'a';
        bd.name[4] = (char)('0' + (*sata_count)++);
        bd.read = ahci_read;
        bd.write = ahci_write;
        bd.flush = ahci_flush;
        bd.priv = port;
        port->sectors = bd.sectors;
        blk_register(&bd);
    }
}

void ahci_probe(void)
{
    int sata = 0;
    PciDevice pci;
    for (unsigned i = 0; pci_find_class(0x01, 0x06, 0x01, i, &pci) == 0; i++)
        probe_controller(&pci, (int)i, &sata);
}
