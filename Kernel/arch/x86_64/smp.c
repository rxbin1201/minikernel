/* Mehrere CPUs: Per-CPU-Daten, Big Kernel Lock, Start der weiteren CPUs (siehe smp.h) */

#include "arch/x86_64/smp.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "core/cmdline.h"
#include "core/sched.h"
#include "core/syscall.h"
#include "arch/x86_64/spinlock.h"
#include "lib/kprintf.h"
#include "lib/ksyms.h"
#include "lib/string.h"
#include "mm/paging.h"
#include "mm/pmm.h"

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

unsigned smp_idle_cpus(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < cpu_count; i++) {
        Cpu *c = &cpus[i];
        if (c->online && c->idle && *(struct Thread *volatile *)&c->current == c->idle)
            n++;
    }
    return n;
}
Cpu     *smp_cpu(unsigned index) { return index < cpu_count ? &cpus[index] : 0; }

/* ---------- Spinlocks (spinlock.h) ---------- */

int spin_cpu_tag(void)
{
    return (int)this_cpu()->index + 1;
}

void spin_deadlock(Spinlock *l)
{
    cpu_cli();
    kprintf("\n*** Spinlock '%s' auf CPU %u doppelt genommen ***\n", l->name, this_cpu()->index);
    uint64_t rbp;
    __asm__ __volatile__("mov %%rbp, %0" : "=r"(rbp));
    backtrace_print((uint64_t)__builtin_return_address(0), rbp, 16);
    kprintf("System angehalten.\n");
    for (;;)
        cpu_hlt();
}

/* ---------- Big Kernel Lock ---------- */

static volatile int bkl_lock;
static volatile int bkl_owner = -1; /* Index der CPU, die ihn haelt */

/* Fehlersuche: wartet eine CPU ueber 20 ms auf den BKL, steht im Log, welcher Thread ihn so lange hielt (das ganze
 * System stockt dann: Kernel-Code laeuft nur unter dem BKL) */
static uint32_t bkl_reports;

void bkl_acquire(void)
{
    Cpu *c = this_cpu();
    uint64_t t0 = 0;
    const char *holder = 0;
    while (__atomic_exchange_n(&bkl_lock, 1, __ATOMIC_ACQUIRE)) {
        if (!t0)
            t0 = time_us();
        for (uint32_t spins = 0; bkl_lock; spins++) {
            __asm__ __volatile__("pause");
            if (!holder && (spins & 0xFFF) == 0 && time_us() - t0 > 20000) {
                int o = bkl_owner;
                Cpu *oc = o >= 0 ? smp_cpu((unsigned)o) : 0;
                holder = oc && oc->current ? thread_name(oc->current) : "?";
            }
        }
    }
    bkl_owner = (int)c->index;
    if (t0 && holder && bkl_reports < 200) {
        bkl_reports++;
        uint64_t now = time_ms(); /* Zeitpunkt seit dem Start: Meldungen lassen sich so einem Ereignis zuordnen */
        kprintf("smp: CPU %u wartete %u ms auf den BKL - gehalten von Thread '%s' (bei %lu,%01lu s)\n", c->index,
                (uint32_t)((time_us() - t0) / 1000), holder, (unsigned long)(now / 1000),
                (unsigned long)(now % 1000 / 100));
    }

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
    cpus[0].online = 1;
    smp_set_gs(&cpus[0]);
    bkl_acquire(); /* kmain ist Kernel-Code */
}

/* Erster C-Code einer weiteren CPU (aus trampoline.S, auf dem Stack ihres Idle-Threads, IF = 0). Bis bkl_acquire
 * nur Per-CPU-Zustand anfassen. */
static void __attribute__((noreturn, used)) ap_entry(Cpu *c)
{
    gdt_init(c);  /* eigene GDT/TSS, GS-Basis = c */
    idt_load();
    paging_ap_init();
    syscall_init(); /* syscall-MSRs gibt es je CPU */
    apic_ap_init();
    c->online = 1;  /* ab hier darf die Boot-CPU die Startseite fuer die naechste CPU verwenden */
    bkl_acquire();
    sched_ap_run();
}

extern char ap_trampoline[], ap_trampoline_end[];
extern char ap_tr_gdtr[], ap_tr_pm_jump[], ap_tr_lm_jump[], ap_tr_cr0[], ap_tr_cr3[], ap_tr_cr4[], ap_tr_efer[];
extern char ap_tr_stack[], ap_tr_cpu[], ap_tr_entry[], ap_tr_gdt[], ap_tr_pm32[], ap_tr_lm64[];

