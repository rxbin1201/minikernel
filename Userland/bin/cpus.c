#include "libc.h"

/* cpus: Auslastung jeder CPU ueber eine Sekunde (oder N Sekunden: cpus N), aufgeteilt in User, Kernel und Leerlauf */

#define MAX_CPUS 16

void _start(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 1;
    if (secs < 1)
        secs = 1;
    CpuInfo a[MAX_CPUS], b[MAX_CPUS];
    int n = 0;
    while (n < MAX_CPUS && sys_cpuinfo((u64)n, &a[n]) == 0)
        n++;
    sys_sleep_ms((u64)secs * 1000);
    for (int i = 0; i < n; i++)
        sys_cpuinfo((u64)i, &b[i]);

    printf("%d CPU(s), Messung ueber %d s\n", n, secs);
    printf("CPU  APIC   User  Kernel  Leerlauf\n");
    for (int i = 0; i < n; i++) {
        u64 u = b[i].ticks_user - a[i].ticks_user, k = b[i].ticks_kernel - a[i].ticks_kernel;
        u64 idle = b[i].ticks_idle - a[i].ticks_idle, all = u + k + idle;
        if (!all)
            all = 1;
        printf("%3d  %4u  %4llu%%  %5llu%%  %7llu%%\n", i, b[i].apic_id, (unsigned long long)(u * 100 / all),
               (unsigned long long)(k * 100 / all), (unsigned long long)(idle * 100 / all));
    }
    sys_exit(0);
}
