#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"
#include "lib/kprintf.h"
#include "mm/paging.h"
#include "core/sched.h"
#include "core/process.h"
#include "arch/x86_64/smp.h"

#define MSR_APIC_BASE     0x1B
#define APIC_BASE_ENABLE  (1ULL << 11)
#define APIC_BASE_X2APIC  (1ULL << 10)

#define LAPIC_ID          0x020
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SVR         0x0F0
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_LVT_LINT0   0x350
#define LAPIC_LVT_LINT1   0x360
#define LAPIC_LVT_ERROR   0x370
#define LAPIC_TIMER_INIT  0x380
#define LAPIC_TIMER_CUR   0x390
#define LAPIC_TIMER_DIV   0x3E0

#define LVT_MASKED        (1u << 16)
#define LVT_PERIODIC      (1u << 17)
#define SVR_ENABLE        (1u << 8)
#define TIMER_DIVIDE_16   0x3

static volatile uint32_t *lapic;
static volatile uint64_t  ticks;
static uint64_t           ticks_per_ms;
static uint64_t           tsc_per_ms;

static int x2apic; /* 1: Register ueber MSRs statt MMIO (die Firmware kann den x2APIC-Modus eingeschaltet haben) */

static inline uint32_t lapic_read(uint32_t reg)
{
    return x2apic ? (uint32_t)rdmsr(0x800 + (reg >> 4)) : lapic[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t v)
{
    if (x2apic)
        wrmsr(0x800 + (reg >> 4), v);
    else
        lapic[reg / 4] = v;
}

/* ---------- 8259 PIC ---------- */

void pic_disable(void)
{
    /* Erst auf 0x20-0x2F umlegen, damit Spurious-IRQs nicht als CPU-Exceptions ankommen, dann alles maskieren */
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);
}

/* ---------- PIT (nur zur Kalibrierung) ---------- */

/* Wartet ~10 ms ueber PIT-Kanal 2 (1,193182 MHz, Zaehlerwert 11932). */
static int pit_wait_10ms(void)
{
    const uint16_t count = 11932;
    outb(0x61, (inb(0x61) & ~0x02) | 0x01); /* Gate an, Lautsprecher aus */
    outb(0x43, 0xB0);                       /* Kanal 2, lo/hi, Modus 0, binaer */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    /* OUT2 wird high, wenn der Zaehler 0 erreicht. Grenze, falls es keinen PIT gibt (z.B. ohne Legacy-Hardware). */
    for (uint64_t spins = 0; !(inb(0x61) & 0x20); spins++)
        if (spins > 20000000ULL)
            return -1;
    return 0;
}

/* ---------- Local APIC ---------- */

static void timer_account(InterruptFrame *f)
{
    Cpu *c = this_cpu();
    if (f->cs & 3)
        c->ticks_user++;
    else if (c->current == c->idle)
        c->ticks_idle++;
    else
        c->ticks_kernel++;
    if (c->index == 0)
        __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED); /* jede CPU hat ihren Timer, die Uhrzeit zaehlt nur die Boot-CPU */
}

/* Timer, waehrend die CPU Kernel-Code ausfuehrt (BKL gehalten) */
static void timer_handler(InterruptFrame *f)
{
    timer_account(f);
    sched_tick(); /* kann zu einem anderen Thread wechseln und kehrt erst spaeter zurueck */
}

/* Timer aus dem User-Mode oder dem Idle-Warten (BKL nicht gehalten, von isr_handler direkt aufgerufen): der BKL wird
 * nur geholt, wenn ein anderer Thread wartet oder der Prozess beendet werden soll. So muessen CPUs, die rechnen,
 * nicht bei jedem Tick auf den Kernel warten. */
void apic_timer_unlocked(InterruptFrame *f)
{
    apic_eoi();
    timer_account(f);
    int user = (f->cs & 3) != 0;
    int resched = sched_tick_prepare();
    if (!resched && !(user && process_kill_pending()))
        return;
    if (user)
        this_cpu()->bkl_timer++;
    bkl_acquire();
    if (resched)
        sched_preempt(); /* kann den Thread wechseln; zurueck (vielleicht auf einer anderen CPU) gilt wieder dieser Frame */
    if (user)
        process_check_killed();
    bkl_release();
}

