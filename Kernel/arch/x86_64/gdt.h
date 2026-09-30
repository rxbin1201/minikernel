#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18 /* +3 (RPL) beim Einsatz */
#define GDT_USER_CODE   0x20 /* +3 (RPL) beim Einsatz */
#define GDT_TSS         0x28

#define IST_DOUBLE_FAULT 1

struct Cpu;
void gdt_init(struct Cpu *c); /* GDT und TSS dieser CPU laden, GS-Basis auf c setzen */
void gdt_dump(void); /* Diagnose */

/* Setzt TSS.rsp0 und den Stack fuer syscall_entry auf den Kernel-Stack des laufenden Threads. */
void gdt_set_kernel_stack(uint64_t top);

#endif
