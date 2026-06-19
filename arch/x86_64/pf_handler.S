/*
 * pf_handler.S - Page fault (#PF, vector 14) entry/exit stub (FIXED v2)
 *
 * Fixes:
 *   - Conditional swapgs: only swap if fault was in user mode
 *     (error code bit 2 = U/S). Kernel-mode faults skip swapgs.
 *   - pf_handler_c now handles COW faults (in pagetable.c).
 *     If it returns (COW resolved), we restore and iretq normally.
 *     If it panics, we never reach iretq.
 */
    .text
    .global pf_handler
    .type pf_handler,@function

    .extern pf_handler_c

pf_handler:
    /*
     * Check if fault was in user mode (error code bit 2 = U/S).
     * The error code is at the top of the stack (pushed last by CPU).
     * If kernel-mode fault, skip swapgs to avoid corrupting GS base.
     */
    testb $4, (%rsp)            /* test bit 2 (U/S) of error code */
    jz 1f                       /* kernel mode → skip swapgs */
    swapgs                      /* user mode → switch to kernel GS base */
1:
    push %rbp
    mov %rsp, %rbp

    /* Save caller-saved registers */
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11

    /*
     * Push error_code as first argument to pf_handler_c.
     * After 10 pushes (rbp + 9 regs = 80 bytes), error_code is at RSP+80.
     */
    mov 80(%rsp), %rdi
    /* Call C handler (may return for COW faults, or panic) */
    call pf_handler_c

    /* Restore registers */
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax

    leave

    /*
     * Conditional swapgs on exit: only swap back if fault was in user mode.
     * Re-check error code (still on stack under the pushed frame).
     * After leave (pop %rbp), RSP points to the CPU-pushed error code.
     */
    testb $4, (%rsp)            /* test bit 2 (U/S) of error code */
    jz 2f                       /* kernel mode → skip swapgs */
    swapgs                      /* user mode → restore user GS base */
2:
    iretq

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits
