#include "core/sched.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "mm/heap.h"
#include "arch/x86_64/gdt.h"
#include "lib/kprintf.h"
#include "mm/kstack.h"
#include "mm/paging.h"
#include "lib/string.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/spinlock.h"

typedef enum { T_READY, T_RUNNING, T_SLEEPING, T_BLOCKED, T_DEAD } ThreadState;

struct Thread {
    uint64_t    rsp;        /* gesicherter Stackzeiger (muss erstes Feld bleiben) */
    uint32_t    id;
    char        name[16];
    ThreadState state;
    uint64_t    wake_tick;  /* fuer T_SLEEPING */
    uint64_t    cpu_ticks;  /* Timer-Ticks, waehrend der Thread lief */
    uint64_t    kstack_top; /* oberes Ende des Kernel-Stacks (0 beim Boot-Thread) */
    AddressSpace *as;       /* eigener Adressraum (User-Prozess) oder NULL = Kernel-Adressraum */
    void       *data;       /* frei nutzbar, z.B. Zeiger auf den Prozess */
    ThreadEntry entry;
    void       *arg;
    Thread     *next;       /* Run-Queue, Dead-Liste oder Mutex-Wartekette (immer nur in einer davon) */
    Thread     *all_next;   /* Liste aller Threads */
};

extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);

/* Laufender Thread und Idle-Thread gehoeren zur CPU (smp.h). Gemeinsam sind Run-Queue, Thread-Zustaende,
 * Aufweckzeiten, Listen und Mutex-Wartelisten: sie stehen unter sched_lock, damit der Timer einer CPU, die gerade
 * User-Code rechnet, ohne Big Kernel Lock Threads wecken und pruefen kann, ob ein Wechsel ansteht (sched_tick_prepare).
 *
 * Den eigentlichen Wechsel (schedule) macht dagegen nur, wer den BKL haelt: nur dort werden Threads aus der Run-Queue
 * genommen. Weil die CPU den BKL ueber den ganzen Wechsel behaelt, kann kein anderer einen gerade abgegebenen Thread
 * starten, bevor switch_context seinen Kontext gesichert hat, auch wenn sched_lock vorher frei wird.
 * Aufraeumen beendeter Threads (reap_dead) ebenso nur unter dem BKL. */
static inline Thread *cur_thread(void) { return this_cpu()->current; }

static Spinlock sched_lock = SPINLOCK_INIT("sched");

static Thread  main_thread;
static Thread *rq_head, *rq_tail;
static Thread *all_list;
static Thread *dead_list;
static uint32_t next_id = 1;
static uint64_t switches;
static int      sched_on;

/* ---------- Run-Queue (nur unter sched_lock) ---------- */

static void enqueue(Thread *t)
{
    t->next = 0;
    if (rq_tail)
        rq_tail->next = t;
    else
        rq_head = t;
    rq_tail = t;
}

static Thread *dequeue(void)
{
    Thread *t = rq_head;
    if (t) {
        rq_head = t->next;
        if (!rq_head)
            rq_tail = 0;
        t->next = 0;
    }
    return t;
}

/* Waehlt den naechsten Thread und wechselt zu ihm. Aufruf mit BKL, IF = 0 und gehaltenem sched_lock (wird hier
 * freigegeben). Kehrt erst zurueck, wenn der aufrufende Thread wieder an der Reihe ist. */
static void schedule_locked(void)
{
    Cpu *c = this_cpu();
    Thread *prev = c->current;

    if (prev->state == T_RUNNING) {
        prev->state = T_READY;
        if (prev != c->idle)
            enqueue(prev);
    }

    Thread *next = dequeue();
    if (!next)
        next = c->idle;
    next->state = T_RUNNING;
    if (next == prev) {
        spin_unlock(&sched_lock, 0); /* 0: Interrupts bleiben aus */
        return;
    }

    c->current = next;
    switches++;
    spin_unlock(&sched_lock, 0);

    /* Stack fuer Ring-3-Interrupts/Syscalls und Adressraum des neuen Threads. Der Kernel-Teil ist in allen
     * Adressraeumen gleich, deshalb kann hier mitten auf dem Kernel-Stack umgeschaltet werden. */
    if (next->kstack_top)
        gdt_set_kernel_stack(next->kstack_top);
    AddressSpace *target = next->as ? next->as : as_kernel();
    if (target != as_current())
        as_switch(target);

    switch_context(&prev->rsp, next->rsp);
}

static void schedule(void)
{
    spin_lock(&sched_lock);
    schedule_locked();
}

/* ---------- Threads anlegen / beenden ---------- */

