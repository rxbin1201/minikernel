/* Mehrere CPUs: Per-CPU-Daten, Big Kernel Lock, Start der weiteren CPUs (siehe smp.h) */

#include "arch/x86_64/smp.h"
#include "arch/x86_64/cpu.h"
#include "mm/paging.h"

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

_Static_assert(offsetof(Cpu, kernel_rsp) == CPU_OFF_KERNEL_RSP, "Offset in syscall_entry.S");
_Static_assert(offsetof(Cpu, user_rsp) == CPU_OFF_USER_RSP, "Offset in syscall_entry.S");

static Cpu      cpus[SMP_MAX_CPUS];
static unsigned cpu_count = 1;

/* ---------- Per-CPU-Daten ---------- */

void smp_set_gs(Cpu *c)
{
    c->self = c;
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0); /* GS-Basis fuer den User-Mode (swapgs tauscht die beiden) */
}

unsigned smp_cpu_count(void) { return cpu_count; }
Cpu     *smp_cpu(unsigned index) { return index < cpu_count ? &cpus[index] : 0; }

/* ---------- Big Kernel Lock ---------- */

static volatile int bkl_lock;
static volatile int bkl_owner = -1; /* Index der CPU, die ihn haelt */

void bkl_acquire(void)
{
    Cpu *c = this_cpu();
    while (__atomic_exchange_n(&bkl_lock, 1, __ATOMIC_ACQUIRE))
        while (bkl_lock)
            __asm__ __volatile__("pause");
    bkl_owner = (int)c->index;

    /* Hat eine andere CPU inzwischen Kernel-Seiten ausgeblendet oder umgestellt, koennte der TLB dieser CPU noch
     * alte Eintraege haben. Kernel-Code laeuft nur unter dem BKL: hier nachzuholen genuegt (statt TLB-Shootdown). */
    uint64_t gen = paging_kernel_gen();
    if (c->tlb_gen != gen) {
        c->tlb_gen = gen;
        uint64_t cr3;
        __asm__ __volatile__("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    }
}

void bkl_release(void)
{
    bkl_owner = -1;
    __atomic_store_n(&bkl_lock, 0, __ATOMIC_RELEASE);
}

int bkl_held(void)
{
    return bkl_owner == (int)this_cpu()->index;
}

int bkl_enter(void)
{
    if (bkl_held())
        return 0;
    bkl_acquire();
    return 1;
}

void bkl_leave(int taken)
{
    if (taken)
        bkl_release();
}

/* ---------- Start ---------- */

void smp_early_init(void)
{
    cpus[0].index = 0;
    smp_set_gs(&cpus[0]);
    bkl_acquire(); /* kmain ist Kernel-Code */
}

void smp_init(void)
{
}
