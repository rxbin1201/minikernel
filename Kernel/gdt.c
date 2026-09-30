#include "gdt.h"
#include "kprintf.h"
#include <stdint.h>

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high; /* obere 4 Bit: Flags (G, D/L), untere 4 Bit: Limit 19:16 */
    uint8_t  base_high;
} __attribute__((packed)) GdtEntry;

typedef struct {
    GdtEntry low;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) GdtTssEntry;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) Tss;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) Gdtr;

/* Eintraege: null, kcode, kdata, udata, ucode, tss (2 Slots) */
static struct {
    GdtEntry    entries[5];
    GdtTssEntry tss;
} __attribute__((packed, aligned(16))) gdt;

static Tss tss;
static uint8_t kernel_rsp0_stack[16384] __attribute__((aligned(16)));
static uint8_t double_fault_stack[16384] __attribute__((aligned(16)));

/* Kernel-Stack, auf den die CPU bei Ring-3-Interrupts (TSS.rsp0) und syscall_entry (siehe syscall.S) wechselt.
 * Der Scheduler setzt ihn bei jedem Threadwechsel auf den Kernel-Stack des naechsten Threads. */
uint64_t syscall_kernel_rsp;

void gdt_set_kernel_stack(uint64_t top)
{
    tss.rsp[0] = top;
    syscall_kernel_rsp = top;
}

/* "Accessed" ist schon gesetzt (wie bei Linux): die CPU muss die GDT beim Laden eines Segments nicht beschreiben,
 * was unter manchen Hypervisoren Probleme macht. */

/* Diagnose: rohe GDT-Eintraege ausgeben */
void gdt_dump(void)
{
    const uint64_t *raw = (const uint64_t *)&gdt;
    Gdtr cur;
    __asm__ __volatile__("sgdt %0" : "=m"(cur));
    kprintf("  GDTR: Basis %#lx, Limit %#x (erwartet %#lx)\n", cur.base, cur.limit, (uint64_t)&gdt);
    for (int i = 0; i < 7; i++)
        kprintf("  GDT[%d] (Selektor %#x) = %#018lx\n", i, i * 8, raw[i]);
}

static GdtEntry make_entry(uint8_t access, uint8_t flags)
{
    /* Im Long Mode werden Base/Limit ignoriert, daher 0 / 0xFFFFF */
    GdtEntry e = {0xFFFF, 0, 0, access, (uint8_t)((flags << 4) | 0xF), 0};
    return e;
}

void gdt_init(void)
{
    gdt.entries[0] = (GdtEntry){0};
    gdt.entries[1] = make_entry(0x9B, 0xA); /* Kernel Code: P, DPL0, exec/read, accessed; G + L */
    gdt.entries[2] = make_entry(0x93, 0xC); /* Kernel Data: P, DPL0, read/write, accessed; G + D/B */
    gdt.entries[3] = make_entry(0xF3, 0xC); /* User Data:   DPL3, accessed */
    gdt.entries[4] = make_entry(0xFB, 0xA); /* User Code:   DPL3, accessed */

    tss.rsp[0]                    = (uint64_t)&kernel_rsp0_stack[sizeof(kernel_rsp0_stack)];
    tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)&double_fault_stack[sizeof(double_fault_stack)];
    tss.iopb_offset               = sizeof(Tss); /* keine I/O-Bitmap */

    uint64_t base  = (uint64_t)&tss;
    uint32_t limit = sizeof(Tss) - 1;
    gdt.tss.low.limit_low        = limit & 0xFFFF;
    gdt.tss.low.base_low         = base & 0xFFFF;
    gdt.tss.low.base_mid         = (base >> 16) & 0xFF;
    gdt.tss.low.access           = 0x89; /* P, 64-bit TSS (available) */
    gdt.tss.low.flags_limit_high = (limit >> 16) & 0xF;
    gdt.tss.low.base_high        = (base >> 24) & 0xFF;
    gdt.tss.base_upper           = base >> 32;
    gdt.tss.reserved             = 0;

    Gdtr gdtr = {sizeof(gdt) - 1, (uint64_t)&gdt};
    __asm__ __volatile__(
        "lgdt %0\n\t"
        /* CS kann nur per far return neu geladen werden */
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : "m"(gdtr), "i"((uint64_t)GDT_KERNEL_CODE), "i"((uint16_t)GDT_KERNEL_DATA)
        : "rax", "memory");

    __asm__ __volatile__("ltr %0" : : "r"((uint16_t)GDT_TSS));
}
