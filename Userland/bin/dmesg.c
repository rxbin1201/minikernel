#include "libc.h"

/* dmesg: gibt das Kernel-Log aus (alles, was der Kernel seit dem Start gemeldet hat; die letzten 256 KiB).
 * Auf echter Hardware ohne serielle Schnittstelle z.B. "dmesg > /disk/log.txt" und die Datei weitergeben. */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static char buf[4096];
    u64 pos = 0;
    s64 n;
    while ((n = sys_klog(&pos, buf, sizeof(buf))) > 0)
        if (write_all(1, buf, (size_t)n) < 0)
            break;
    sys_exit(0);
}
