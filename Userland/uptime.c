#include "libc.h"

/* uptime: wie lange das System laeuft, Uhrzeit und Zahl der Prozesse */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    u64 s = (u64)sys_ticks() / 100; /* der Zeitgeber tickt 100-mal pro Sekunde */
    int procs = 0;
    ProcInfo pi;
    for (u64 i = 0; sys_procinfo(i, &pi) == 0; i++)
        if (pi.state == 0)
            procs++;
    s64 now = sys_time();
    if (now > 0) {
        DateTime dt;
        time_to_date((u64)now, &dt);
        printf("%02d:%02d:%02d  ", dt.hour, dt.min, dt.sec);
    }
    printf("laeuft seit ");
    if (s >= 86400)
        printf("%llu Tag(en), ", (unsigned long long)(s / 86400));
    printf("%llu:%02llu:%02llu,  %d Prozess(e)\n", (unsigned long long)(s / 3600 % 24), (unsigned long long)(s / 60 % 60),
           (unsigned long long)(s % 60), procs);
    sys_exit(0);
}
