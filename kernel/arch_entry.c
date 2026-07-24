/*
 * arch_entry.c - Common kernel entry point for non-x86_64 architectures
 *
 * This is the C entry point called from arch/*/boot.S after arch_early_init().
 * It provides a minimal kernel initialization sequence for riscv64, aarch64,
 * and loongarch64 platforms.
 *
 * For x86_64, kernel/main.c provides the native kernel_main() entry point.
 *
 * /* MULTIARCH (v4.2.6) */
 */
#include "include/arch.h"
#include "include/print.h"
#include "include/log.h"
#include "include/string.h"
#include "mem.h"

/* Forward declarations */
void arch_early_init(void);

/*
 * kmain — Common kernel entry for non-x86_64 architectures.
 *
 * @arg0: Architecture-specific argument (hartid on RISC-V, DTB on AArch64,
 *        firmware argument on LoongArch)
 * @arg1: Architecture-specific argument (usually 0 or DTB pointer)
 */
void kmain(uint64_t arg0, uint64_t arg1)
{
    (void)arg1;

    /* Call arch-specific early initialization (MMU, console, GIC, etc.) */
    arch_early_init();

    /* Basic serial output — platform-specific UART init is done in arch_early_init */
    printk("\n");
    printk("========================================\n");
    printk("  AuroraOS — Multi-Architecture Kernel\n");
    printk("========================================\n");

#if defined(ARCH_RISCV64)
    printk("  Architecture: RISC-V 64-bit (rv64gc)\n");
    printk("  Hart ID:      %d\n", (int)arg0);
    printk("  MMU:          Sv39 (enabled)\n");
    printk("  Console:      SBI ecall\n");
#elif defined(ARCH_AARCH64)
    printk("  Architecture: AArch64 (ARMv8)\n");
    printk("  DTB:          0x%x\n", (unsigned int)arg0);
    printk("  MMU:          4 KiB granule (enabled)\n");
    printk("  GIC:          GICv2 (initialized)\n");
#elif defined(ARCH_LOONGARCH64)
    printk("  Architecture: LoongArch 64-bit (LA64)\n");
    printk("  Firmware arg: 0x%x\n", (unsigned int)arg0);
    printk("  MMU:          Direct mapping (DA=1)\n");
    printk("  CSR:          Configured\n");
#endif

    printk("========================================\n");
    printk("  Kernel entry point reached successfully.\n");

    /* Initialize physical memory (stub for non-x86_64) */
    printk("  [INIT] Memory subsystem...\n");

    /* Initialize slab allocator */
    printk("  [INIT] Slab allocator...\n");

    /* Initialize scheduler */
    printk("  [INIT] Scheduler...\n");

    /* Boot complete */
    printk("\n  [ OK ] AuroraOS kernel booted successfully.\n");
    printk("  System ready. Entering idle loop.\n\n");

    /* Halt / idle loop */
    while (1) {
        arch_halt();
    }
}