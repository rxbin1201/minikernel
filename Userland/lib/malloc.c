/* First-Fit-Allocator, siehe malloc.h */

#include "malloc.h"
#include "thread.h"

static UBlock *u_first, *u_last;
static Mutex   u_lock = MUTEX_INIT;
volatile int   u_threaded; /* thread_create setzt es: ab dann sperren (vorher kostet es nichts) */

static void *malloc_locked(u64 n);
static void  free_locked(void *p);

void *u_malloc(u64 n)
{
    if (!u_threaded)
        return malloc_locked(n);
    mutex_lock(&u_lock);
    void *p = malloc_locked(n);
    mutex_unlock(&u_lock);
    return p;
}

void u_free(void *p)
{
    if (!u_threaded) {
        free_locked(p);
        return;
    }
    mutex_lock(&u_lock);
    free_locked(p);
    mutex_unlock(&u_lock);
}

static void *malloc_locked(u64 n)
{
    if (!n)
        return 0;
    n = (n + 15) & ~15ULL;

    UBlock *b = u_first;
    while (b) {
        if (b->free && b->size >= n) {
            if (b->size >= n + sizeof(UBlock) + 16) { /* Rest abtrennen */
                UBlock *rest = (UBlock *)((char *)(b + 1) + n);
                rest->size = b->size - n - sizeof(UBlock);
                rest->free = 1;
                rest->prev = b;
                UBlock *next = (UBlock *)((char *)(rest + 1) + rest->size);
                if (u_last == b)
                    u_last = rest;
                else
                    next->prev = rest;
                b->size = n;
            }
            b->free = 0;
            return b + 1;
        }
        UBlock *next = (UBlock *)((char *)(b + 1) + b->size);
        b = (b == u_last) ? 0 : next;
    }

    /* Nichts frei: Heap per brk verlaengern */
    char *end = sys_brk(0);
    u64 total = sizeof(UBlock) + n;
    if (sys_brk(end + total) != end + total)
        return 0;
    UBlock *nb = (UBlock *)end;
    nb->size = n;
    nb->free = 0;
    nb->prev = u_last;
    u_last = nb;
    if (!u_first)
        u_first = nb;
    return nb + 1;
}

static void free_locked(void *p)
{
    if (!p)
        return;
    UBlock *b = (UBlock *)p - 1;
    b->free = 1;

    if (b != u_last) { /* mit Nachfolger verschmelzen */
        UBlock *next = (UBlock *)((char *)(b + 1) + b->size);
        if (next->free) {
            b->size += sizeof(UBlock) + next->size;
            if (next == u_last)
                u_last = b;
            else
                ((UBlock *)((char *)(b + 1) + b->size))->prev = b;
        }
    }
    if (b->prev && b->prev->free) { /* mit Vorgaenger verschmelzen */
        UBlock *pb = b->prev;
        pb->size += sizeof(UBlock) + b->size;
        if (b == u_last)
            u_last = pb;
        else
            ((UBlock *)((char *)(pb + 1) + pb->size))->prev = pb;
    }
}
