#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

/* Praeemptiver Round-Robin-Scheduler mit einer gemeinsamen Run-Queue fuer alle CPUs. Zeitscheibe = 1 APIC-Timer-Tick
 * (10 ms). Braucht Heap, laufenden APIC-Timer (apic_init) und aktivierte Interrupts. Alle Funktionen setzen den Big
 * Kernel Lock voraus (siehe smp.h); jede CPU hat ihren eigenen laufenden Thread und Idle-Thread. */

typedef struct Thread Thread;
typedef void (*ThreadEntry)(void *arg);

/* Macht den aktuellen Kontrollfluss zum Thread 0 ("main") und legt den Idle-Thread an. */
void sched_init(void);

/* Legt einen Thread an (eigener 16-KiB-Stack) und stellt ihn in die Run-Queue. NULL bei Speichermangel. */
Thread *thread_create(const char *name, ThreadEntry entry, void *arg);

/* Wie thread_create, aber der Thread laeuft in einem eigenen Adressraum (wird beim Beenden zerstoert). */
struct AddressSpace;
Thread *thread_create_in(const char *name, ThreadEntry entry, void *arg, struct AddressSpace *as);

void  thread_set_as(Thread *t, struct AddressSpace *as); /* nach exec: neuer Adressraum des laufenden Threads */
void  thread_set_data(Thread *t, void *data); /* freier Zeiger pro Thread (z.B. Prozess) */
void *thread_data(const Thread *t);

void thread_yield(void);
void thread_sleep_ms(uint64_t ms);
void thread_exit(void) __attribute__((noreturn)); /* wird auch beim Rueckkehr aus entry aufgerufen */

Thread     *thread_current(void);
uint32_t    thread_id(const Thread *t);
const char *thread_name(const Thread *t);

/* Mutex: wartende Threads schlafen, statt zu spinnen. Nicht rekursiv. */
typedef struct {
    volatile int locked;
    Thread *owner;
    Thread *waiters_head, *waiters_tail;
} Mutex;
#define MUTEX_INIT {0, 0, 0, 0}

void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);

/* Wird vom Timer-Interrupt aufgerufen: weckt Schlafende und wechselt ggf. den Thread. */
void sched_tick(void); /* mit BKL */

/* Timer ohne BKL (CPU kam aus dem User-Mode oder dem Idle-Warten): Buchhaltung und Aufwecken; 1 = es wartet ein
 * Thread. Dann den BKL nehmen und sched_preempt() aufrufen. */
int  sched_tick_prepare(void);
void sched_preempt(void);

uint64_t sched_switch_count(void);

/* Fuer smp.c: Idle-Thread einer weiteren CPU anlegen, sein Stack-Ende, und auf der CPU den Scheduler starten */
Thread  *sched_ap_idle(void);
uint64_t sched_thread_stack(const Thread *t);
void     sched_ap_run(void) __attribute__((noreturn));
void     sched_dump(void); /* Threadliste mit Zustand und CPU-Ticks auf die Konsole */

#endif
