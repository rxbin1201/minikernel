#include "libc.h"
#include "malloc.h"
#include "thread.h"

/* Prueft Threads in Programmen (thread.h, SYS_THREAD_*, Futex). Exit-Code 0 = alles in Ordnung, sonst die Nummer der
 * ersten fehlgeschlagenen Pruefung. Fuer die Faelle, in denen der ganze Prozess endet (exit, Absturz, kill), startet
 * es sich selbst mit einem Argument und prueft, wie das Kind endet. */

static int failed, checks;

static void check(int number, int ok)
{
    checks++;
    if (!ok && !failed) {
        failed = number;
        fprintf(2, "[threadtest] Pruefung %d fehlgeschlagen\n", number);
    }
}

/* ---------- Bausteine ---------- */

static void *twice(void *arg)
{
    return (void *)((u64)arg * 2);
}

static Mutex       count_lock = MUTEX_INIT;
static volatile u64 counter, atomic_counter;
#define COUNT_ROUNDS 100000

static void *count_up(void *arg)
{
    (void)arg;
    for (int i = 0; i < COUNT_ROUNDS; i++) {
        mutex_lock(&count_lock);
        counter++; /* ohne Sperre gingen hier Erhoehungen verloren */
        mutex_unlock(&count_lock);
        __atomic_add_fetch(&atomic_counter, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

static void *burn(void *arg)
{
    s64 until = sys_time_us() + (s64)(u64)arg * 1000;
    volatile u64 x = 0;
    while (sys_time_us() < until)
        for (int i = 0; i < 10000; i++)
            x += (u64)i;
    return 0;
}

static volatile u32 flag;

static void *wait_flag(void *arg)
{
    (void)arg;
    while (__atomic_load_n(&flag, __ATOMIC_ACQUIRE) == 0)
        sys_futex_wait(&flag, 0, 0);
    return (void *)7;
}

/* malloc/free aus mehreren Threads, jeder Block mit eigenem Muster */
static void *malloc_stress(void *arg)
{
    u64 id = (u64)arg, seed = id * 2654435761u + 1;
    unsigned char *blk[32] = {0};
    u64 len[32] = {0};
    for (int i = 0; i < 3000; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        int k = (int)((seed >> 33) % 32);
        if (blk[k]) {
            for (u64 j = 0; j < len[k]; j++)
                if (blk[k][j] != (unsigned char)(id + j))
                    return (void *)1;
            u_free(blk[k]);
            blk[k] = 0;
        } else {
            len[k] = 1 + (seed >> 40) % 3000;
            blk[k] = u_malloc(len[k]);
            if (!blk[k])
                return (void *)2;
            for (u64 j = 0; j < len[k]; j++)
                blk[k][j] = (unsigned char)(id + j);
        }
    }
    for (int k = 0; k < 32; k++)
        u_free(blk[k]);
    return 0;
}

/* mmap/munmap, waehrend die anderen Threads auf anderen CPUs laufen (TLB der anderen CPUs) */
static void *map_stress(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200; i++) {
        s64 a = sys_mmap(3 * 4096);
        if (a < 0)
            return (void *)1;
        u64 *p = (u64 *)a; /* je ein Wert in jeder der drei Seiten */
        p[0] = (u64)i;
        p[512] = (u64)i + 1;
        p[1024] = (u64)i + 2;
        if (p[0] != (u64)i || p[512] != (u64)i + 1 || p[1024] != (u64)i + 2)
            return (void *)2;
        if (sys_munmap((void *)a, 3 * 4096) != 0)
            return (void *)3;
    }
    return 0;
}

static int pipe_fds[2];
static char pipe_got;

static void *read_pipe(void *arg)
{
    (void)arg;
    char c = 0;
    s64 n = sys_read(pipe_fds[0], &c, 1); /* blockiert, waehrend der erste Thread den Deskriptor schliesst */
    pipe_got = n == 1 ? c : 0;
    return 0;
}

static volatile int never; /* bleibt 0: die Schleifen enden nur mit dem Prozess */

static void *forever(void *arg)
{
    (void)arg;
    while (!never)
        sys_futex_wait(&flag, flag, 0);
    return 0;
}

static void *spin(void *arg)
{
    (void)arg;
    for (volatile u64 x = 0; !never; x++)
        ;
    return 0;
}

static void *exit_later(void *arg)
{
    sys_sleep_ms(100);
    sys_exit((int)(u64)arg);
}

static void *crash_later(void *arg)
{
    (void)arg;
    sys_sleep_ms(50);
    *(volatile int *)0 = 1;
    return 0;
}

static void *thread_exit_later(void *arg)
{
    (void)arg;
    sys_sleep_ms(50);
    thread_exit(0);
}

/* ---------- Kindprozesse ---------- */

static int child(const char *mode)
{
    if (strcmp(mode, "mainexit") == 0) { /* erster Thread endet allein, ein anderer beendet spaeter den Prozess */
        thread_create(exit_later, (void *)42);
        thread_exit(0);
    }
    if (strcmp(mode, "exitspin") == 0) { /* exit, waehrend ein anderer Thread rechnet */
        thread_create(spin, 0);
        sys_sleep_ms(50);
        sys_exit(7);
    }
    if (strcmp(mode, "crash") == 0) { /* Absturz in einem Thread beendet alle */
        int t = thread_create(crash_later, 0);
        thread_join(t, 0); /* wartet ewig: der Absturz beendet auch diesen Thread */
        sys_exit(3);
    }
    if (strcmp(mode, "hang") == 0) { /* fuer kill: einer wartet im Futex, einer rechnet, einer schlaeft */
        thread_create(forever, 0);
        thread_create(spin, 0);
        for (;;)
            sys_sleep_ms(1000);
    }
    if (strcmp(mode, "execreset") == 0) { /* exec nach einem beendeten, nicht abgeholten Thread */
        thread_create(twice, 0);
        sys_sleep_ms(50);
        sys_exec("/bin/threadtest", "threadtest afterexec");
        return 98;
    }
    if (strcmp(mode, "afterexec") == 0) { /* neues Programm: wieder Thread 0, die Nummern beginnen wieder bei 1 */
        int first = thread_create(forever, 0), n = first > 0;
        while (n < 20 && thread_create(forever, 0) > 0)
            n++;
        return thread_self() == 0 && first == 1 && n == 20 ? 0 : 1;
    }
    if (strcmp(mode, "lastexit") == 0) { /* der letzte Thread beendet sich selbst: Code 0 */
        thread_create(thread_exit_later, 0);
        thread_exit(0);
    }
    return 99;
}

/* Startet "threadtest mode", wartet; liefert state (0 normal, 1 Absturz, 2 kill), *code = Exit-Code */
static int run_child(const char *mode, int *code, int kill_after_ms, s64 *took_ms)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "threadtest %s", mode);
    s64 t0 = sys_time_us();
    s64 pid = sys_spawn("/bin/threadtest", cmd);
    if (pid <= 0)
        return -1;
    if (kill_after_ms) {
        sys_sleep_ms((u64)kill_after_ms);
        sys_kill((int)pid);
    }
    int state = (int)sys_wait((int)pid, code);
    if (took_ms)
        *took_ms = (sys_time_us() - t0) / 1000;
    return state;
}

/* CPUs, die waehrend fn User-Code gerechnet haben (mindestens 20 Ticks) */
static int busy_cpus(void (*fn)(void))
{
    u64 before[16] = {0};
    CpuInfo ci;
    int n = 0;
    while (n < 16 && sys_cpuinfo((u64)n, &ci) == 0)
        before[n++] = ci.ticks_user;
    fn();
    int busy = 0;
    for (int i = 0; i < n; i++)
        if (sys_cpuinfo((u64)i, &ci) == 0 && ci.ticks_user - before[i] >= 20)
            busy++;
    return busy;
}

static void burn_four(void)
{
    int t[4];
    for (int i = 0; i < 4; i++)
        t[i] = thread_create(burn, (void *)600); /* je 600 ms */
    for (int i = 0; i < 4; i++)
        thread_join(t[i], 0);
}

static int cpu_count(void)
{
    CpuInfo ci;
    int n = 0;
    while (sys_cpuinfo((u64)n, &ci) == 0)
        n++;
    return n;
}

void _start(int argc, char **argv)
{
    if (argc > 1)
        sys_exit(child(argv[1]));

    /* 1. erzeugen, abholen, Rueckgabewerte; eigene Nummer */
    int t[48];
    for (int i = 0; i < 4; i++)
        t[i] = thread_create(twice, (void *)(u64)(i + 10));
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        void *r = 0;
        ok = ok && t[i] > 0 && thread_join(t[i], &r) == 0 && (u64)r == (u64)(i + 10) * 2;
    }
    check(1, ok);
    check(2, thread_self() == 0);
    check(3, thread_join(t[0], 0) != 0); /* schon abgeholt */

    /* 2. gemeinsamer Zaehler unter der Sperre */
    for (int i = 0; i < 4; i++)
        t[i] = thread_create(count_up, 0);
    for (int i = 0; i < 4; i++)
        thread_join(t[i], 0);
    check(4, counter == 4 * COUNT_ROUNDS);
    check(5, atomic_counter == 4 * COUNT_ROUNDS);

    /* 3. echt parallel auf mehreren CPUs */
    int cpus = cpu_count();
    if (cpus >= 2) {
        int busy = busy_cpus(burn_four);
        printf("[threadtest] 4 Threads rechneten auf %d von %d CPUs\n", busy, cpus);
        check(6, busy >= 2);
    }

    /* 4. Futex: falscher Wert, Zeitgrenze, Wecken */
    volatile u32 x = 5;
    check(7, sys_futex_wait(&x, 4, 0) == ERR_AGAIN);
    s64 t0 = sys_time_us();
    check(8, sys_futex_wait(&x, 5, 60) == ERR_TIMEDOUT && sys_time_us() - t0 >= 50000);
    int w = thread_create(wait_flag, 0);
    sys_sleep_ms(30);
    __atomic_store_n(&flag, 1, __ATOMIC_RELEASE);
    sys_futex_wake(&flag, 1);
    void *r = 0;
    check(9, thread_join(w, &r) == 0 && (u64)r == 7);

    /* 5. malloc/free und mmap/munmap aus vier Threads gleichzeitig */
    for (int i = 0; i < 4; i++)
        t[i] = thread_create(malloc_stress, (void *)(u64)(i + 1));
    ok = 1;
    for (int i = 0; i < 4; i++)
        ok = ok && thread_join(t[i], &r) == 0 && r == 0;
    check(10, ok);
    for (int i = 0; i < 4; i++)
        t[i] = thread_create(map_stress, 0);
    ok = 1;
    for (int i = 0; i < 4; i++)
        ok = ok && thread_join(t[i], &r) == 0 && r == 0;
    check(11, ok);

    /* 6. Deskriptor schliessen, waehrend ein anderer Thread darauf liest: das Objekt bleibt bis zum Ende des read */
    check(12, sys_pipe(pipe_fds) == 0);
    w = thread_create(read_pipe, 0);
    sys_sleep_ms(30);
    sys_close(pipe_fds[0]);
    sys_write(pipe_fds[1], "x", 1);
    check(13, thread_join(w, 0) == 0 && pipe_got == 'x');
    sys_close(pipe_fds[1]);

    /* 7. viele Threads gleichzeitig (frueher hoechstens 16; die Grenze selbst prueft limittest); exec geht nur mit einem */
    int n = 0;
    flag = 0;
    while (n < 40 && (t[n] = thread_create(forever, 0)) > 0)
        n++;
    check(14, n == 40);
    check(15, sys_exec("/bin/echo", "echo nein") == ERR_AGAIN);

    /* 8. fork mit laufenden Threads: das Kind hat nur den aufrufenden Thread */
    s64 pid = sys_fork();
    if (pid == 0)
        sys_exit(thread_self() == 0 && thread_create(twice, 0) > 0 ? 5 : 6);
    int code = -1;
    check(16, pid > 0 && sys_wait((int)pid, &code) == 0 && code == 5);

    /* 9. ganze Prozesse: Ende des ersten Threads, exit, Absturz, kill, letzter Thread */
    s64 took = 0;
    check(17, run_child("mainexit", &code, 0, 0) == 0 && code == 42);
    check(18, run_child("exitspin", &code, 0, &took) == 0 && code == 7 && took < 2000);
    check(19, run_child("crash", &code, 0, 0) == 1);
    check(20, run_child("hang", &code, 200, &took) == 2 && took < 2000);
    check(21, run_child("lastexit", &code, 0, 0) == 0 && code == 0);
    check(22, run_child("execreset", &code, 0, 0) == 0 && code == 0);

    if (!failed)
        printf("[threadtest] alle %d Pruefungen OK\n", checks);
    sys_exit(failed); /* beendet auch die 40 wartenden Threads aus Schritt 7 */
}
