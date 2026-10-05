#include "arch/x86_64/idt.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/apic.h"
#include "lib/kprintf.h"
#include "lib/ksyms.h"
#include "arch/x86_64/smp.h"
#include "core/process.h"

#define IDT_ENTRIES    256
#define ISR_STUBS      256  /* 0-31 Exceptions, 32-255 Interrupts */
#define GATE_INTERRUPT 0x8E /* P, DPL0, 64-bit Interrupt Gate (loescht IF) */

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) IdtEntry;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) Idtr;

static IdtEntry idt[IDT_ENTRIES] __attribute__((aligned(16)));

extern uint64_t isr_stub_table[ISR_STUBS]; /* aus isr.S */

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound Range Exceeded",
    "Invalid Opcode", "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 FP Exception", "Alignment Check", "Machine Check",
    "SIMD FP Exception", "Virtualization Exception", "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved",
};

static void set_gate(int vector, uint64_t handler, uint8_t ist)
{
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].ist         = ist;
    idt[vector].type_attr   = GATE_INTERRUPT;
    idt[vector].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = handler >> 32;
    idt[vector].zero        = 0;
}

void idt_init(void)
{
    for (int i = 0; i < ISR_STUBS; i++)
        set_gate(i, isr_stub_table[i], i == 8 ? IST_DOUBLE_FAULT : 0);

    idt_load();
}

/* Alle CPUs teilen sich eine IDT */
void idt_load(void)
{
    Idtr idtr = {sizeof(idt) - 1, (uint64_t)&idt};
    __asm__ __volatile__("lidt %0" : : "m"(idtr));
}

static IdtHandler handlers[IDT_ENTRIES];

void idt_set_handler(uint8_t vector, IdtHandler handler)
{
    handlers[vector] = handler;
}

static void isr_dispatch(InterruptFrame *f);

/* Wird von isr_common (isr.S) aufgerufen. Kam der Interrupt aus dem User-Mode oder dem Idle-Warten, haelt diese
 * CPU den Big Kernel Lock noch nicht (smp.h). Wechselt der Handler den Thread, gehoert "taken" zum unterbrochenen
 * Kontext: er wird beim Fortsetzen (vielleicht auf einer anderen CPU) wieder richtig freigegeben. */
