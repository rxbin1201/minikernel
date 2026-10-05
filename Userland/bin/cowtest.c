#include "libc.h"
#include "malloc.h"
#include "thread.h"

/* Prueft fork mit Copy-on-Write: Eltern und Kind teilen sich die Seiten, bis einer schreibt - danach sieht keiner die
 * Aenderungen des anderen. Auch wenn der Kernel schreibt (read in einen geteilten Puffer), ueber mehrere Generationen
 * und wenn der Elternprozess danach Threads startet. Exit-Code 0 = alles in Ordnung, sonst die Nummer der ersten
 * fehlgeschlagenen Pruefung. Misst ausserdem, wie lange fork bei 32 MiB belegtem Speicher braucht. */

static int failed, checks;

static void check(int number, int ok)
{
    checks++;
    if (!ok && !failed) {
        failed = number;
        fprintf(2, "[cowtest] Pruefung %d fehlgeschlagen\n", number);
    }
}

#define BIG   (32u << 20)
#define PAGES (BIG / 4096)

static unsigned data_word = 0x1234; /* .data */
static unsigned char *big;

static void fill(unsigned char seed)
{
    for (unsigned p = 0; p < PAGES; p++)
        big[(u64)p * 4096] = (unsigned char)(seed + p);
}

static int same(unsigned char seed)
{
    for (unsigned p = 0; p < PAGES; p++)
        if (big[(u64)p * 4096] != (unsigned char)(seed + p))
            return 0;
    return 1;
}

static void wait_byte(int fd)
{
    char c;
    sys_read(fd, &c, 1);
}

static int child_ok(s64 pid)
{
    int code = -1;
    return pid > 0 && sys_wait((int)pid, &code) == 0 && code == 0;
}

static void *scribble(void *arg)
{
    (void)arg;
    fill(77); /* aus einem Thread des Elternprozesses */
    return 0;
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    big = u_malloc(BIG);
    check(1, big != 0);
    if (!big)
        sys_exit(failed);
    fill(1);
    unsigned stack_word = 0xABCD;

    /* 1. Das Kind sieht alles wie vor dem fork; was der Elternprozess danach schreibt, sieht es nicht */
    int go[2], back[2];
    sys_pipe(go);
    sys_pipe(back);
    s64 pid = sys_fork();
    if (pid == 0) {
        int ok = same(1) && data_word == 0x1234 && stack_word == 0xABCD;
        sys_write(back[1], "r", 1);
        wait_byte(go[0]); /* der Elternprozess hat inzwischen geschrieben */
        ok = ok && same(1) && data_word == 0x1234;
        fill(50); /* eigene Kopien */
        data_word = 0x5555;
        ok = ok && same(50) && data_word == 0x5555;
        sys_exit(ok ? 0 : 1);
    }
    wait_byte(back[0]);
    fill(9);
    data_word = 0x9999;
    sys_write(go[1], "g", 1);
    check(2, child_ok(pid));
    check(3, same(9) && data_word == 0x9999); /* die Kopien des Kindes haben den Elternprozess nicht beruehrt */

    /* 2. Der Kernel schreibt in eine geteilte Seite (read in den Puffer des Kindes) */
    pid = sys_fork();
    if (pid == 0) {
        wait_byte(go[0]);
        s64 n = sys_read(go[0], big + 4096 * 5, 5); /* Seite 5 ist noch geteilt */
        sys_exit(n == 5 && memcmp(big + 4096 * 5, "hallo", 5) == 0 && big[4096 * 6] == (unsigned char)(9 + 6) ? 0 : 1);
    }
    sys_write(go[1], "x", 1);
    sys_write(go[1], "hallo", 5);
    check(4, child_ok(pid));
    check(5, big[4096 * 5] == (unsigned char)(9 + 5));

    /* 3. Mehrere Generationen: das Enkelkind schreibt, Kind und Eltern bleiben unberuehrt */
    pid = sys_fork();
    if (pid == 0) {
        s64 g = sys_fork();
        if (g == 0) {
            fill(200);
            sys_exit(same(200) ? 0 : 1);
        }
        int ok = child_ok(g) && same(9);
        fill(100);
        sys_exit(ok && same(100) ? 0 : 1);
    }
    check(6, child_ok(pid));
    check(7, same(9));

    /* 4. Der Elternprozess startet nach dem fork einen Thread: seine Seiten werden vorher aufgeloest, das Kind behaelt
     * die alten Inhalte */
    pid = sys_fork();
    if (pid == 0) {
        wait_byte(go[0]);
        sys_exit(same(9) ? 0 : 1);
    }
    int t = thread_create(scribble, 0);
    check(8, t > 0 && thread_join(t, 0) == 0 && same(77));
    sys_write(go[1], "t", 1);
    check(9, child_ok(pid));

    /* 5. fork + exec (der haeufigste Fall: die Seiten werden nie kopiert) */
    pid = sys_fork();
    if (pid == 0) {
        sys_exec("/bin/sleep", "sleep 0");
        sys_exit(1);
    }
    check(10, child_ok(pid));

    /* 6. Wie lange dauert fork mit 32 MiB belegtem Speicher? (vorher: alle 8192 Seiten kopieren) */
    s64 best = -1;
    for (int i = 0; i < 3; i++) {
        s64 t0 = sys_time_us();
        pid = sys_fork();
        if (pid == 0)
            sys_exit(0);
        s64 us = sys_time_us() - t0;
        child_ok(pid);
        if (best < 0 || us < best)
            best = us;
    }
    printf("[cowtest] fork mit 32 MiB: %d us\n", (int)best);
    check(11, best >= 0 && best < 200000); /* in QEMU ohne KVM: Kopieren etwa 780 ms, Copy-on-Write etwa 7 ms */

    if (!failed)
        printf("[cowtest] alle %d Pruefungen OK\n", checks);
    sys_exit(failed);
}
