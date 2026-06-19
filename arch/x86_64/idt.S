/*
 * idt.S - Interrupt Descriptor Table (entries filled at runtime via C)
 */
    .section .text
    .global load_idt

load_idt:
    lidt idt_desc(%rip)
    ret

    .align 16
    .global idt
idt:
    .space 256 * 16

    .align 4
idt_desc:
    .word 256 * 16 - 1
    .quad idt

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits
