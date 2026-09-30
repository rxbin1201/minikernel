#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

/* Kernel-Heap in einem eigenen virtuellen Bereich (1 TiB, max. 1 GiB gross). Waechst bei Bedarf,
 * indem er PMM-Frames per paging_map einblendet. Braucht pmm_init und paging_harden. */
void heap_init(void);

/* Alle Allokationen sind 16-Byte ausgerichtet. Bei Fehler wird NULL geliefert. */
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

uint64_t heap_used_bytes(void);  /* Summe der belegten Nutzdaten */
uint64_t heap_total_bytes(void); /* aktuell gemappte Heap-Groesse */

/* Prueft die Block-Struktur. 1 = konsistent, 0 = beschaedigt (mit Meldung auf Serial). */
int heap_check(void);

#endif
