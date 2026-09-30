/* Symbolnamen und Backtraces fuer Fehlermeldungen des Kernels */

#include "lib/ksyms.h"
#include "lib/kprintf.h"
#include "mm/paging.h"

extern char __text_start[], __text_end[];

const char *ksym_lookup(uint64_t addr, uint64_t *offset)
{
    if (!ksym_count || addr < (uint64_t)__text_start || addr >= (uint64_t)__text_end || addr < ksym_table[0].addr)
        return 0;
    unsigned lo = 0, hi = ksym_count; /* letzter Eintrag mit addr <= gesuchter Adresse */
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (ksym_table[mid].addr <= addr)
            lo = mid;
        else
            hi = mid;
    }
    if (offset)
        *offset = addr - ksym_table[lo].addr;
    return ksym_names + ksym_table[lo].name;
}

void ksym_print(uint64_t addr)
{
    uint64_t off;
    const char *name = ksym_lookup(addr, &off);
    if (name)
        kprintf("%#lx %s+%#lx", (unsigned long)addr, name, (unsigned long)off);
    else
        kprintf("%#lx", (unsigned long)addr);
}

/* Ein Stackrahmen ist lesbar, wenn beide Woerter (gesichertes rbp, Ruecksprungadresse) gemappt sind */
static int frame_readable(uint64_t rbp)
{
    return rbp && !(rbp & 7) && paging_translate(rbp, 0, 0) && paging_translate(rbp + 8, 0, 0);
}

void backtrace_print(uint64_t rip, uint64_t rbp, int max_frames)
{
    kprintf("  Aufrufkette:\n    ");
    ksym_print(rip);
    kprintf("\n");
    for (int i = 0; i < max_frames && frame_readable(rbp); i++) {
        const uint64_t *frame = (const uint64_t *)rbp;
        uint64_t ret = frame[1], next = frame[0];
        if (!ret)
            break;
        kprintf("    ");
        ksym_print(ret - 1); /* -1: der Aufruf selbst, nicht die Anweisung danach */
        kprintf("\n");
        if (next <= rbp) /* der Stack waechst nach unten: Aufrufer liegen weiter oben */
            break;
        rbp = next;
    }
}
