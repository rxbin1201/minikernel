#ifndef CPU_H
#define CPU_H

#include <stdint.h>

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_sti(void) { __asm__ __volatile__("sti" : : : "memory"); }
static inline void cpu_cli(void) { __asm__ __volatile__("cli" : : : "memory"); }
static inline void cpu_hlt(void) { __asm__ __volatile__("hlt"); }

/* Interrupts ausschalten und den vorherigen RFLAGS-Wert liefern; irq_restore stellt IF wieder her.
 * Auf einem CPU der einfachste Schutz gegen Reentranz aus Interrupt-Handlern (nestbar). */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & (1ULL << 9))
        __asm__ __volatile__("sti" : : : "memory");
}

/* Interrupts aktivieren und bis zum naechsten Interrupt schlafen (ohne Race dazwischen). */
static inline void cpu_wait_for_interrupt(void) { __asm__ __volatile__("sti; hlt" : : : "memory"); }

#endif
