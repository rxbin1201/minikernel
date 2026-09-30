#include "heap.h"
#include "cpu.h"
#include "paging.h"
#include "pmm.h"
#include "kprintf.h"
#include "string.h"

#define HEAP_BASE   0x0000010000000000ULL
#define HEAP_LIMIT  (1ULL << 30)
#define GROW_MIN    (64ULL * 1024)
#define PAGE        4096ULL
#define BLOCK_MAGIC 0xB10CB10Cu

/* Bloecke liegen lueckenlos hintereinander; der Nachbar ergibt sich aus size, der Vorgaenger aus prev. */
typedef struct Block {
    uint64_t      size;  /* Nutzdaten in Bytes, Vielfaches von 16 */
    struct Block *prev;
    uint32_t      magic;
    uint32_t      free;
} __attribute__((aligned(16))) Block; /* sizeof = 32, Nutzdaten damit 16-Byte ausgerichtet */

static Block   *first;
static Block   *last;
static uint64_t heap_size; /* gemappte Bytes ab HEAP_BASE */
static uint64_t used;

static Block *next_block(Block *b)
{
    Block *n = (Block *)((uint8_t *)(b + 1) + b->size);
    return (uint64_t)n < HEAP_BASE + heap_size ? n : 0;
}

void heap_init(void)
{
    paging_reserve_kernel_slot(HEAP_BASE); /* damit spaeter erzeugte Adressraeume den Heap sehen */
    first = last = 0;
    heap_size = used = 0;
}

/* Macht mindestens `req` Bytes Nutzdaten am Ende verfuegbar. 1 = ok, 0 = Speicher/Adressraum voll. */
static int heap_grow(uint64_t req)
{
    uint64_t extra;
    if (last && last->free)
        extra = req > last->size ? req - last->size : 0; /* freier letzter Block wird verlaengert */
    else
        extra = req + sizeof(Block);

    uint64_t grow = (extra + PAGE - 1) & ~(PAGE - 1);
    if (grow < GROW_MIN)
        grow = GROW_MIN;
    if (heap_size + grow > HEAP_LIMIT)
        return 0;

    uint64_t done = 0;
    for (; done < grow; done += PAGE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame)
            break;
        if (paging_map(HEAP_BASE + heap_size + done, frame, PAGE_WRITE | PAGE_NX) != 0) {
            pmm_free_frame(frame);
            break;
        }
    }
    if (done < grow) { /* Rollback */
        for (uint64_t a = 0; a < done; a += PAGE) {
            uint64_t virt = HEAP_BASE + heap_size + a, phys;
            if (paging_translate(virt, &phys, 0)) {
                paging_unmap(virt);
                pmm_free_frame(phys);
            }
        }
        return 0;
    }

    if (last && last->free) {
        last->size += grow;
    } else {
        Block *b = (Block *)(HEAP_BASE + heap_size);
        b->size  = grow - sizeof(Block);
        b->prev  = last;
        b->magic = BLOCK_MAGIC;
        b->free  = 1;
        if (!first)
            first = b;
        last = b;
    }
    heap_size += grow;
    return 1;
}

static void *kmalloc_locked(size_t size)
{
    if (size == 0 || size > HEAP_LIMIT)
        return 0;
    size = (size + 15) & ~(size_t)15;

    for (int attempt = 0; attempt < 2; attempt++) {
        for (Block *b = first; b; b = next_block(b)) {
            if (!b->free || b->size < size)
                continue;

            /* Rest abtrennen, wenn er noch fuer einen eigenen Block reicht */
            if (b->size >= size + sizeof(Block) + 16) {
                Block *nb   = next_block(b);
                Block *rest = (Block *)((uint8_t *)(b + 1) + size);
                rest->size  = b->size - size - sizeof(Block);
                rest->prev  = b;
                rest->magic = BLOCK_MAGIC;
                rest->free  = 1;
                if (nb)
                    nb->prev = rest;
                else
                    last = rest;
                b->size = size;
            }
            b->free = 0;
            used += b->size;
            return b + 1;
        }
        if (attempt == 0 && !heap_grow(size))
            break;
    }
    return 0;
}

static void kfree_locked(void *ptr)
{
    if (!ptr)
        return;
    uint64_t p = (uint64_t)ptr;
    if (p < HEAP_BASE + sizeof(Block) || p >= HEAP_BASE + heap_size || p % 16) {
        kprintf("heap: kfree mit ungueltigem Zeiger %#lx\n", p);
        return;
    }
    Block *b = (Block *)ptr - 1;
    if (b->magic != BLOCK_MAGIC) {
        kprintf("heap: kfree auf beschaedigten Block %#lx\n", p);
        return;
    }
    if (b->free) {
        kprintf("heap: double free bei %#lx\n", p);
        return;
    }

    used -= b->size;
    b->free = 1;

    /* Mit dem Nachfolger verschmelzen */
    Block *n = next_block(b);
    if (n && n->free) {
        b->size += sizeof(Block) + n->size;
        n->magic = 0;
        Block *nn = next_block(b);
        if (nn)
            nn->prev = b;
        else
            last = b;
    }

    /* Mit dem Vorgaenger verschmelzen */
    Block *pb = b->prev;
    if (pb && pb->free) {
        pb->size += sizeof(Block) + b->size;
        b->magic = 0;
        Block *nn = next_block(pb);
        if (nn)
            nn->prev = pb;
        else
            last = pb;
    }
}

/* Die oeffentlichen Funktionen schuetzen den Heap vor Reentranz aus Interrupts/Thread-Wechseln (1 CPU). */
void *kmalloc(size_t size)
{
    uint64_t f = irq_save();
    void *p = kmalloc_locked(size);
    irq_restore(f);
    return p;
}

void kfree(void *ptr)
{
    uint64_t f = irq_save();
    kfree_locked(ptr);
    irq_restore(f);
}

void *kcalloc(size_t count, size_t size)
{
    if (size && count > HEAP_LIMIT / size)
        return 0;
    void *p = kmalloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return 0;
    }
    Block *b = (Block *)ptr - 1;
    if (b->magic == BLOCK_MAGIC && ((size + 15) & ~(size_t)15) <= b->size)
        return ptr; /* passt schon, kein Verkleinern */

    void *n = kmalloc(size);
    if (!n)
        return 0; /* altes Stueck bleibt gueltig */
    memcpy(n, ptr, b->size < size ? b->size : size);
    kfree(ptr);
    return n;
}

uint64_t heap_used_bytes(void)  { return used; }
uint64_t heap_total_bytes(void) { return heap_size; }

int heap_check(void)
{
    uint64_t total = 0;
    Block *prev = 0;
    for (Block *b = first; b; b = next_block(b)) {
        if (b->magic != BLOCK_MAGIC || b->prev != prev || b->size % 16) {
            kprintf("heap: Block beschaedigt bei %#lx\n", (uint64_t)b);
            return 0;
        }
        if (prev && prev->free && b->free) {
            kprintf("heap: zwei freie Bloecke nebeneinander bei %#lx\n", (uint64_t)b);
            return 0;
        }
        total += sizeof(Block) + b->size;
        prev = b;
    }
    if (prev != last || total != heap_size) {
        kprintf("heap: Groesse/Ende inkonsistent\n");
        return 0;
    }
    return 1;
}
