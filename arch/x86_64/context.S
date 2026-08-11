    .text
    .global context_switch
    .type context_switch,@function

/*
 * context_switch(old_rsp_ptr, new_rsp)
 * old_rsp_ptr: pointer to where to store current RSP (uint64_t*)
 * new_rsp: new RSP value to load
 */
context_switch:
    /* FIXED (v4.3.9): BOOT-08 — Remove pushfq/popfq from context_switch.
     * Rely on the caller's interrupt state. The task frame already has
     * rflags at the correct position, pushed by the caller or set at
     * task creation time. pushfq/popfq would corrupt the interrupt flag
     * state if the caller had interrupts disabled. */

    /* Save registers callee-saved (rbx, rbp, r12-r15) */
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    /* Save current stack pointer into *old_rsp_ptr */
    mov %rdi, %rax
    mov %rsp, (%rax)
    /* Load new stack pointer */
    mov %rsi, %rsp

    /* Restore registers from new stack (pop in reverse) */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp

    /* Return to new context (ret will pop RIP from new stack) */
    ret

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits
