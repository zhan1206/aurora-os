/*
 * irq_handler.S - IRQ0 (PIT timer) entry/exit stub (FIXED v3)
 *
 * This handler runs in KERNEL context (interrupts kernel code).
 * Do NOT use swapgs — that is only for user↔kernel transitions
 * via syscall/sysret or iretq from user mode.
 *
 * Fixes v3:
 *   - Removed incorrect swapgs (caused triple fault in kernel context)
 *   - EOI to master PIC only (IRQ0 is on master)
 *   - need_resched flag set by C handler, checked at safe points
 */
    .text
    .global irq0_handler
    .type irq0_handler,@function

    .extern pit_irq_c_handler

irq0_handler:
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

    /* Call C handler (sets need_resched flag) */
    mov %rsp, %rdi
    call pit_irq_c_handler

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

    /* Send EOI to master PIC */
    mov $0x20, %al
    outb %al, $0x20

    iretq

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits
