#include "mm/pmm.h"
#include "arch/x86_64/cpu.h"
#include "lib/kprintf.h"
#include "lib/string.h"

#define EFI_CONVENTIONAL_MEMORY 7
#define LOW_RESERVED            (1ULL << 20)   /* alles unter 1 MiB bleibt reserviert */
#define PMM_LIMIT               (32ULL << 30)

/* Bit = 1: belegt/reserviert, Bit = 0: frei */
static uint64_t *bitmap;
static uint64_t  bitmap_words;
static uint64_t  frame_count;   /* Frames, die die Bitmap abdeckt */
static uint64_t  managed;       /* tatsaechlich nutzbare Frames */
static uint64_t  free_count;
static uint64_t  search_hint;   /* Wort-Index, ab dem die naechste Suche beginnt */
static uint64_t  low_page;      /* freie Seite unter 1 MiB (Startcode weiterer CPUs), 0 = keine */

static inline int  frame_used(uint64_t f) { return (bitmap[f / 64] >> (f % 64)) & 1; }
static inline void frame_set(uint64_t f)  { bitmap[f / 64] |=  (1ULL << (f % 64)); }
static inline void frame_clear(uint64_t f){ bitmap[f / 64] &= ~(1ULL << (f % 64)); }

/* Liefert den auf [LOW_RESERVED, PMM_LIMIT) begrenzten nutzbaren Bereich eines Descriptors. */
static int usable_range(const BootMemoryDescriptor *d, uint64_t *start, uint64_t *end)
{
    if (d->type != EFI_CONVENTIONAL_MEMORY)
        return 0;
    uint64_t s = d->physical_start;
    uint64_t e = s + d->page_count * PMM_FRAME_SIZE;
    if (s < LOW_RESERVED)
        s = LOW_RESERVED;
    if (e > PMM_LIMIT)
        e = PMM_LIMIT;
    if (e <= s)
        return 0;
    *start = s;
    *end = e;
    return 1;
}

#define FOR_EACH_DESCRIPTOR(info, d)                                                              \
    for (uint64_t off_ = 0;                                                                       \
         off_ + sizeof(BootMemoryDescriptor) <= (info)->memory_map_size &&                        \
         ((d) = (const BootMemoryDescriptor *)((const uint8_t *)(info)->memory_map + off_), 1);   \
         off_ += (info)->descriptor_size)

