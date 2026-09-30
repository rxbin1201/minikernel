#ifndef PART_H
#define PART_H

#include <stdint.h>
#include "drivers/block/blk.h"

#define PART_MAX 16

typedef struct {
    uint64_t start;    /* erster Sektor */
    uint64_t sectors;
    int      gpt;      /* 1 = GPT, 0 = MBR */
    uint8_t  mbr_type; /* nur MBR */
    int      index;    /* 1-basierte Nummer in der Tabelle */
} PartInfo;

/* Liest MBR oder GPT (bei GPT-Schutz-MBR die GPT). Liefert die Anzahl der Partitionen (ohne erweiterte MBR-Partitionen
 * und leere Eintraege). */
int part_scan(BlkDev *d, PartInfo *out, int max);

#endif
