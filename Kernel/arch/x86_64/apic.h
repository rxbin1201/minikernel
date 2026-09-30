#ifndef APIC_H
#define APIC_H

#include <stdint.h>
#include "arch/x86_64/idt.h"

#define VECTOR_TIMER    0x40
#define VECTOR_KEYBOARD 0x41
#define VECTOR_SPURIOUS 0xFF

#define APIC_TIMER_HZ   100

/* Schaltet die beiden 8259-PICs ab (remap auf 0x20-0x2F und alles maskieren). */
void pic_disable(void);

/* Aktiviert den Local APIC des Boot-CPUs (Basis aus der MADT), kalibriert den APIC-Timer ueber
 * den PIT und startet ihn periodisch mit APIC_TIMER_HZ. Interrupts bleiben ausgeschaltet.
 * 0 = ok. */
int apic_init(uint64_t lapic_base);

void     apic_eoi(void);

/* Timer-Interrupt, wenn die CPU den Big Kernel Lock nicht haelt (von isr_handler aufgerufen, siehe smp.h) */
void     apic_timer_unlocked(InterruptFrame *f);
uint32_t apic_id(void);

/* Fuer weitere CPUs (smp.c): Local APIC samt Timer auf der aufrufenden CPU einschalten; INIT und STARTUP senden */
void apic_ap_init(void);
void apic_send_init(uint32_t apic_id);
void apic_send_startup(uint32_t apic_id, uint64_t page);

uint64_t apic_ticks(void);                 /* Ticks seit apic_init (je 1/APIC_TIMER_HZ s) */
uint64_t apic_timer_ticks_per_ms(void);    /* Ergebnis der Kalibrierung */
void     apic_sleep_ms(uint64_t ms);       /* braucht aktivierte Interrupts */

/* Millisekunden auf Basis des TSC; funktioniert auch mit IF = 0. Fuer Timeouts von Treibern. */
uint64_t time_ms(void);
uint64_t time_us(void); /* Mikrosekunden, ebenfalls ueber den TSC */

/* Wartet, bis `cond` wahr wird, hoechstens `timeout_ms`. Ergebnis: 1 = Bedingung erfuellt, 0 = Zeit abgelaufen. */
#define WAIT_UNTIL(cond, timeout_ms)                                              \
    ({                                                                            \
        uint64_t deadline_ = time_ms() + (timeout_ms);                            \
        int ok_ = 1;                                                              \
        while (!(cond)) {                                                         \
            if (time_ms() > deadline_) { ok_ = (cond) ? 1 : 0; break; }           \
            __asm__ __volatile__("pause");                                        \
        }                                                                         \
        ok_;                                                                      \
    })

#endif
