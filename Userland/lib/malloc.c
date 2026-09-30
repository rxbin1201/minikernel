/* First-Fit-Allocator, siehe malloc.h */

#include "malloc.h"

static UBlock *u_first, *u_last;

void *u_malloc(u64 n)
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

void u_free(void *p)
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
