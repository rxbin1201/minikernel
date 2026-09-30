/* Kernel-Einstiegspunkt. Wechselt auf einen eigenen Stack; RDI (BootInfo*) bleibt erhalten. */

.section .text
.global _start
_start:
    cli
    leaq kernel_stack_top(%rip), %rsp
    xorq %rbp, %rbp
    call kmain
1:  cli
    hlt
    jmp 1b

.section .bss
.balign 16
kernel_stack_bottom:
    .skip 65536
kernel_stack_top:

.section .note.GNU-stack,"",@progbits