int apic_init(uint64_t lapic_base)
{
    uint64_t msr = rdmsr(MSR_APIC_BASE);

#ifdef APIC_FORCE_X2APIC /* Testhilfe: x2APIC selbst einschalten (Uebergang xAPIC -> x2APIC ist erlaubt) */
    uint32_t cpuid_eax = 1, cpuid_ebx, cpuid_ecx, cpuid_edx;
    __asm__ __volatile__("cpuid" : "+a"(cpuid_eax), "=b"(cpuid_ebx), "=c"(cpuid_ecx), "=d"(cpuid_edx));
    if (!(cpuid_ecx & (1u << 21)))
        kprintf("apic: (Test) CPU meldet kein x2APIC\n");
    if ((cpuid_ecx & (1u << 21)) && !(msr & APIC_BASE_X2APIC)) {
        wrmsr(MSR_APIC_BASE, msr | APIC_BASE_ENABLE);
        wrmsr(MSR_APIC_BASE, msr | APIC_BASE_ENABLE | APIC_BASE_X2APIC);
        msr = rdmsr(MSR_APIC_BASE);
    }
#endif

    if (msr & APIC_BASE_X2APIC) {
        x2apic = 1; /* Register sind nur ueber MSRs erreichbar, kein MMIO */
        lapic_base = 0;
        wrmsr(MSR_APIC_BASE, msr | APIC_BASE_ENABLE);
    } else {
        if (!lapic_base)
            lapic_base = msr & 0x000FFFFFFFFFF000ULL;
        wrmsr(MSR_APIC_BASE, (msr & ~0x000FFFFFFFFFF000ULL) | (lapic_base & 0x000FFFFFFFFFF000ULL) | APIC_BASE_ENABLE);
        if (paging_map_mmio(lapic_base, 4096) != 0) {
            kprintf("apic: LAPIC-MMIO nicht mappbar\n");
            return -1;
        }
        lapic = (volatile uint32_t *)lapic_base;
    }

    lapic_write(LAPIC_TPR, 0);                              /* alle Prioritaeten zulassen */
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED | 0xFE);
    lapic_write(LAPIC_SVR, SVR_ENABLE | VECTOR_SPURIOUS);  /* APIC einschalten */

    /* Timer kalibrieren: freilaufend von 0xFFFFFFFF, waehrend der PIT 10 ms zaehlt */
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIVIDE_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VECTOR_TIMER);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    uint64_t tsc0 = rdtsc();
    if (pit_wait_10ms() != 0) {
        kprintf("apic: PIT antwortet nicht, Timer-Kalibrierung nicht moeglich\n");
        return -1;
    }
    tsc_per_ms = (rdtsc() - tsc0) / 10;
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_INIT, 0);

    ticks_per_ms = elapsed / 10;
    if (!ticks_per_ms) {
        kprintf("apic: Kalibrierung fehlgeschlagen\n");
        return -1;
    }

    idt_set_handler(VECTOR_TIMER, timer_handler);
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | VECTOR_TIMER);
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)(ticks_per_ms * (1000 / APIC_TIMER_HZ)));

    kprintf("apic: %s, LAPIC-ID %u @ %#lx, Timer %lu Ticks/ms (Teiler 16), %d Hz\n",
            x2apic ? "x2APIC (MSR)" : "xAPIC (MMIO)", apic_id(), lapic_base, ticks_per_ms, APIC_TIMER_HZ);
    return 0;
}

/* Local APIC einer weiteren CPU einschalten, Timer wie auf der Boot-CPU (deren Kalibrierung gilt fuer alle) */
void apic_ap_init(void)
{
    if (x2apic)
        wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE | APIC_BASE_X2APIC);
    else
        wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED | 0xFE);
    lapic_write(LAPIC_SVR, SVR_ENABLE | VECTOR_SPURIOUS);
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIVIDE_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | VECTOR_TIMER);
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)(ticks_per_ms * (1000 / APIC_TIMER_HZ)));
}

#define LAPIC_ICR_LOW  0x300
#define LAPIC_ICR_HIGH 0x310
#define ICR_PENDING    (1u << 12)

/* Interprozessor-Interrupt an die CPU mit dieser APIC-ID */
static void send_ipi(uint32_t apic_id, uint32_t low)
{
    if (x2apic) {
        wrmsr(0x830, ((uint64_t)apic_id << 32) | low); /* x2APIC: ICR ist ein 64-Bit-MSR */
        return;
    }
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, low);
    WAIT_UNTIL(!(lapic_read(LAPIC_ICR_LOW) & ICR_PENDING), 10);
}

void apic_send_init(uint32_t apic_id)
{
    send_ipi(apic_id, 0x00004500); /* INIT, Level assert */
}

void apic_send_startup(uint32_t apic_id, uint64_t page)
{
    send_ipi(apic_id, 0x00004600 | (uint32_t)(page >> 12)); /* STARTUP: die CPU beginnt bei page (unter 1 MiB) */
}

void apic_eoi(void)
{
    if (lapic || x2apic)
        lapic_write(LAPIC_EOI, 0);
}

uint32_t apic_id(void)
{
    if (x2apic)
        return lapic_read(LAPIC_ID); /* x2APIC: volle 32 Bit ohne Verschiebung */
    return lapic ? lapic_read(LAPIC_ID) >> 24 : 0;
}

uint64_t apic_ticks(void)                { return ticks; }
uint64_t apic_timer_ticks_per_ms(void)   { return ticks_per_ms; }

/* Zeit ueber den TSC (bei der Kalibrierung mitgemessen). Laeuft auch mit ausgeschalteten Interrupts, anders als
 * apic_ticks; gedacht fuer Geraete-Timeouts. Vor apic_init: Millisekunden = 0. */
uint64_t time_ms(void)
{
    return tsc_per_ms ? rdtsc() / tsc_per_ms : 0;
}

uint64_t time_us(void)
{
    return tsc_per_ms >= 1000 ? rdtsc() / (tsc_per_ms / 1000) : time_ms() * 1000;
}

void apic_sleep_ms(uint64_t ms)
{
    uint64_t target = ticks + (ms * APIC_TIMER_HZ + 999) / 1000;
    while (ticks < target)
        cpu_wait_for_interrupt();
}
