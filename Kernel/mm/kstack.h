#ifndef KSTACK_H
#define KSTACK_H

#include <stdint.h>

#define KSTACK_PAGES 4 /* 16 KiB nutzbar, davor eine ungemappte Guard-Page */

/* Kernel-Stacks in einem eigenen virtuellen Bereich, jeweils mit ungemappter Guard-Page darunter.
 * Ein Ueberlauf loest so einen Page Fault (bzw. Double Fault) aus, statt still den Heap zu zerstoeren. */
void kstack_init(void);

/* Liefert das obere Ende (16-Byte-ausgerichtet, exklusiv) oder 0 bei Speichermangel. */
uint64_t kstack_alloc(void);
void     kstack_free(uint64_t top);

#endif
