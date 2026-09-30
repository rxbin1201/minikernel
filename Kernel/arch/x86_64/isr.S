/* Exception-/Interrupt-Stubs fuer alle 256 Vektoren. Fuer Vektoren ohne CPU-Fehlercode wird 0
 * gepusht, damit der InterruptFrame immer gleich aussieht. */

.altmacro

.section .text

.macro ISR n
isr\n:
    /* Vektoren mit Fehlercode: 8, 10-14, 17, 21, 29, 30 */
    .if !((\n == 8) | ((\n >= 10) & (\n <= 14)) | (\n == 17) | (\n == 21) | (\n == 29) | (\n == 30))
    pushq $0
    .endif
    pushq $\n
    jmp isr_common
.endm

.macro ISR_STUB n
    ISR %n
.endm

.set i, 0
.rept 256
    ISR_STUB i
    .set i, i + 1
.endr

isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi        /* InterruptFrame* (SysV: 1. Argument) */
    cld
    call isr_handler       /* rsp ist hier 16-Byte-ausgerichtet */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp         /* Vektor + Fehlercode entfernen */

    /* Rueckkehr nach Ring 3: Selektoren im Frame auf die bekannten User-Werte setzen. Der Kernel bestimmt sie ohnehin
     * (Frame: rip, cs, rflags, rsp, ss ab rsp); so haengt der iretq nicht davon ab, was die CPU beim Eintritt gepusht hat. */
    testb $3, 8(%rsp)
    jz 1f
    movq $0x23, 8(%rsp)
    movq $0x1B, 32(%rsp)
1:
    iretq

.macro ISR_ENTRY n
    .quad isr\n
.endm

.section .rodata
.balign 8
.global isr_stub_table
isr_stub_table:
.set i, 0
.rept 256
    ISR_ENTRY %i
    .set i, i + 1
.endr

.section .note.GNU-stack,"",@progbits