static void reap_dead(void)
{
    uint64_t f = spin_lock(&sched_lock);
    Thread *t = dead_list;
    dead_list = 0;
    spin_unlock(&sched_lock, f);

    while (t) {
        Thread *next = t->next;

        f = spin_lock(&sched_lock);
        for (Thread **p = &all_list; *p; p = &(*p)->all_next) {
            if (*p == t) {
                *p = t->all_next;
                break;
            }
        }
        spin_unlock(&sched_lock, f);

        if (t->as)
            as_destroy(t->as); /* laeuft nicht mehr, also ist dieser Adressraum nicht in CR3 */
        kstack_free(t->kstack_top);
        kfree(t);
        t = next;
    }
}

/* Erster Code eines neuen Threads (von thread_trampoline aufgerufen, noch mit IF = 0). */
void thread_bootstrap(ThreadEntry entry, void *arg)
{
    cpu_sti();
    reap_dead();
    entry(arg);
    thread_exit();
}

static Thread *create(const char *name, ThreadEntry entry, void *arg, AddressSpace *as, int runnable)
{
    Thread *t = kcalloc(1, sizeof(Thread));
    if (!t)
        return 0;
    t->kstack_top = kstack_alloc();
    if (!t->kstack_top) {
        kfree(t);
        return 0;
    }
    t->as = as;

    size_t i = 0;
    for (; name[i] && i < sizeof(t->name) - 1; i++)
        t->name[i] = name[i];
    t->entry = entry;
    t->arg   = arg;

    /* Initialer Stack-Frame passend zu switch_context: r15 r14 r13 r12 rbx rbp, dann Ruecksprungadresse.
     * Nach dem 'ret' liegt rsp genau auf top (16-Byte-ausgerichtet), wie es der 'call' im Trampolin braucht. */
    uint64_t top = t->kstack_top & ~15ULL;
    uint64_t *sp = (uint64_t *)(top - 7 * 8);
    sp[0] = 0;                        /* r15 */
    sp[1] = 0;                        /* r14 */
    sp[2] = (uint64_t)arg;            /* r13 */
    sp[3] = (uint64_t)entry;          /* r12 */
    sp[4] = 0;                        /* rbx */
    sp[5] = 0;                        /* rbp */
    sp[6] = (uint64_t)thread_trampoline;
    t->rsp = (uint64_t)sp;

    uint64_t f = spin_lock(&sched_lock);
    t->id = next_id++;
    t->all_next = all_list;
    all_list = t;
    t->state = T_READY;
    if (runnable)
        enqueue(t);
    spin_unlock(&sched_lock, f);
    return t;
}

Thread *thread_create_in(const char *name, ThreadEntry entry, void *arg, AddressSpace *as)
{
    reap_dead();
    return create(name, entry, arg, as, 1);
}

Thread *thread_create(const char *name, ThreadEntry entry, void *arg)
{
    return thread_create_in(name, entry, arg, 0);
}

/* Idle-Thread jeder CPU: waehrend er auf den naechsten Interrupt wartet, haelt die CPU den Big Kernel Lock nicht.
 * Der Timer-Interrupt holt ihn wieder (bkl_enter) und wechselt ueber sched_tick zu bereiten Threads. */
static void __attribute__((noreturn)) idle_loop(void)
{
    for (;;) {
        reap_dead();
        cpu_cli();
        bkl_release();
        do /* ohne BKL schlafen, bis es etwas aufzuraeumen gibt; bereite Threads holt der Timer (sched_tick_prepare) */
            cpu_wait_for_interrupt(); /* sti; hlt */
        while ((cpu_cli(), !*(Thread *volatile *)&dead_list));
        bkl_acquire();
        cpu_sti();
    }
}

static void idle_main(void *arg)
{
    (void)arg;
    idle_loop();
}

/* Legt den Idle-Thread einer weiteren CPU an; die CPU laeuft spaeter mit sched_ap_run auf seinem Stack los. */
Thread *sched_ap_idle(void)
{
    return create("idle", idle_main, 0, 0, 0);
}

uint64_t sched_thread_stack(const Thread *t)
{
    return t->kstack_top;
}

/* Erster Code einer weiteren CPU mit Scheduler (auf dem Stack ihres Idle-Threads, BKL gehalten, IF = 0) */
void sched_ap_run(void)
{
    Cpu *c = this_cpu();
    c->current = c->idle;
    c->idle->state = T_RUNNING;
    gdt_set_kernel_stack(c->idle->kstack_top);
    cpu_sti();
    idle_loop();
}

void sched_init(void)
{
    uint64_t f = irq_save();

    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.id = 0;
    memcpy(main_thread.name, "main", 5);
    main_thread.state = T_RUNNING;
    all_list = &main_thread;
    this_cpu()->current = &main_thread;

    irq_restore(f); /* create() nimmt selbst kmalloc/irq_save */
    this_cpu()->idle = create("idle", idle_main, 0, 0, 0);

    f = irq_save();
    sched_on = 1;
    irq_restore(f);
}

