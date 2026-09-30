/* Kernel-Log als Ringpuffer (siehe klog.h) */

#include "lib/klog.h"
#include "arch/x86_64/spinlock.h"

static char     buf[KLOG_SIZE];
static uint64_t total;
static uint64_t high;       /* groesster Stand von total: bis dorthin kann der Puffer schon ueberschrieben sein */
static uint64_t line_start; /* Anfang der aktuellen Zeile */
static int      cr;         /* '\r' gesehen: das naechste Zeichen (ausser '\n') ueberschreibt die Zeile */
static Spinlock klog_lock = SPINLOCK_INIT("klog"); /* innerster Lock: kprintf kommt auch unter anderen Locks */

void klog_putc(char c)
{
    uint64_t f = spin_lock(&klog_lock);
    if (c == '\r') { /* Zeilenbearbeitung der Shell: sonst landet jeder Tastendruck als eigene Zeile im Log */
        cr = 1;
        spin_unlock(&klog_lock, f);
        return;
    }
    if (cr && c != '\n' && high - line_start < KLOG_SIZE)
        total = line_start;
    cr = 0;
    buf[total % KLOG_SIZE] = c;
    total++;
    if (total > high)
        high = total;
    if (c == '\n')
        line_start = total;
    spin_unlock(&klog_lock, f);
}

uint64_t klog_total(void)
{
    return total;
}

uint64_t klog_read(uint64_t *pos, char *out, uint64_t max)
{
    uint64_t f = spin_lock(&klog_lock);
    uint64_t oldest = high > KLOG_SIZE ? high - KLOG_SIZE : 0;
    if (*pos < oldest)
        *pos = oldest;
    if (*pos > total) /* die Zeile wurde seitdem ueberschrieben */
        *pos = total;
    uint64_t n = 0;
    while (n < max && *pos < total) {
        out[n++] = buf[*pos % KLOG_SIZE];
        (*pos)++;
    }
    spin_unlock(&klog_lock, f);
    return n;
}
