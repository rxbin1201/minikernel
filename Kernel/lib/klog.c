/* Kernel-Log als Ringpuffer (siehe klog.h) */

#include "lib/klog.h"
#include "arch/x86_64/spinlock.h"

static char     buf[KLOG_SIZE];
static uint64_t total;
static Spinlock klog_lock = SPINLOCK_INIT("klog"); /* innerster Lock: kprintf kommt auch unter anderen Locks */

void klog_putc(char c)
{
    uint64_t f = spin_lock(&klog_lock);
    buf[total % KLOG_SIZE] = c;
    total++;
    spin_unlock(&klog_lock, f);
}

uint64_t klog_total(void)
{
    return total;
}

uint64_t klog_read(uint64_t *pos, char *out, uint64_t max)
{
    uint64_t f = spin_lock(&klog_lock);
    uint64_t oldest = total > KLOG_SIZE ? total - KLOG_SIZE : 0;
    if (*pos < oldest)
        *pos = oldest;
    uint64_t n = 0;
    while (n < max && *pos < total) {
        out[n++] = buf[*pos % KLOG_SIZE];
        (*pos)++;
    }
    spin_unlock(&klog_lock, f);
    return n;
}
