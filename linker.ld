/*
 * linker.ld - AuroraOS kernel linker script
 */
ENTRY(start)

SECTIONS
{
    . = 0x100000;

    .text : {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    /*
     * .data must be page-aligned because entry.S places page tables
     * (pml4/pdpt/pd) in .data with .align 4096.
     * Without ALIGN here, the alignment directive in entry.S
     * cannot guarantee 4KiB alignment.
     */
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

    /*
     * FIXED (v4.3.2): BSS-001 — Stack moved OUT of .bss to its own section.
     * Previously the 32KB kernel stack was in .bss, and stack overflow
     * during selftest (which has multiple 1024-byte local buffers) would
     * corrupt BSS globals (current, idle_task, kernel_cr3), causing:
     *   BUG-CURRENT-NULL: current/idle_task zeroed, scheduler dead
     *   BUG-CR3-CACHE:   kernel_cr3 corrupted, must re-read CR3
     * Now the stack is in its own .stack section AFTER .bss, with a
     * guard page (4KB unmapped) and stack canary at the bottom.
     * Stack size increased from 32KB to 64KB to handle selftest depth.
     */
    . = ALIGN(4096);
    __guard_page = .;
    . += 4096;   /* unmapped guard page (4KB) */
    .stack : {
        __stack_bottom = .;
        . += 65536;  /* 64KB kernel stack */
        __stack_top = .;
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.note.gnu.property)
        *(.comment)
    }
}
