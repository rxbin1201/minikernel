/* Kontextwechsel zwischen Kernel-Threads. */

.section .text

/* void switch_context(uint64_t *old_rsp, uint64_t new_rsp)
 * Sichert die callee-saved Register auf dem alten Stack, merkt sich dessen RSP und laedt den des neuen Threads.
 * Reihenfolge der Pops muss zum initialen Stack-Frame in sched.c passen. */
.global switch_context
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* Einstieg eines neuen Threads: switch_context "kehrt" hierher zurueck, r12 = entry, r13 = arg. */
.global thread_trampoline
thread_trampoline:
    movq %r12, %rdi
    movq %r13, %rsi
    call thread_bootstrap
    ud2

.section .note.GNU-stack,"",@progbits
