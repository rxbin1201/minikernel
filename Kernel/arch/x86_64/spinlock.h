#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "arch/x86_64/cpu.h"

/* Spinlock fuer kurze Abschnitte, die auch ohne Big Kernel Lock (smp.h) sicher sein muessen: Scheduler, Heap, PMM,
 * Kernel-Seitentabellen. Er sperrt die Interrupts dieser CPU, solange er gehalten wird (sonst koennte ein
 * Interrupt-Handler auf derselben CPU auf ihn warten). Nicht rekursiv; wer ihn doppelt nimmt, haelt das System mit
 * Meldung an, statt still zu haengen.
 *
 * Reihenfolge, wenn mehrere noetig sind (immer von links nach rechts nehmen):
 *   BKL -> sched_lock -> heap_lock -> paging_lock -> pmm_lock */

typedef struct {
    volatile int      locked;
    volatile int      owner; /* Index der haltenden CPU + 1, 0 = frei */
    const char       *name;
} Spinlock;

#define SPINLOCK_INIT(n) {0, 0, n}

void spin_deadlock(Spinlock *l) __attribute__((noreturn)); /* smp.c */
int  spin_cpu_tag(void);                                   /* smp.c: Index dieser CPU + 1 */

static inline uint64_t spin_lock(Spinlock *l)
{
    uint64_t flags = irq_save();
    int me = spin_cpu_tag();
    if (l->owner == me)
        spin_deadlock(l);
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (l->locked)
            __asm__ __volatile__("pause");
    l->owner = me;
    return flags;
}

static inline void spin_unlock(Spinlock *l, uint64_t flags)
{
    l->owner = 0;
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}

static inline int spin_held(const Spinlock *l)
{
    return l->owner == spin_cpu_tag();
}

#endif
