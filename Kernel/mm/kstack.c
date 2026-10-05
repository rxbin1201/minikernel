#include "mm/kstack.h"
#include "arch/x86_64/cpu.h"
#include "mm/paging.h"
#include "mm/pmm.h"

#define KSTACK_BASE 0x0000018000000000ULL /* 1,5 TiB: eigener Top-Level-Slot */
#define PAGE        4096ULL
#define SLOT_SIZE   ((KSTACK_PAGES + 1) * PAGE) /* Guard + Stack */
#define MAX_SLOTS   65536 /* Kernel-Threads gleichzeitig (1,3 GiB Adressraum, belegt wird nur, was laeuft) */

/* Belegte Plaetze als Bitmap (frueher eine Freiliste mit 128 Eintraegen: wurden mehr Stacks auf einmal frei, gingen
 * die uebrigen Plaetze fuer immer verloren). Unter dem BKL (thread_create, reap_dead). */
static uint64_t used_slots[MAX_SLOTS / 64];
static uint32_t slot_hint; /* Wort, ab dem gesucht wird */

static int slot_take(uint32_t *slot)
{
    for (uint32_t n = 0; n < MAX_SLOTS / 64; n++) {
        uint32_t w = (slot_hint + n) % (MAX_SLOTS / 64);
        if (used_slots[w] != ~0ULL) {
            uint32_t bit = (uint32_t)__builtin_ctzll(~used_slots[w]);
            used_slots[w] |= 1ULL << bit;
            slot_hint = w;
            *slot = w * 64 + bit;
            return 1;
        }
    }
    return 0;
}

static void slot_give(uint32_t slot)
{
    used_slots[slot / 64] &= ~(1ULL << (slot % 64));
    if (slot / 64 < slot_hint)
        slot_hint = slot / 64;
}

void kstack_init(void)
{
    paging_reserve_kernel_slot(KSTACK_BASE);
}

uint64_t kstack_alloc(void)
{
    uint64_t f = irq_save();

    uint32_t slot;
    if (!slot_take(&slot)) {
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
            slot_give(slot);
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
    slot_give((uint32_t)((base - PAGE - KSTACK_BASE) / SLOT_SIZE));
    irq_restore(f);
}
