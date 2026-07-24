/*
 * arch/aarch64/linker.ld - AArch64 kernel linker script
 *
 * Entry point: _start (in boot.S)
 * Load address: 0x40080000 (typical QEMU virt machine)
 * Memory layout: supports QEMU virt machine with 128MB+ RAM
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

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
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
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