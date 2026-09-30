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
 * haelt, darf nie auf eine andere CPU warten (die spinnt womoeglich mit ausgeschalteten Interrupts auf den Lock).
 *
 * Feinere Locks (spinlock.h) fuer Teile, die auch ohne BKL laufen koennen:
 *   sched_lock   Run-Queue, Thread-Zustaende, Aufweckzeiten (sched.c). Der Timer einer CPU, die User-Code rechnet
 *                oder im Idle wartet, weckt damit Threads ohne BKL und holt ihn nur, wenn wirklich gewechselt werden
 *                muss und keine andere CPU frei ist (apic_timer_unlocked).
 *   heap_lock    Kernel-Heap (kmalloc/kfree)
 *   paging_lock  Kernel-Bereich der Seitentabellen
 *   pmm_lock     physische Frames
 * Reihenfolge: BKL -> sched_lock -> heap_lock -> paging_lock -> pmm_lock. Den Threadwechsel selbst macht nur, wer den
 * BKL haelt. Der Selbsttest "smp" prueft Heap und PMM mit Threads, die den BKL abgeben.
 *
 * Syscalls ohne BKL (syscall_unlocked in syscall.c): ticks, getpid, time, cpuinfo, yield (wenn niemand wartet) und
 * brk/mmap/munmap. Sie fassen nur den eigenen Prozess an (ein Thread je Prozess) oder Teile mit eigenem Lock. */

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
    uint64_t       bkl_timer;   /* Timer-Interrupts in User-Code, die doch den BKL holen mussten (Threadwechsel/Kill) */
    uint64_t       sys_unlocked, sys_bkl; /* Syscalls ohne bzw. mit BKL (syscall_dispatch) */
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
unsigned smp_idle_cpus(void);    /* CPUs, die gerade ihren Idle-Thread laufen lassen (Momentaufnahme, ohne Lock) */
Cpu     *smp_cpu(unsigned index); /* NULL ausserhalb */
void     smp_set_gs(Cpu *c);      /* GS-Basis dieser CPU auf c setzen */

void bkl_acquire(void);
void bkl_release(void);
int  bkl_held(void);   /* haelt diese CPU den BKL? */
int  bkl_enter(void);  /* nimmt den BKL, falls diese CPU ihn nicht schon haelt; Ergebnis fuer bkl_leave */
void bkl_leave(int taken);

#endif