/* Adresse eines Feldes in der Kopie des Startcodes */
#define TR(page, sym) ((void *)((page) + (uint64_t)((sym) - ap_trampoline)))

static void delay_us(uint64_t us)
{
    uint64_t end = time_us() + us;
    while (time_us() < end)
        __asm__ __volatile__("pause");
}

/* INIT, dann zweimal STARTUP (so schreibt es Intels MultiProcessor-Spezifikation vor). 1 = CPU laeuft. */
static int start_cpu(Cpu *c, uint64_t page)
{
    apic_send_init(c->apic_id);
    delay_us(10000);
    for (int attempt = 0; attempt < 2 && !c->online; attempt++) {
        apic_send_startup(c->apic_id, page);
        for (int i = 0; i < 1000 && !c->online; i++) /* bis 100 ms */
            delay_us(100);
    }
    return c->online;
}

void smp_init(void)
{
    const AcpiInfo *a = acpi_info();
    cpus[0].apic_id = apic_id();
    if (!a || !a->valid || a->cpu_count <= 1 || cmdline_has("nosmp"))
        return;
    unsigned want = SMP_MAX_CPUS;
    const char *opt = cmdline_get("cpus"); /* cpus=N: hoechstens N CPUs benutzen */
    if (opt && opt[0] >= '1' && opt[0] <= '9') {
        want = (unsigned)(opt[0] - '0');
        if (opt[1] >= '0' && opt[1] <= '9')
            want = want * 10 + (unsigned)(opt[1] - '0');
    }

    uint64_t page = pmm_low_page();
    uint64_t size = (uint64_t)(ap_trampoline_end - ap_trampoline);
    if (!page || size > 4096) {
        kprintf("smp: keine freie Seite unter 1 MiB fuer den Start weiterer CPUs\n");
        return;
    }

    /* Startcode kopieren und seine Sprungziele/Register eintragen */
    memcpy((void *)page, ap_trampoline, size);
    uint64_t cr0, cr3, cr4;
    __asm__ __volatile__("mov %%cr0, %0; mov %%cr3, %1; mov %%cr4, %2" : "=r"(cr0), "=r"(cr3), "=r"(cr4));
    *(uint32_t *)((uint8_t *)TR(page, ap_tr_gdtr) + 2) = (uint32_t)(uint64_t)TR(page, ap_tr_gdt);
    *(uint32_t *)TR(page, ap_tr_pm_jump) = (uint32_t)(uint64_t)TR(page, ap_tr_pm32);
    *(uint32_t *)TR(page, ap_tr_lm_jump) = (uint32_t)(uint64_t)TR(page, ap_tr_lm64);
    *(uint64_t *)TR(page, ap_tr_cr0)   = cr0;
    *(uint64_t *)TR(page, ap_tr_cr3)   = cr3;
    *(uint64_t *)TR(page, ap_tr_cr4)   = cr4 & ~(1ULL << 17);           /* PCIDE nur im Long Mode erlaubt */
    *(uint64_t *)TR(page, ap_tr_efer)  = rdmsr(0xC0000080) & ~(1ULL << 10); /* LMA setzt die CPU selbst */
    *(uint64_t *)TR(page, ap_tr_entry) = (uint64_t)ap_entry;
    paging_set_flags(page, PAGE_WRITE); /* ausfuehrbar, solange CPUs starten */

    for (int i = 0; i < a->cpu_count && cpu_count < want && cpu_count < SMP_MAX_CPUS; i++) {
        uint32_t id = a->cpu_apic_id[i];
        if (id == cpus[0].apic_id)
            continue;
        Cpu *c = &cpus[cpu_count];
        c->index = cpu_count;
        c->apic_id = id;
        c->idle = sched_ap_idle();
        if (!c->idle)
            break;
        *(uint64_t *)TR(page, ap_tr_stack) = sched_thread_stack(c->idle) & ~15ULL;
        *(uint64_t *)TR(page, ap_tr_cpu)   = (uint64_t)c;
        if (!start_cpu(c, page)) {
            /* Aufhoeren: startet sie doch noch verspaetet, darf sie keine Daten einer anderen CPU vorfinden */
            kprintf("smp: CPU mit APIC-ID %u startet nicht, keine weiteren\n", id);
            break;
        }
        cpu_count++;
    }
    paging_set_flags(page, PAGE_WRITE | PAGE_NX);
    kprintf("smp: %u von %d CPU(s) laufen\n", cpu_count, a->cpu_count);
}
