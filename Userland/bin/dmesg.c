#include "libc.h"

/* dmesg: gibt das Kernel-Log aus (alles, was der Kernel seit dem Start gemeldet hat; die letzten 256 KiB).
 * Auf echter Hardware ohne serielle Schnittstelle z.B. "dmesg > /disk/log.txt" und die Datei weitergeben. */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static char buf[4096];
    u64 end = ~0ULL, pos = 0;
    s64 n;
    sys_klog(&end, buf, 0); /* nur bis zum jetzigen Ende: geht die Ausgabe auf die Konsole, landet sie selbst im Log */
    while (pos < end && (n = sys_klog(&pos, buf, end - pos < sizeof(buf) ? end - pos : sizeof(buf))) > 0)
        if (write_all(1, buf, (size_t)n) < 0)
            break;
    sys_exit(0);
}
