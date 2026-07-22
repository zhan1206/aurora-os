/*
 * arch/riscv64/linker.ld - RISC-V 64-bit kernel linker script
 *
 * Entry point: _start (in boot.S)
 * Load address: 0x80200000 (typical OpenSBI/bbl load address)
 * Memory layout: supports QEMU virt machine with 128MB+ RAM
 */
ENTRY(_start)

SECTIONS
{
    . = 0x80200000;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(16);
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    . = . + 0x8000;  /* 32KB stack */
    _stack_top = .;

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.note.gnu.property)
        *(.comment)
    }
}