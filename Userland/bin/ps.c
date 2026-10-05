#include "libc.h"

/* ps: listet die Prozesse (PID, Eltern-PID, Prozessgruppe, Threads, Zustand, Name) */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int tty = sys_isatty(1) != 0;
    printf("%s  PID  PPID  PGID  THR  ZUSTAND    NAME%s\n", tty ? C_BOLD : "", tty ? C_RESET : "");
    ProcInfo pi;
    for (u64 i = 0; sys_procinfo(i, &pi) == 0; i++)
        printf("%5u %5u %5u %4u  %s%-9s%s  %s\n", pi.pid, pi.ppid, pi.pgid, pi.threads,
               tty ? (pi.state ? C_DIM : C_GREEN) : "", pi.state ? "beendet" : "laeuft", tty ? C_RESET : "", pi.name);
    sys_exit(0);
}
