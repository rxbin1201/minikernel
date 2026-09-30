#ifndef BLK_H
#define BLK_H

#include <stdint.h>

#define BLK_SECTOR_SIZE 512
#define BLK_MAX_DEVICES 8
#define BLK_MAX_SECTORS_PER_REQUEST 64 /* Treiber teilen groessere Zugriffe selbst auf */

/* Blockgeraete-Schicht. Treiber: virtio-blk (nur QEMU), AHCI (SATA) und NVMe. Alle Geraete haben 512-Byte-Sektoren;
 * Laufwerke mit anderer logischer Sektorgroesse werden nicht registriert. */

typedef struct BlkDev BlkDev;

struct BlkDev {
    char     name[16];    /* z.B. "nvme0n1", "sata0", "vblk0" */
    char     model[41];   /* Modellbezeichnung soweit bekannt */
    uint64_t sectors;
    int  (*read)(BlkDev *d, uint64_t lba, uint32_t count, void *buf);        /* 0 = ok */
    int  (*write)(BlkDev *d, uint64_t lba, uint32_t count, const void *buf);
    int  (*flush)(BlkDev *d);                                                 /* darf NULL sein */
    void *priv;
};

/* Sucht alle Geraete (PCI muss gescannt sein, sched/heap/pmm laufen). Liefert die Anzahl. */
int blk_init(void);

int     blk_count(void);
BlkDev *blk_get(int index);
unsigned blk_generation(void); /* wird bei jedem neu registrierten Geraet (z.B. USB-Stick) groesser */

/* Zugriffe mit Bereichspruefung; 0 = ok, -1 = Fehler */
int blk_read(BlkDev *d, uint64_t lba, uint32_t count, void *buf);
int blk_write(BlkDev *d, uint64_t lba, uint32_t count, const void *buf);
int blk_flush(BlkDev *d);
void blk_flush_all(void); /* alle Geraete, z.B. vor dem Ausschalten */

/* --- fuer Treiber --- */
int  blk_register(BlkDev *d);                       /* Kopie des Deskriptors wird abgelegt */
void *blk_dma_alloc(uint64_t bytes);                /* zusammenhaengend, genullt, 4 KiB-ausgerichtet; NULL bei Fehler */
void blk_trim_model(char *s);                       /* Leerzeichen am Ende entfernen */

void virtio_blk_probe(void);
void ahci_probe(void);
void nvme_probe(void);

#endif
