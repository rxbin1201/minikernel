/* Threads und Stacks, siehe thread.h */

#include "thread.h"
#include "malloc.h"

#define STACK_SIZE  (64 * 1024)

typedef struct {
    ThreadFn fn;
    void    *arg;
} Start;

static Mutex  lock = MUTEX_INIT;
static void **stacks; /* Stack je Thread-Nummer, frei bei thread_join; die Tabelle waechst mit den Nummern */
static int    nstacks;

/* Platz fuer Nummer tid (mit lock); 0 = kein Speicher */
static int stacks_fit(int tid)
{
    if (tid < nstacks)
        return 1;
    int n = nstacks ? nstacks : 16;
    while (n <= tid)
        n *= 2;
    void **t = u_malloc(sizeof(void *) * (u64)n);
    if (!t)
        return 0;
    for (int i = 0; i < n; i++)
        t[i] = i < nstacks ? stacks[i] : 0;
    u_free(stacks);
    stacks = t;
    nstacks = n;
    return 1;
}

static void start(void *p)
{
    Start *s = p;
    thread_exit(s->fn(s->arg));
}

int thread_create(ThreadFn fn, void *arg)
{
    s64 base = sys_mmap(STACK_SIZE);
    if (base < 0)
        return (int)base;
    Start *s = (Start *)(base + STACK_SIZE) - 1; /* Startdaten oben auf dem neuen Stack */
    s->fn = fn;
    s->arg = arg;
    u_threaded = 1;
    mutex_lock(&lock);
    s64 tid = sys_thread_create(start, s, s);
    if (tid >= 0 && stacks_fit((int)tid))
        stacks[tid] = (void *)base; /* (ohne Platz bleibt der Stack bis zum Programmende eingeblendet) */
    mutex_unlock(&lock);
    if (tid < 0)
        sys_munmap((void *)base, STACK_SIZE);
    return (int)tid;
}

int thread_join(int tid, void **result)
{
    if (tid < 0)
        return ERR_INVAL;
    /* Stack vorher austragen: nach dem Abholen kann ein anderer Thread die Nummer sofort neu vergeben */
    mutex_lock(&lock);
    void *st = tid < nstacks ? stacks[tid] : 0;
    if (tid < nstacks)
        stacks[tid] = 0;
    mutex_unlock(&lock);
    u64 v = 0;
    s64 r = sys_thread_join(tid, &v);
    if (r != 0) {
        mutex_lock(&lock);
        if (tid < nstacks)
            stacks[tid] = st;
        mutex_unlock(&lock);
        return (int)r;
    }
    if (result)
        *result = (void *)v;
    if (st) /* der Thread ist beendet: sein Stack wird nicht mehr gebraucht */
        sys_munmap(st, STACK_SIZE);
    return 0;
}

void thread_exit(void *result)
{
    sys_thread_exit((u64)result);
}
