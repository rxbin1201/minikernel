/* Threads und Stacks, siehe thread.h */

#include "thread.h"

#define MAX_THREADS 16
#define STACK_SIZE  (64 * 1024)

typedef struct {
    ThreadFn fn;
    void    *arg;
} Start;

static Mutex lock = MUTEX_INIT;
static void *stacks[MAX_THREADS]; /* Stack je Thread-Nummer, frei bei thread_join */

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
    if (tid >= 0 && tid < MAX_THREADS)
        stacks[tid] = (void *)base;
    mutex_unlock(&lock);
    if (tid < 0)
        sys_munmap((void *)base, STACK_SIZE);
    return (int)tid;
}

int thread_join(int tid, void **result)
{
    if (tid < 0 || tid >= MAX_THREADS)
        return ERR_INVAL;
    /* Stack vorher austragen: nach dem Abholen kann ein anderer Thread die Nummer sofort neu vergeben */
    mutex_lock(&lock);
    void *st = stacks[tid];
    stacks[tid] = 0;
    mutex_unlock(&lock);
    u64 v = 0;
    s64 r = sys_thread_join(tid, &v);
    if (r != 0) {
        mutex_lock(&lock);
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
