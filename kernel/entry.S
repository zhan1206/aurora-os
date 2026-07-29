/*
 * entry.S - Multiboot1 entry point for 64-bit kernel
 *
 * Uses Multiboot1 (not Multiboot2) for maximum GRUB compatibility.
 * GRUB loads us in 32-bit protected mode, we switch to long mode.
 */
.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000003   /* ALIGN + MEMINFO */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot
.align 8
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM

    /* A.OUT kludge fields (unused for ELF) */
    .long 0  /* header_addr */
    .long 0  /* load_addr */
    .long 0  /* load_end_addr */
    .long 0  /* bss_end_addr */
    .long 0  /* entry_addr */

/* FIXED (v4.3.3): MB2-001 — Add Multiboot2 header alongside Multiboot1.
 * Previously only Multiboot1 was used even though code claimed MB2 support.
 * Now adds proper Multiboot2 header for GRUB2 compatibility. */
.align 8
# Multiboot2 header
mb2_header_start:
    .long 0xE85250D6          # magic
    .long 0                    # architecture: i386
    .long mb2_header_end - mb2_header_start  # header_length
    .long -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start))  # checksum

    # Address tag
    .align 8
    .word 2                    # type: address
    .word 0                    # flags
    .long 24                   # size
    .long mb2_header_start     # header_addr
    .long 0x100000             # load_addr
    .long 0                    # load_end_addr (0 = entire file)
    .long 0                    # bss_end_addr

    # Entry tag
    .align 8
    .word 3                    # type: entry
    .word 0                    # flags
    .long 12                   # size
    .long start                # entry_addr (Multiboot2 entry)

    # End tag
    .align 8
    .word 0                    # type: end
    .word 0                    # flags
    .long 8                    # size
mb2_header_end:

.section .text
.code32
.global start
.type start,@function
start:
    cli
    cld

    /* Save Multiboot info */
    mov %eax, %esi               /* magic */
    mov %ebx, %edi               /* multiboot_info pointer */

    /* Set up temporary 32-bit stack */
    lea stack_top, %esp

    /* Check if CPU supports CPUID */
    pushf
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax
    push %eax
    popf
    pushf
    pop %eax
    xor %ecx, %eax
    jz no_long_mode

    /* Check extended features for long mode */
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode

    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx       /* LM bit */
    jz no_long_mode

    /* Set up 4-level paging: PML4[0] → PDPT[0] → PD[0] (2MB pages, 0-1GB) */
    /* Clear page tables */
    xor %eax, %eax
    mov $pml4, %edi
    mov $((4096*3)/4), %ecx
    rep stosl

    /* PML4[0] = pdpt | PRESENT | RW */
    movl $(pdpt + 0x003), pml4

    /* PDPT[0] = pd | PRESENT | RW */
    movl $(pd + 0x003), pdpt

    /* PD: 512 entries × 2MB pages = 1GB identity mapping */
    mov $pd, %edi
    mov $0x00000083, %eax        /* 2MB page, PRESENT, RW, PS */
    mov $512, %ecx
1:  stosl
    movl $0, (%edi)               /* upper 32 bits = 0 */
    add $4, %edi
    add $0x200000, %eax          /* next 2MB */
    loop 1b

    /* Enable PAE */
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    /* Load PML4 */
    mov $pml4, %eax
    mov %eax, %cr3

    /* Enable Long Mode in EFER */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr

    /* Enable paging */
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    /* Load 64-bit GDT */
    lgdt gdt64_desc

    /* Far jump to 64-bit code */
    ljmp $0x08, $long_mode_start

no_long_mode:
    /* Write 'ERR' to VGA */
    movl $0x0E45, 0xB8000       /* 'E' */
    movl $0x0E52, 0xB8002       /* 'R' */
    movl $0x0E52, 0xB8004       /* 'R' */
    cli
1:  hlt
    jmp 1b

.code64
long_mode_start:
    /* Set up segment registers for 64-bit mode */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Enable SSE/FPU (required for -O2 compiled code which uses SSE) */
    mov %cr0, %rax
    and $~0x4, %rax              /* Clear CR0.EM (Emulation) */
    or  $0x2, %rax               /* Set CR0.MP (Monitor Coprocessor) */
    mov %rax, %cr0
    mov %cr4, %rax
    or  $0x600, %rax             /* Set CR4.OSFXSR | CR4.OSXMMEXCPT */
    mov %rax, %cr4
    fninit                        /* Initialize x87 FPU */

    /* 
 * FIXED (v4.1.8): Zero BSS section before calling kernel_main.
 * Previously, bss_start and bss_end pointed to the same address
 * (both defined right next to each other in entry.S), so BSS was
 * never cleared.  This caused uninitialized static variables to
 * contain garbage values.  (BUG C-17 / H-4)
 *
 * Now uses linker-defined __bss_start and __bss_end symbols.
 */
    movabs $__bss_start, %rdi
    movabs $__bss_end, %rcx
    sub %rdi, %rcx                /* rcx = BSS size in bytes */
    jz 2f
    xor %eax, %eax
    shr $3, %rcx                  /* convert to qwords */
1:  mov %rax, (%rdi)
    add $8, %rdi
    loop 1b
2:

    /* Set up 64-bit stack */
    movabs $__stack_top, %rsp

    /* FIXED (v4.3.2): BSS-001 — Write stack canary at bottom of kernel stack.
     * __stack_bottom is a linker-defined symbol at the bottom of the .stack
     * section.  We write a magic value there so that sched_init() can detect
     * stack overflow.  If the canary is ever overwritten, we know the stack
     * overflowed past its 64KB boundary. */
    movabs $__stack_bottom, %rdi
    movabs $0xDEAD0000BEEFCAFE, %rax
    mov %rax, (%rdi)

    /* Zero-extend saved 32-bit values */
    mov %esi, %esi               /* magic */
    mov %ebx, %ebx               /* multiboot_info (FIXED: %edi was clobbered in 32-bit code) */

    /* Pass arguments to kernel_main */
    mov %rsi, %rdi               /* magic */
    mov %rbx, %rsi               /* multiboot info (FIXED: was %rdi which was overwritten) */

    call kernel_main

    cli
1:  hlt
    jmp 1b

.section .data
.align 4096
pml4:
    .space 4096
pdpt:
    .space 4096
pd:
    .space 4096

.align 16
gdt64:
    .quad 0                      /* null */
    .quad 0x00AF9A000000FFFF     /* kernel code, 64-bit */
    .quad 0x00AF92000000FFFF     /* kernel data */
gdt64_end:

gdt64_desc:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

.section .bss
.global bss_start, bss_end
/* FIXED (v4.3.2): BSS-001 — Stack moved to linker-defined .stack section.
 * The old 32KB stack allocation in .bss has been removed.  Stack is now
 * defined in linker.ld as a 64KB .stack section AFTER .bss with a guard
 * page.  This prevents stack overflow from corrupting BSS globals.
 * bss_start/bss_end are kept for backward compatibility (they were
 * already unused after v4.1.8). */
bss_start:
bss_end:

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits
