#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Layout entspricht dem, was isr_common auf den Stack pusht (niedrigste Adresse zuerst). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) InterruptFrame;

void idt_init(void);
void idt_load(void); /* die (gemeinsame) IDT auf dieser CPU laden */

/* Handler fuer Vektoren >= 32. Das EOI an den Local APIC sendet der Dispatcher. */
typedef void (*IdtHandler)(InterruptFrame *frame);
void idt_set_handler(uint8_t vector, IdtHandler handler);

#endif
