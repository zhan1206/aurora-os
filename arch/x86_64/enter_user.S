    .text
    .global enter_user
    .type enter_user,@function

/* enter_user(entry, stack_top) */
enter_user:
    /* arguments: rdi=entry, rsi=stack_top */
    /* push user SS, user RSP, RFLAGS, user CS, RIP then iretq */
    /* do NOT change %rsp here; iretq will load user RSP from stack */
    pushq $0x23
    pushq %rsi
    pushfq
    pushq $0x1b
    pushq %rdi
    iretq

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits
