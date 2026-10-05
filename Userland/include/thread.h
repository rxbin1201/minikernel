#ifndef THREAD_H
#define THREAD_H

#include "user.h"

/* Threads in Programmen: teilen Speicher und Datei-Deskriptoren. Bis zu 16 je Prozess (mit dem ersten und mit
 * beendeten, noch nicht abgeholten). Jeder neue Thread bekommt einen eigenen Stack von 64 KiB (per mmap, darunter
 * eine ungemappte Luecke als Schutz), thread_join gibt ihn wieder frei.
 *
 *   int t = thread_create(arbeit, &daten);   // arbeit(&daten) laeuft parallel
 *   void *ergebnis;
 *   thread_join(t, &ergebnis);               // wartet aufs Ende
 *
 * exit (auch das Ende von main bzw. _start), ein Absturz oder Strg+C beenden alle Threads des Programms; thread_exit
 * nur den eigenen. malloc/free sind threadsicher, sobald ein zweiter Thread laeuft. */

typedef void *(*ThreadFn)(void *arg);

int  thread_create(ThreadFn fn, void *arg); /* Nummer oder negativer Fehler (ERR_AGAIN: zu viele Threads) */
int  thread_join(int tid, void **result);   /* 0 oder Fehler; result darf 0 sein */
void thread_exit(void *result) __attribute__((noreturn));
static inline int thread_self(void) { return (int)sys_gettid(); }

/* Sperre: wer sie nicht bekommt, schlaeft (Futex), statt die CPU zu belasten. Nicht rekursiv. */
typedef struct {
    volatile u32 v; /* 0 frei, 1 belegt, 2 belegt und es wartet jemand */
} Mutex;
#define MUTEX_INIT {0}

static inline int mutex_trylock(Mutex *m)
{
    u32 c = 0;
    return __atomic_compare_exchange_n(&m->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void mutex_lock(Mutex *m)
{
    u32 c = 0;
    if (__atomic_compare_exchange_n(&m->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    if (c != 2)
        c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    while (c != 0) { /* als "es wartet jemand" markiert: schlafen, bis unlock weckt */
        sys_futex_wait(&m->v, 2, 0);
        c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    }
}

static inline void mutex_unlock(Mutex *m)
{
    if (__atomic_exchange_n(&m->v, 0, __ATOMIC_RELEASE) != 1) /* es wartete jemand */
        sys_futex_wake(&m->v, 1);
}

extern volatile int u_threaded; /* malloc.c: 1, sobald thread_create einmal lief (dann sperrt malloc) */

#endif
