#include "libc.h"

/* Prueft fork, exec, Pipes und dup2. Exit-Code 0 = alles in Ordnung, sonst die Nummer der ersten fehlgeschlagenen Pruefung. */
static int global = 1;
static int failed;

static void check(int number, int ok)
{
    if (!ok && !failed) {
        failed = number;
        fprintf(2, "[forktest] Pruefung %d fehlgeschlagen\n", number);
    }
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* 1. fork: das Kind hat eine eigene Kopie des Speichers, die Pipe ist geteilt */
    int fds[2];
    check(1, sys_pipe(fds) == 0);
    s64 pid = sys_fork();
    if (pid == 0) {
        global = 99; /* darf beim Elternprozess nicht ankommen */
        sys_close(fds[0]);
        write_all(fds[1], "hi", 2);
        sys_exit(7);
    }
    check(2, pid > 0);
    sys_close(fds[1]);
    int code = -1;
    check(3, sys_wait((int)pid, &code) == 0 && code == 7);
    check(4, global == 1);
    char buf[16];
    check(5, sys_read(fds[0], buf, sizeof(buf)) == 2 && buf[0] == 'h' && buf[1] == 'i');
    check(6, sys_read(fds[0], buf, sizeof(buf)) == 0); /* alle Schreibenden weg: Dateiende */
    sys_close(fds[0]);

    /* 2. fork + dup2 + exec: /bin/echo schreibt in die Pipe statt auf die Konsole */
    check(7, sys_pipe(fds) == 0);
    pid = sys_fork();
    if (pid == 0) {
        sys_dup2(fds[1], 1);
        sys_close(fds[0]);
        sys_close(fds[1]);
        sys_exec("/bin/echo", "echo exec-ok \"mit Leerzeichen\"");
        sys_exit(99); /* nur erreicht, wenn exec scheitert */
    }
    sys_close(fds[1]);
    char out[64];
    int total = 0;
    s64 n;
    while ((n = sys_read(fds[0], out + total, (u64)(sizeof(out) - 1 - (u64)total))) > 0)
        total += (int)n; /* bis zum Dateiende der Pipe (echo schreibt in mehreren Stuecken) */
    out[total] = 0;
    check(8, strcmp(out, "exec-ok mit Leerzeichen\n") == 0); /* Argument in Anfuehrungszeichen bleibt ein Argument */
    check(9, n == 0);
    check(10, sys_wait((int)pid, &code) == 0 && code == 0);
    sys_close(fds[0]);

    /* 3. exec eines nicht vorhandenen Programms kehrt mit einem Fehler zurueck */
    check(11, sys_exec("/bin/gibtsnicht", "x") < 0);

    /* 4. Prozessgruppe und kill: ein schlafendes Kind wird beendet */
    pid = sys_fork();
    if (pid == 0) {
        sys_sleep_ms(60000);
        sys_exit(5);
    }
    sys_sleep_ms(50);
    check(12, sys_kill((int)pid) == 0);
    s64 state = sys_wait((int)pid, &code);
    check(13, state == 2); /* 2 = gekillt */

    if (!failed)
        printf("[forktest] alles ok\n");
    sys_exit(failed);
}
