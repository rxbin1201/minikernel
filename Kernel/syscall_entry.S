/* Einsprung fuer die syscall-Instruktion und Wechsel nach Ring 3. */

.section .text

/* syscall_entry: von der CPU per LSTAR angesprungen.
 *   rax = Nummer, Argumente rdi, rsi, rdx, r10, r8, r9 (wie unter Linux); rcx = Ruecksprung-RIP, r11 = RFLAGS.
 * IF ist durch SFMASK aus. Es wird noch der User-Stack benutzt, deshalb sofort auf den Kernel-Stack des laufenden
 * Threads (syscall_kernel_rsp, vom Scheduler gepflegt) wechseln. Alle Register werden als SyscallFrame gesichert
 * (Layout siehe syscall.h; fork kopiert ihn fuer das Kind); zurueck kommen alle unveraendert ausser rax
 * (Rueckgabewert), rcx und r11 (wie ueblich zerstoert). */
.global syscall_entry
syscall_entry:
    movq %rsp, syscall_user_rsp(%rip)
    movq syscall_kernel_rsp(%rip), %rsp

    pushq syscall_user_rsp(%rip)
    pushq %r11
    pushq %rcx
    pushq %rax
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r10
    pushq %r8
    pushq %r9
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi        /* SyscallFrame* */
    cld
    call syscall_dispatch  /* rsp ist 16-Byte-ausgerichtet (16 Pushes auf ausgerichtetem Stack) */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax              /* Rueckgabewert (von syscall_dispatch in den Frame geschrieben) */
    popq %rcx
    popq %r11
    popq %rsp              /* User-Stack */
    sysretq

/* void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg1, uint64_t arg2)   [noreturn]
 * Springt per iretq nach Ring 3. arg1/arg2 landen in rdi/rsi (argc/argv von _start). */
.global enter_user
enter_user:
    movw $0x1B, %ax        /* User-Datensegment (0x18 | RPL 3) */
    movw %ax, %ds
    movw %ax, %es

    pushq $0x1B            /* SS */
    pushq %rsi             /* RSP */
    pushq $0x202           /* RFLAGS: IF an */
    pushq $0x23            /* CS (0x20 | RPL 3) */
    pushq %rdi             /* RIP */

    movq %rdx, %rdi        /* arg1 */
    movq %rcx, %rsi        /* arg2 */
    xorl %eax, %eax        /* keine Kernel-Werte in Registern nach Ring 3 mitgeben */
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq

/* void enter_user_regs(const SyscallFrame *r)   [noreturn]
 * Springt nach Ring 3 mit genau den Registern aus einem SyscallFrame (fuer das Kind eines fork). Der Aufrufer setzt
 * r->rax vorher auf 0. Offsets siehe SyscallFrame in syscall.h: r15 0, r14 8, r13 16, r12 24, rbx 32, rbp 40, r9 48,
 * r8 56, r10 64, rdx 72, rsi 80, rdi 88, rax 96, rcx 104 (= User-RIP), r11 112 (= User-RFLAGS), user_rsp 120. */
.global enter_user_regs
enter_user_regs:
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    pushq $0x1B            /* SS */
    pushq 120(%rdi)        /* RSP */
    movq 112(%rdi), %rax
    orq  $0x202, %rax      /* RFLAGS des Users, IF sicher an */
    andq $-257, %rax        /* TF aus (~0x100) */
    pushq %rax
    pushq $0x23            /* CS */
    pushq 104(%rdi)        /* RIP */

    movq 0(%rdi), %r15
    movq 8(%rdi), %r14
    movq 16(%rdi), %r13
    movq 24(%rdi), %r12
    movq 32(%rdi), %rbx
    movq 40(%rdi), %rbp
    movq 48(%rdi), %r9
    movq 56(%rdi), %r8
    movq 64(%rdi), %r10
    movq 72(%rdi), %rdx
    movq 80(%rdi), %rsi
    movq 96(%rdi), %rax
    xorl %ecx, %ecx
    xorl %r11d, %r11d
    movq 88(%rdi), %rdi    /* rdi zuletzt: es zeigte auf den Frame */
    iretq

.section .bss
.balign 8
.global syscall_user_rsp
syscall_user_rsp:
    .skip 8

.section .note.GNU-stack,"",@progbits