void isr_handler(InterruptFrame *f)
{
    if (f->vector == VECTOR_TIMER && !bkl_held()) {
        apic_timer_unlocked(f); /* holt den BKL nur bei Bedarf */
        return;
    }
    if (f->vector == VECTOR_WAKE) { /* nur aus dem hlt holen: den bereiten Thread holt sich idle_loop */
        apic_eoi();
        return;
    }
    if (f->vector == VECTOR_TLB) { /* TLB leeren (CR3 neu laden) und mitzaehlen */
        uint64_t cr3;
        __asm__ __volatile__("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
        __atomic_add_fetch(&this_cpu()->tlb_flushes, 1, __ATOMIC_RELEASE);
        apic_eoi();
        return;
    }
    int taken = bkl_enter();
    isr_dispatch(f);
    bkl_leave(taken);
}

static void isr_dispatch(InterruptFrame *f)
{
    /* Externe Interrupts (APIC-Timer, IOAPIC ...) */
    if (f->vector >= 32) {
        /* EOI vor dem Handler: der Timer-Handler kann den Thread wechseln und kehrt dann erst spaeter
         * zurueck; bis dahin wuerde der APIC sonst weitere Timer-Interrupts blockieren.
         * Spurious Interrupts bekommen kein EOI. */
        if (f->vector != VECTOR_SPURIOUS)
            apic_eoi();
        if (handlers[f->vector])
            handlers[f->vector](f);
        else if (f->vector != VECTOR_SPURIOUS)
            kprintf("unbehandelter Interrupt %lu\n", f->vector);
        if ((f->cs & 3) == 3)
            process_check_killed(); /* Ctrl-C/kill: ein Prozess im User-Mode wird spaetestens beim naechsten Timer-Tick beendet */
        return;
    }

    /* Breakpoint ist ein Trap: einfach weiterlaufen */
    if (f->vector == 3) {
        kprintf("[int3] Breakpoint bei RIP=%#lx\n", f->rip);
        return;
    }

    /* Schreibzugriff auf eine Copy-on-Write-Seite (nach fork; Fehlercode: Seite vorhanden + Schreiben), aus dem
     * Programm oder aus dem Kernel, der in einen User-Puffer schreibt: eigene Kopie anlegen und den Befehl wiederholen */
    if (f->vector == 14 && (f->error_code & 3) == 3) {
        uint64_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        if (process_cow_fault(process_current(), cr2))
            return;
    }
    /* Seite fehlt (Fehlercode Bit 0 = 0): eingeblendete Datei (jetzt laden) oder der Stack waechst */
    if (f->vector == 14 && !(f->error_code & 1)) {
        uint64_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        if (process_page_fault(process_current(), cr2, (f->error_code & 2) != 0))
            return;
    }

    /* Ausnahme in Ring 3: nur den Prozess beenden, der Kernel laeuft weiter */
    if ((f->cs & 3) == 3) {
        Process *p = process_current();
        kprintf("\n[Prozess %u '%s'] %s bei RIP=%#lx, Fehlercode %#lx", process_pid(p), process_name(p),
                f->vector < 32 ? exception_names[f->vector] : "Fehler", f->rip, f->error_code);
        if (f->vector == 14) {
            uint64_t cr2;
            __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
            kprintf(", Adresse %#lx", cr2);
        }
        kprintf(" -> beendet\n");
        process_fault();
    }

    /* Seitenfehler im Kernel an einer User-Adresse: ein Syscall hatte den Puffer geprueft, dann hat ein anderer
     * Thread desselben Prozesses ihn ausgeblendet (munmap, brk). Das trifft nur diesen Prozess. */
    if (f->vector == 14 && process_current()) {
        uint64_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        if (cr2 >= USER_BASE && cr2 < USER_END) {
            Process *p = process_current();
            kprintf("\n[Prozess %u '%s'] Kernel greift auf ausgeblendeten Speicher %#lx zu (RIP=%#lx) -> beendet\n",
                    process_pid(p), process_name(p), cr2, f->rip);
            process_fault();
        }
    }

    kprintf("\n*** EXCEPTION %lu: %s ***\n", f->vector, f->vector < 32 ? exception_names[f->vector] : "Interrupt");

    if (f->vector == 14) {
        uint64_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  CR2 (Fault-Adresse) = %#018lx\n", cr2);
    }

    kprintf("  error  = %#018lx\n", f->error_code);
    kprintf("  RIP    = %#018lx\n", f->rip);
    kprintf("  CS     = %#018lx\n", f->cs);
    kprintf("  RFLAGS = %#018lx\n", f->rflags);
    kprintf("  RSP    = %#018lx\n", f->rsp);
    kprintf("  RAX    = %#018lx  RBX = %#018lx\n", f->rax, f->rbx);
    kprintf("  RCX    = %#018lx  RDX = %#018lx\n", f->rcx, f->rdx);

    if (f->vector == 13) { /* #GP im Kernel: bei einem iretq steht der Interrupt-Frame am Stackzeiger */
        const uint64_t *fr = (const uint64_t *)f->rsp;
        kprintf("  Stack bei RSP (iretq-Frame: rip cs rflags rsp ss):\n   ");
        for (int k = 0; k < 5; k++)
            kprintf(" %#lx", fr[k]);
        kprintf("\n");
        gdt_dump();
    }
    backtrace_print(f->rip, f->rbp, 16);
    kprintf("System angehalten.\n");

    for (;;)
        __asm__ __volatile__("cli; hlt");
}
