#include "mm/kstack.h"
#include "arch/x86_64/cpu.h"
#include "mm/paging.h"
#include "mm/pmm.h"

#define KSTACK_BASE 0x0000018000000000ULL /* 1,5 TiB: eigener Top-Level-Slot */
#define PAGE        4096ULL
#define SLOT_SIZE   ((KSTACK_PAGES + 1) * PAGE) /* Guard + Stack */
#define MAX_SLOTS   4096
#define FREE_LIST   128

static uint32_t next_slot;
static uint32_t free_slots[FREE_LIST];
static unsigned free_count;

void kstack_init(void)
{
    paging_reserve_kernel_slot(KSTACK_BASE);
}

uint64_t kstack_alloc(void)
{
    uint64_t f = irq_save();

    uint32_t slot;
    if (free_count)
        slot = free_slots[--free_count];
    else if (next_slot < MAX_SLOTS)
        slot = next_slot++;
    else {
        irq_restore(f);
        return 0;
    }

    uint64_t base = KSTACK_BASE + slot * SLOT_SIZE + PAGE; /* Seite 0 des Slots bleibt die Guard-Page */
    for (unsigned i = 0; i < KSTACK_PAGES; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame || paging_map(base + i * PAGE, frame, PAGE_WRITE | PAGE_NX) != 0) {
            if (frame)
                pmm_free_frame(frame);
            for (unsigned j = 0; j < i; j++) { /* Rollback */
                uint64_t phys;
                if (paging_translate(base + j * PAGE, &phys, 0)) {
                    paging_unmap(base + j * PAGE);
                    pmm_free_frame(phys);
                }
            }
            if (free_count < FREE_LIST)
                free_slots[free_count++] = slot;
            irq_restore(f);
            return 0;
        }
    }
    irq_restore(f);
    return base + KSTACK_PAGES * PAGE;
}

void kstack_free(uint64_t top)
{
    if (!top)
        return;
    uint64_t f = irq_save();
    uint64_t base = top - KSTACK_PAGES * PAGE;
    for (unsigned i = 0; i < KSTACK_PAGES; i++) {
        uint64_t phys;
        if (paging_translate(base + i * PAGE, &phys, 0)) {
            paging_unmap(base + i * PAGE);
            pmm_free_frame(phys);
        }
    }
    uint32_t slot = (uint32_t)((base - PAGE - KSTACK_BASE) / SLOT_SIZE);
    if (free_count < FREE_LIST)
        free_slots[free_count++] = slot;
    irq_restore(f);
}
