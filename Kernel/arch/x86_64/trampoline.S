/* Startcode fuer weitere CPUs (Application Processors). smp.c kopiert alles von ap_trampoline bis
 * ap_trampoline_end in eine Seite unter 1 MiB, traegt dort die ap_tr_*-Werte ein und schickt der CPU INIT und
 * STARTUP (SIPI) mit dieser Seite. Die CPU beginnt im 16-Bit Real Mode bei Offset 0 der Seite (CS = Seite / 16).
 *
 * Ablauf: Real Mode -> Protected Mode (eigene kleine GDT) -> CR4/CR3/EFER/CR0 wie auf der Boot-CPU (Paging mit den
 * Seitentabellen des Kernels, Long Mode) -> 64-Bit-Code -> Sprung nach ap_entry(Cpu *) auf dem Stack des Idle-Threads.
 * Die Seite ist im Kernel identity-mapped; smp.c macht sie fuer die Dauer des Starts ausfuehrbar.
 * Alle Adressen im Code sind Abstaende zum Anfang der Seite (ebx = physische Adresse der Seite). */

#define OFF(sym) ((sym) - ap_trampoline)

.section .rodata
.balign 16
.global ap_trampoline
ap_trampoline:
.code16
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds
    xorl %ebx, %ebx
    movw %ax, %bx
    shll $4, %ebx               /* ebx = Seite */

    lgdtl OFF(ap_tr_gdtr)
    movl %cr0, %eax
    orl  $1, %eax               /* PE */
    movl %eax, %cr0
    ljmpl *OFF(ap_tr_pm_jump)

.code32
ap_tr_pm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    movl OFF(ap_tr_cr4)(%ebx), %eax
    movl %eax, %cr4             /* u.a. PAE */
    movl OFF(ap_tr_cr3)(%ebx), %eax
    movl %eax, %cr3             /* PML4 des Kernels (liegt unter 4 GiB) */
    movl $0xC0000080, %ecx
    movl OFF(ap_tr_efer)(%ebx), %eax
    xorl %edx, %edx
    wrmsr                       /* LME, NXE (die Seitentabellen benutzen NX), SCE */
    movl OFF(ap_tr_cr0)(%ebx), %eax
    movl %eax, %cr0             /* PG, WP ...: jetzt im Compatibility Mode */
    ljmpl *OFF(ap_tr_lm_jump)(%ebx)

.code64
ap_tr_lm64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movq OFF(ap_tr_stack)(%rbx), %rsp
    movq OFF(ap_tr_cpu)(%rbx), %rdi
    movq OFF(ap_tr_entry)(%rbx), %rax
    xorq %rbp, %rbp
    call *%rax
1:  cli
    hlt
    jmp 1b

.balign 8
ap_tr_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF    /* 0x08: 32-Bit-Code */
    .quad 0x00CF92000000FFFF    /* 0x10: Daten */
    .quad 0x00AF9A000000FFFF    /* 0x18: 64-Bit-Code */
.global ap_tr_gdtr, ap_tr_pm_jump, ap_tr_lm_jump, ap_tr_cr0, ap_tr_cr3, ap_tr_cr4, ap_tr_efer
.global ap_tr_stack, ap_tr_cpu, ap_tr_entry, ap_tr_gdt, ap_tr_pm32, ap_tr_lm64
ap_tr_gdtr:
    .word 4 * 8 - 1
    .long 0                     /* Seite + OFF(ap_tr_gdt) */
.balign 4
ap_tr_pm_jump:
    .long 0                     /* Seite + OFF(ap_tr_pm32) */
    .word 0x08
.balign 4
ap_tr_lm_jump:
    .long 0                     /* Seite + OFF(ap_tr_lm64) */
    .word 0x18
.balign 8
ap_tr_cr0:   .quad 0
ap_tr_cr3:   .quad 0
ap_tr_cr4:   .quad 0
ap_tr_efer:  .quad 0
ap_tr_stack: .quad 0
ap_tr_cpu:   .quad 0
ap_tr_entry: .quad 0
.global ap_trampoline_end
ap_trampoline_end:

.section .note.GNU-stack,"",@progbits
