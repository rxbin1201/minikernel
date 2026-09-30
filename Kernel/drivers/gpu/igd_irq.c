/* Intel-Grafik Gen9: Bildwechsel-Interrupt (Vblank).
 *
 * Die Pipe meldet den Beginn jeder Austastluecke in ihrem Interrupt-Register (DE_PIPE_IIR, Bit 0). Die GPU schickt
 * den Interrupt als Nachricht (MSI) direkt an den Local APIC. Der Handler zaehlt mit und weckt Threads, die auf den
 * naechsten Bildwechsel warten (Grafikprogramme ueber SYS_GFX 4, die Doppelpufferung vor dem naechsten Schreiben).
 * Wie i915 (gen8_irq_handler): Master-Bit aus, Ursache lesen und quittieren, Master-Bit wieder an. */

#include "drivers/gpu/igd_internal.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/idt.h"
#include "core/sched.h"
#include "drivers/pci.h"
#include "lib/kprintf.h"

#define MASTER_IRQ            0x44200                   /* Bit 31: Interrupts an; Bits 16-18: Pipe A-C meldet etwas */
#define MASTER_IRQ_ENABLE     (1u << 31)
#define DE_PIPE_IMR(p)        (0x44404 + 0x10u * (uint32_t)(p))
#define DE_PIPE_IIR(p)        (0x44408 + 0x10u * (uint32_t)(p))
#define DE_PIPE_IER(p)        (0x4440C + 0x10u * (uint32_t)(p))
#define PIPE_VBLANK           (1u << 0)
#define PIPE_FRMCOUNT(p)      (0x70040 + 0x1000u * (uint32_t)(p))

#define MAX_WAITERS 4

static int               irq_ok;
static volatile uint64_t vbl_count, irq_count;
static Event             waiters[MAX_WAITERS];
static volatile int      waiting[MAX_WAITERS];

static void igd_irq(InterruptFrame *f)
{
    (void)f;
    irq_count++;
    uint32_t master = igd_rd(MASTER_IRQ);
    igd_wr(MASTER_IRQ, 0);
    int p = igd_state.scanout_pipe;
    if (p >= 0 && (master & (1u << (16 + p)))) {
        uint32_t iir = igd_rd(DE_PIPE_IIR(p));
        if (iir & PIPE_VBLANK) {
            igd_wr(DE_PIPE_IIR(p), PIPE_VBLANK); /* nur Vblank quittieren: den Unterlauf-Merker lesen die Tests */
            vbl_count++;
            for (int i = 0; i < MAX_WAITERS; i++)
                if (waiting[i])
                    event_signal(&waiters[i]);
        }
    }
    igd_wr(MASTER_IRQ, MASTER_IRQ_ENABLE);
}

void igd_irq_init(const PciDevice *d)
{
    int p = igd_state.scanout_pipe;
    if (p < 0)
        return;
    idt_set_handler(VECTOR_IGD, igd_irq);
    int msi = pci_enable_msi(d, VECTOR_IGD, apic_id());
    if (!msi) {
        kprintf("igd: kein MSI: Bildwechsel weiter ohne Interrupt\n");
        return;
    }
    igd_wr(DE_PIPE_IIR(p), PIPE_VBLANK);
    igd_wr(DE_PIPE_IER(p), igd_rd(DE_PIPE_IER(p)) | PIPE_VBLANK);
    igd_wr(DE_PIPE_IMR(p), igd_rd(DE_PIPE_IMR(p)) & ~PIPE_VBLANK);
    igd_wr(MASTER_IRQ, MASTER_IRQ_ENABLE);
    irq_ok = 1;
    /* Kommt er auch? Zwei Bilder abwarten */
    uint64_t c0 = vbl_count;
    thread_sleep_ms(100);
    if (vbl_count < c0 + 2) {
        kprintf("igd: Bildwechsel-Interrupt kommt nicht (IIR %#x, MASTER %#x): ohne Interrupt weiter\n",
                igd_rd(DE_PIPE_IIR(p)), igd_rd(MASTER_IRQ));
        igd_wr(DE_PIPE_IMR(p), igd_rd(DE_PIPE_IMR(p)) | PIPE_VBLANK);
        igd_wr(DE_PIPE_IER(p), igd_rd(DE_PIPE_IER(p)) & ~PIPE_VBLANK);
        irq_ok = 0;
        return;
    }
    kprintf("igd: Bildwechsel-Interrupt per %s (Vektor %#x)\n", msi == 2 ? "MSI-X" : "MSI", VECTOR_IGD);
}

int igd_vblank_ok(void)
{
    return irq_ok;
}

uint64_t igd_vblank_count(void)
{
    return vbl_count;
}

/* Wartet auf den naechsten Bildwechsel (hoechstens timeout_ms). 1 = kam, 0 = Zeit abgelaufen (Pipe aus?) */
int igd_wait_vblank(int timeout_ms)
{
    if (!irq_ok)
        return 0;
    uint64_t c0 = vbl_count;
    int slot = -1;
    for (int i = 0; i < MAX_WAITERS && slot < 0; i++)
        if (!waiting[i])
            slot = i;
    if (slot < 0) { /* alle Plaetze belegt (sehr unwahrscheinlich): kurz schlafen und nachsehen */
        for (int t = 0; t < timeout_ms && vbl_count == c0; t += 2)
            thread_sleep_ms(2);
        return vbl_count != c0;
    }
    waiters[slot].pending = 0;
    waiting[slot] = 1;
    uint64_t end = time_ms() + (uint64_t)timeout_ms;
    while (vbl_count == c0 && time_ms() < end)
        event_wait(&waiters[slot], (uint64_t)(end - time_ms()) + 1);
    waiting[slot] = 0;
    return vbl_count != c0;
}

/* igdtest vblank: eine Sekunde mitzaehlen und mit dem Bildzaehler der Pipe vergleichen */
int igd_vblank_test(void)
{
    if (!irq_ok) {
        kprintf("igdvbl: kein Bildwechsel-Interrupt\n");
        return -1;
    }
    int p = igd_state.scanout_pipe;
    uint32_t f0 = igd_rd(PIPE_FRMCOUNT(p));
    uint64_t v0 = vbl_count, i0 = irq_count, t0 = time_us();
    int woke = 0, late = 0;
    uint64_t worst = 0;
    while (time_us() - t0 < 1000000) {
        uint64_t c = vbl_count, w0 = time_us();
        if (igd_wait_vblank(50)) {
            woke++;
            uint64_t d = time_us() - w0; /* Wartezeit: hoechstens ein Bild */
            if (d > worst)
                worst = d;
            if (vbl_count > c + 1)
                late++;
        }
    }
    uint64_t us = time_us() - t0;
    uint32_t frames = igd_rd(PIPE_FRMCOUNT(p)) - f0;
    kprintf("igdvbl: %lu Bildwechsel-Interrupts in %lu ms (Pipe zaehlte %u Bilder), %lu Interrupts insgesamt\n",
            (unsigned long)(vbl_count - v0), (unsigned long)(us / 1000), frames, (unsigned long)(irq_count - i0));
    kprintf("igdvbl: %d-mal geweckt, laengste Wartezeit %lu us, %d-mal ein Bild verpasst\n", woke,
            (unsigned long)worst, late);
    uint64_t got = vbl_count - v0;
    return got + 2 >= frames && got <= frames + 2 && woke > 0 ? 0 : -2;
}
