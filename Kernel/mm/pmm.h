#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "boot_info.h"

#define PMM_FRAME_SIZE 4096UL

/* Bitmap-Allocator fuer physische 4-KiB-Frames. Verwaltet nur EfiConventionalMemory
 * oberhalb von 1 MiB und unterhalb von 32 GiB (so weit reicht das Mapping in paging.c). */
void pmm_init(const BootInfo *info);

/* Liefern die physische Adresse (identity-mapped, also direkt nutzbar) oder 0 bei Fehler.
 * Frame 0 wird nie vergeben, 0 ist also ein eindeutiger Fehlerwert. */
uint64_t pmm_alloc_frame(void);
uint64_t pmm_alloc_frames(uint64_t count); /* physisch zusammenhaengend */

void pmm_free_frame(uint64_t addr);
void pmm_free_frames(uint64_t addr, uint64_t count);

uint64_t pmm_total_frames(void); /* vom PMM verwaltete Frames (ohne reservierte) */
uint64_t pmm_free_frame_count(void);

/* Eine freie RAM-Seite unter 1 MiB (vom PMM nie vergeben), z.B. fuer den Real-Mode-Start weiterer CPUs; 0 = keine */
uint64_t pmm_low_page(void);

#endif
