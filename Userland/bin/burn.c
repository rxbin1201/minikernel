#include "libc.h"

/* burn [ms]: rechnet ms Millisekunden lang (Standard 1000) im User-Mode, fast ohne Syscalls. Zum Ausprobieren und
 * Testen mehrerer CPUs: "burn 5000 & burn 5000 & cpus" zeigt zwei ausgelastete CPUs. Ausgabe: erreichte Runden. */

void _start(int argc, char **argv)
{
    s64 ms = argc > 1 ? atoi(argv[1]) : 1000;
    s64 end = sys_ticks() + (ms + 9) / 10;
    volatile u64 x = 1;
    u64 rounds = 0;
    while (sys_ticks() < end) {
        for (int i = 0; i < 1000000; i++)
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        rounds++;
    }
    printf("burn: %llu Runden\n", (unsigned long long)rounds);
    sys_exit(0);
}