void pmm_init(const BootInfo *info)
{
    const BootMemoryDescriptor *d;
    uint64_t s, e, max_end = 0;

    /* 0. Eine freie Seite unter 1 MiB merken (nicht Seite 0): dort startet smp.c die weiteren CPUs im Real Mode */
    FOR_EACH_DESCRIPTOR(info, d) {
        uint64_t ls = d->physical_start < 0x1000 ? 0x1000 : d->physical_start;
        uint64_t le = d->physical_start + d->page_count * PMM_FRAME_SIZE;
        if (d->type == EFI_CONVENTIONAL_MEMORY && !low_page && ls < le && ls + PMM_FRAME_SIZE <= 0x9F000)
            low_page = ls;
    }

    /* 1. Groesse der Bitmap bestimmen */
    FOR_EACH_DESCRIPTOR(info, d) {
        if (usable_range(d, &s, &e) && e > max_end)
            max_end = e;
    }
    if (!max_end) {
        kprintf("pmm: kein nutzbarer Speicher gefunden\n");
        return;
    }

    frame_count  = max_end / PMM_FRAME_SIZE;
    bitmap_words = (frame_count + 63) / 64;
    uint64_t bitmap_bytes  = bitmap_words * 8;
    uint64_t bitmap_frames = (bitmap_bytes + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    /* 2. Bitmap in den ersten Bereich legen, der gross genug ist */
    uint64_t bitmap_addr = 0;
    FOR_EACH_DESCRIPTOR(info, d) {
        if (usable_range(d, &s, &e) && e - s >= bitmap_frames * PMM_FRAME_SIZE) {
            bitmap_addr = s;
            break;
        }
    }
    if (!bitmap_addr) {
        kprintf("pmm: kein Platz fuer die Bitmap\n");
        return;
    }
    bitmap = (uint64_t *)bitmap_addr;

    /* 3. Erst alles als belegt markieren (inkl. Padding-Bits am Ende), dann freie Bereiche freigeben */
    memset(bitmap, 0xFF, bitmap_bytes);
    FOR_EACH_DESCRIPTOR(info, d) {
        if (!usable_range(d, &s, &e))
            continue;
        for (uint64_t f = s / PMM_FRAME_SIZE; f < e / PMM_FRAME_SIZE; f++) {
            if (frame_used(f)) {
                frame_clear(f);
                managed++;
                free_count++;
            }
        }
    }

    /* 4. Die Bitmap selbst reservieren */
    for (uint64_t f = bitmap_addr / PMM_FRAME_SIZE; f < bitmap_addr / PMM_FRAME_SIZE + bitmap_frames; f++) {
        if (!frame_used(f)) {
            frame_set(f);
            free_count--;
            managed--;
        }
    }

    kprintf("pmm: Bitmap @ %#lx (%lu Frames), %lu Frames frei (%lu MiB)\n",
            bitmap_addr, bitmap_frames, free_count, free_count * PMM_FRAME_SIZE / (1024 * 1024));
}

static uint64_t alloc_frame_impl(void)
{
    for (uint64_t n = 0; n < bitmap_words; n++) {
        uint64_t w = (search_hint + n) % bitmap_words;
        if (bitmap[w] == ~0ULL)
            continue;
        uint64_t bit = __builtin_ctzll(~bitmap[w]);
        uint64_t f = w * 64 + bit;
        frame_set(f);
        free_count--;
        search_hint = w;
        return f * PMM_FRAME_SIZE;
    }
    return 0;
}

static uint64_t alloc_frames_impl(uint64_t count)
{
    if (count == 0)
        return 0;
    if (count == 1)
        return pmm_alloc_frame();

    uint64_t run = 0;
    for (uint64_t f = 0; f < frame_count; f++) {
        if (frame_used(f)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            uint64_t first = f + 1 - count;
            for (uint64_t i = first; i <= f; i++)
                frame_set(i);
            free_count -= count;
            return first * PMM_FRAME_SIZE;
        }
    }
    return 0;
}

static void free_frames_impl(uint64_t addr, uint64_t count)
{
    uint64_t first = addr / PMM_FRAME_SIZE;
    if (addr % PMM_FRAME_SIZE || first + count > frame_count) {
        kprintf("pmm: ungueltiges free bei %#lx\n", addr);
        return;
    }
    for (uint64_t f = first; f < first + count; f++) {
        if (!frame_used(f)) {
            kprintf("pmm: double free bei %#lx\n", f * PMM_FRAME_SIZE);
            continue;
        }
        frame_clear(f);
        free_count++;
        if (f / 64 < search_hint)
            search_hint = f / 64;
    }
}

/* Oeffentliche Funktionen: gegen Reentranz aus Interrupts/Thread-Wechseln geschuetzt (1 CPU). */
uint64_t pmm_alloc_frame(void)
{
    uint64_t f = irq_save();
    uint64_t r = alloc_frame_impl();
    irq_restore(f);
    return r;
}

uint64_t pmm_alloc_frames(uint64_t count)
{
    uint64_t f = irq_save();
    uint64_t r = count == 1 ? alloc_frame_impl() : alloc_frames_impl(count);
    irq_restore(f);
    return r;
}

void pmm_free_frame(uint64_t addr)
{
    pmm_free_frames(addr, 1);
}

void pmm_free_frames(uint64_t addr, uint64_t count)
{
    uint64_t f = irq_save();
    free_frames_impl(addr, count);
    irq_restore(f);
}

uint64_t pmm_low_page(void)          { return low_page; }
uint64_t pmm_total_frames(void)      { return managed; }
uint64_t pmm_free_frame_count(void)  { return free_count; }