void thread_exit(void)
{
    spin_lock(&sched_lock); /* bewusst kein Restore der Interrupts: dieser Thread laeuft nie wieder */
    cur_thread()->state = T_DEAD;
    cur_thread()->next = dead_list;
    dead_list = cur_thread();
    schedule_locked();
    for (;;)
        cpu_hlt(); /* unerreichbar */
}

/* ---------- Kooperative Funktionen ---------- */

void thread_yield(void)
{
    uint64_t f = irq_save();
    schedule();
    irq_restore(f);
}

void thread_sleep_ms(uint64_t ms)
{
    uint64_t ticks = (ms * APIC_TIMER_HZ + 999) / 1000;
    if (!ticks)
        ticks = 1;

    uint64_t f = spin_lock(&sched_lock);
    cur_thread()->wake_tick = apic_ticks() + ticks;
    cur_thread()->state = T_SLEEPING;
    schedule_locked();
    irq_restore(f);
}

void mutex_lock(Mutex *m)
{
    uint64_t f = spin_lock(&sched_lock);
    while (m->locked) {
        cur_thread()->next = 0;
        if (m->waiters_tail)
            m->waiters_tail->next = cur_thread();
        else
            m->waiters_head = cur_thread();
        m->waiters_tail = cur_thread();
        cur_thread()->state = T_BLOCKED;
        schedule_locked();
        spin_lock(&sched_lock);
    }
    m->locked = 1;
    m->owner  = cur_thread();
    spin_unlock(&sched_lock, f);
}

void mutex_unlock(Mutex *m)
{
    uint64_t f = spin_lock(&sched_lock);
    m->locked = 0;
    m->owner  = 0;
    Thread *w = m->waiters_head;
    if (w) {
        m->waiters_head = w->next;
        if (!m->waiters_head)
            m->waiters_tail = 0;
        w->state = T_READY;
        enqueue(w); /* prueft beim Aufwachen erneut, ob der Mutex frei ist */
    }
    spin_unlock(&sched_lock, f);
}

/* ---------- Timer ---------- */

/* Tick-Buchhaltung und Aufwecken faelliger Threads; 1 = es wartet ein Thread, ein Wechsel steht an. Braucht keinen
 * BKL (nur sched_lock), damit CPUs, die User-Code rechnen, fuer den Timer nicht auf den BKL warten muessen. */
int sched_tick_prepare(void)
{
    if (!sched_on)
        return 0;
    cur_thread()->cpu_ticks++; /* der laufende Thread gehoert dieser CPU */

    uint64_t now = apic_ticks();
    uint64_t f = spin_lock(&sched_lock);
    for (Thread *t = all_list; t; t = t->all_next) {
        if (t->state == T_SLEEPING && t->wake_tick <= now) {
            t->state = T_READY;
            enqueue(t);
        }
    }
    int waiting = rq_head != 0;
    spin_unlock(&sched_lock, f);
    /* Rechnet hier User-Code (BKL nicht gehalten) und ist eine andere CPU frei, uebernimmt die den Wartenden bei ihrem
     * naechsten Tick: so wird rechnender Code nicht unterbrochen, solange CPUs frei sind. Haelt diese CPU den BKL
     * (Kernel-Code), muss sie selbst wechseln: die freie CPU kaeme ohne den BKL nie an den Wartenden. */
    if (waiting && !bkl_held() && cur_thread() != this_cpu()->idle && smp_idle_cpus() > 0)
        waiting = 0;
    return waiting;
}

/* Naechster Thread nach Round-Robin (mit BKL, IF = 0) */
void sched_preempt(void)
{
    schedule();
}

void sched_tick(void)
{
    if (sched_tick_prepare())
        schedule();
}

/* ---------- Auskunft ---------- */

void        thread_set_data(Thread *t, void *data) { t->data = data; }
void        thread_set_as(Thread *t, AddressSpace *as) { t->as = as; }
void       *thread_data(const Thread *t)        { return t->data; }
Thread     *thread_current(void)                { return cur_thread(); }
uint32_t    thread_id(const Thread *t)          { return t->id; }
const char *thread_name(const Thread *t)        { return t->name; }
uint64_t    sched_switch_count(void)            { return switches; }

void sched_dump(void)
{
    static const char *names[] = {"bereit", "laeuft", "schlaeft", "blockiert", "beendet"};
    uint64_t f = spin_lock(&sched_lock);
    kprintf("  %3s %-10s %-10s %s\n", "ID", "Name", "Zustand", "CPU-Ticks");
    for (Thread *t = all_list; t; t = t->all_next)
        kprintf("  %3u %-10s %-10s %lu\n", t->id, t->name, names[t->state], t->cpu_ticks);
    spin_unlock(&sched_lock, f);
}
