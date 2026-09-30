#ifndef MALLOC_H
#define MALLOC_H

#include "user.h"

/* Kleiner First-Fit-Allocator auf Basis von brk (Bloecke liegen lueckenlos hintereinander, Freigabe
 * verschmilzt mit freien Nachbarn). Nicht threadsicher; ein Prozess hat nur einen Thread. */

typedef struct UBlock {
    u64            size;  /* Nutzdaten, Vielfaches von 16 */
    struct UBlock *prev;
    u64            free;
    u64            pad;   /* Header = 32 Byte, Nutzdaten damit 16-Byte ausgerichtet */
} UBlock;


void *u_malloc(u64 n);
void u_free(void *p);

#endif
