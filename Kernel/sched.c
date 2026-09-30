#include "sched.h"
#include "apic.h"
#include "cpu.h"
#include "heap.h"
#include "gdt.h"
#include "kprintf.h"
#include "kstack.h"
#include "paging.h"
#include "string.h"

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

static Thread  main_thread;
static Thread *current;
static Thread *idle;
static Thread *rq_head, *rq_tail;
static Thread *all_list;
static Thread *dead_list;
static uint32_t next_id = 1;
static uint64_t switches;
static int      sched_on;

/* ---------- Run-Queue (nur mit ausgeschalteten Interrupts benutzen) ---------- */

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

/* Waehlt den naechsten Thread und wechselt zu ihm. Aufruf nur mit IF = 0. Kehrt erst zurueck,
 * wenn der aufrufende Thread wieder an der Reihe ist. */
static void schedule(void)
{
    Thread *prev = current;

    if (prev->state == T_RUNNING) {
        prev->state = T_READY;
        if (prev != idle)
            enqueue(prev);
    }

    Thread *next = dequeue();
    if (!next)
        next = idle;
    next->state = T_RUNNING;
    if (next == prev)
        return;

    current = next;
    switches++;

    /* Stack fuer Ring-3-Interrupts/Syscalls und Adressraum des neuen Threads. Der Kernel-Teil ist in allen
     * Adressraeumen gleich, deshalb kann hier mitten auf dem Kernel-Stack umgeschaltet werden. */
    if (next->kstack_top)
        gdt_set_kernel_stack(next->kstack_top);
    AddressSpace *target = next->as ? next->as : as_kernel();
    if (target != as_current())
        as_switch(target);

    switch_context(&prev->rsp, next->rsp);
}

/* ---------- Threads anlegen / beenden ---------- */

static void reap_dead(void)
{
    uint64_t f = irq_save();
    Thread *t = dead_list;
    dead_list = 0;
    irq_restore(f);

    while (t) {
        Thread *next = t->next;

        f = irq_save();
        for (Thread **p = &all_list; *p; p = &(*p)->all_next) {
            if (*p == t) {
                *p = t->all_next;
                break;
            }
        }
        irq_restore(f);

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

    uint64_t f = irq_save();
    t->id = next_id++;
    t->all_next = all_list;
    all_list = t;
    t->state = T_READY;
    if (runnable)
        enqueue(t);
    irq_restore(f);
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

static void idle_main(void *arg)
{
    (void)arg;
    for (;;) {
        reap_dead();
        cpu_wait_for_interrupt();
    }
}

void sched_init(void)
{
    uint64_t f = irq_save();

    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.id = 0;
    memcpy(main_thread.name, "main", 5);
    main_thread.state = T_RUNNING;
    all_list = &main_thread;
    current  = &main_thread;

    irq_restore(f); /* create() nimmt selbst kmalloc/irq_save */
    idle = create("idle", idle_main, 0, 0, 0);

    f = irq_save();
    sched_on = 1;
    irq_restore(f);
}

void thread_exit(void)
{
    irq_save(); /* bewusst kein Restore: dieser Thread laeuft nie wieder */
    current->state = T_DEAD;
    current->next = dead_list;
    dead_list = current;
    schedule();
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

    uint64_t f = irq_save();
    current->wake_tick = apic_ticks() + ticks;
    current->state = T_SLEEPING;
    schedule();
    irq_restore(f);
}

void mutex_lock(Mutex *m)
{
    uint64_t f = irq_save();
    while (m->locked) {
        current->next = 0;
        if (m->waiters_tail)
            m->waiters_tail->next = current;
        else
            m->waiters_head = current;
        m->waiters_tail = current;
        current->state = T_BLOCKED;
        schedule();
    }
    m->locked = 1;
    m->owner  = current;
    irq_restore(f);
}

void mutex_unlock(Mutex *m)
{
    uint64_t f = irq_save();
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
    irq_restore(f);
}

/* ---------- Timer ---------- */

void sched_tick(void)
{
    if (!sched_on)
        return;

    uint64_t now = apic_ticks();
    current->cpu_ticks++;

    for (Thread *t = all_list; t; t = t->all_next) {
        if (t->state == T_SLEEPING && t->wake_tick <= now) {
            t->state = T_READY;
            enqueue(t);
        }
    }
    schedule();
}

/* ---------- Auskunft ---------- */

void        thread_set_data(Thread *t, void *data) { t->data = data; }
void        thread_set_as(Thread *t, AddressSpace *as) { t->as = as; }
void       *thread_data(const Thread *t)        { return t->data; }
Thread     *thread_current(void)                { return current; }
uint32_t    thread_id(const Thread *t)          { return t->id; }
const char *thread_name(const Thread *t)        { return t->name; }
uint64_t    sched_switch_count(void)            { return switches; }

void sched_dump(void)
{
    static const char *names[] = {"bereit", "laeuft", "schlaeft", "blockiert", "beendet"};
    uint64_t f = irq_save();
    kprintf("  %3s %-10s %-10s %s\n", "ID", "Name", "Zustand", "CPU-Ticks");
    for (Thread *t = all_list; t; t = t->all_next)
        kprintf("  %3u %-10s %-10s %lu\n", t->id, t->name, names[t->state], t->cpu_ticks);
    irq_restore(f);
}
