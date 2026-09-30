#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stddef.h>

/* Mehrere CPUs (SMP).
 *
 * Per-CPU-Daten: jede CPU hat eine Cpu-Struktur, im Kernel erreichbar ueber das GS-Register (this_cpu()). Beim
 * Wechsel zwischen User- und Kernel-Mode tauscht swapgs die GS-Basis (syscall_entry, isr_common, enter_user).
 *
 * Big Kernel Lock (BKL): Kernel-Code laeuft immer nur auf einer CPU gleichzeitig, User-Programme laufen parallel.
 * Regel: eine CPU haelt den BKL, solange sie Kernel-Code ausfuehrt. Ausnahmen sind nur die Einsprung-Stubs (vor
 * bkl_enter), das Warten des Idle-Threads (hlt) und das Spinnen auf den Lock selbst. Syscalls und Interrupts aus dem
 * User-Mode oder aus dem Idle-Warten holen ihn mit bkl_enter/bkl_leave; bei einem Threadwechsel bleibt er bei der CPU.
 * Deshalb gilt innerhalb des Kernels weiter: Interrupts aus = exklusiver Zugriff (wie auf einer CPU). Wer den BKL
 * haelt, darf nie auf eine andere CPU warten (die spinnt womoeglich mit ausgeschalteten Interrupts auf den Lock). */

#define SMP_MAX_CPUS 16

struct Thread;

typedef struct Cpu {
    struct Cpu    *self;        /* 0:  fuer this_cpu() (gs:0) */
    uint64_t       kernel_rsp;  /* 8:  Kernel-Stack fuer syscall_entry (Stack des laufenden Threads) */
    uint64_t       user_rsp;    /* 16: Zwischenspeicher fuer den User-Stack in syscall_entry */
    uint32_t       index;       /* 0 = Boot-CPU */
    uint32_t       apic_id;
    struct Thread *current;     /* laufender Thread */
    struct Thread *idle;        /* Idle-Thread dieser CPU */
    uint64_t       tlb_gen;     /* zuletzt gesehener Stand von paging_kernel_gen (siehe bkl_acquire) */
    volatile int   online;
    uint64_t       ticks_user, ticks_kernel, ticks_idle; /* Timer-Ticks nach Zustand der CPU */
} Cpu;

#define CPU_OFF_KERNEL_RSP 8
#define CPU_OFF_USER_RSP   16

static inline Cpu *this_cpu(void)
{
    Cpu *c;
    __asm__ __volatile__("movq %%gs:0, %0" : "=r"(c));
    return c;
}

/* Boot-CPU: GS-Basis setzen und den BKL nehmen (ganz am Anfang von kmain) */
void smp_early_init(void);

/* Weitere CPUs aus der MADT starten (nach Scheduler und APIC). Kommandozeile "nosmp" oder "cpus=N" begrenzt. */
void smp_init(void);

unsigned smp_cpu_count(void);    /* laufende CPUs */
Cpu     *smp_cpu(unsigned index); /* NULL ausserhalb */
void     smp_set_gs(Cpu *c);      /* GS-Basis dieser CPU auf c setzen */

void bkl_acquire(void);
void bkl_release(void);
int  bkl_held(void);   /* haelt diese CPU den BKL? */
int  bkl_enter(void);  /* nimmt den BKL, falls diese CPU ihn nicht schon haelt; Ergebnis fuer bkl_leave */
void bkl_leave(int taken);

#endif
