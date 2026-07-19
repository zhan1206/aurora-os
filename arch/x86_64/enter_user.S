    .text
    .global enter_user
    .type enter_user,@function

/*
 * FIXED (v4.1.8): Push a known-good RFLAGS value (0x202 = IF=1, bit 1 always 1)
 * instead of the kernel's current RFLAGS (pushfq).  Previously, pushfq would
 * copy the kernel's RFLAGS (IF=0) into the user context, leaving user mode
 * with interrupts disabled and causing deadlocks.  (BUG C-20 / H-2)
 */
/* enter_user(entry, stack_top) */
enter_user:
    /* arguments: rdi=entry, rsi=stack_top */
    /* push user SS, user RSP, RFLAGS, user CS, RIP then iretq */
    pushq $0x23       /* User SS */
    pushq %rsi        /* User RSP */
    pushq $0x202      /* User RFLAGS: IF=1 (interrupts enabled), IOPL=0 */
    pushq $0x1b       /* User CS (ring 3) */
    pushq %rdi        /* User RIP (entry point) */
    iretq

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits
