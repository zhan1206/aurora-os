/*
 * main.c - Kernel entry point with styled boot sequence
 *
 * Uses theme.h design tokens for all visual elements.
 * Layout follows AuroraOS Visual Aesthetics Design Specification §6.1.
 */
#include "include/print.h"
#include "include/kstdio.h"
#include "vfs.h"  /* FIXED (v4.4.2): BUILD-04 — for vfs_mount */
#include "include/theme.h"
#include "include/version.h"
#include "include/net.h"
#include "include/fat32.h"
#include "layout.h"
#include "mem.h"
#include "sched.h"
#include "include/pit.h"
#include "include/idt.h"
#include "include/log.h"
#include "console.h"
#include "shell.h"
#include "pagetable.h"
#include "syscall.h"
#include "fs.h"
#include "smp.h"
#include "acpi.h"
#include "block_dev.h"
#include "perf.h"
#include "sysctl.h"
#include "procfs.h"
#include "stack_protect.h"
#include "aslr.h"
#include "module.h"
#include "rtc.h"
#include "cmdline.h"
#include "drm.h"
#include "nvme.h"
#include "kgdb.h"
#include "pci.h"
#include "usb/xhci.h"
#include "usb/hid.h"
#include "../boot/boot_info.h"

/* ================================================================
 * Global Theme State (defined in theme.h)
 * ================================================================ */
int g_theme_mode      = THEME_DARK;
int g_reduced_motion  = 0;
int g_anim_enabled    = 1;

#define AURORA_COPY     "(c) 2026 AuroraOS Contributors — MIT License"

/* ================================================================
 * Boot banner — Aurora (northern lights) themed ASCII logo
 * ================================================================ */
static const char *logo[] = {
    "   .  *  ~  .  *  ~  .  *  ~  .  *  ~  .  *  ~  .",
    "  *                                               *",
    " ~         A   U   R   O   R   A   O  S          ~",
    "  *                                               *",
    "   ~  *  .  ~  *  .  ~  *  .  ~  *  .  ~  *  .  ~",
    NULL
};

static const char *logo_title[] = {
    "",
    "    A Self-Built x86_64 Operating System",
    NULL
};

static void boot_print_logo(void) {
    /* Top decorative line */
    console_write_ansi(BOOT_LOGO_SHADOW);
    console_draw_hr(SEP_DOT);
    console_write_ansi(SGR_RESET);
    console_vpad(1);

    /* Logo */
    for (int i = 0; logo[i]; ++i) {
        console_write_ansi(BOOT_LOGO_COLOR);
        console_write_centered(logo[i]);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }

    /* Subtitle */
    for (int i = 0; logo_title[i]; ++i) {
        console_write_ansi(CLR_MUTED);
        console_write_centered(logo_title[i]);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }

    console_vpad(1);

    /* Version / Build info in a compact box */
    console_write_ansi(BOOT_VERSION_COLOR);
    console_write_centered(AURORAOS_VERSION);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_write_ansi(BOOT_BUILD_COLOR);
    console_write_centered(AURORAOS_FULL_VERSION);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_write_ansi(CLR_MUTED);
    console_write_centered(AURORA_COPY);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_vpad(1);
    /* Bottom decorative line */
    console_write_ansi(BOOT_LOGO_SHADOW);
    console_draw_hr(SEP_DOT);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* ================================================================
 * Demo tasks
 * ================================================================ */
static void task_fn1(void) {
    for (int i = 0; i < 5; ++i) {
        log_printf(LOG_LEVEL_INFO, "task1 running iter=%d\n", i);
        yield();
    }
    do_exit_current(0);
}

static void task_fn2(void) {
    for (int i = 0; i < 5; ++i) {
        log_printf(LOG_LEVEL_INFO, "task2 running iter=%d\n", i);
        yield();
    }
    do_exit_current(0);
}

/* ================================================================
 * Bootstrap task — runs post-init code in a proper task context
 *
 * FIXED (v4.3.9): BOOT-06 — Previously kernel_main() called yield()
 * (which calls schedule()) directly from the idle task context.
 * The idle task should not call schedule() because it IS the idle
 * task.  Now the post-init work runs in a dedicated bootstrap task,
 * and kernel_main() enters a pure idle loop after creating it.
 * ================================================================ */
static void bootstrap_task(void) {
    /* === Phase 4: Self-test & Launch === */
    kernel_selftest();

    /* GUI (v4.2.6) — Create demo windows */
    {
        struct drm_window *shell_win = drm_window_create(50, 50, 400, 300, "Shell");
        struct drm_window *welcome_win = drm_window_create(200, 150, 350, 200, "Welcome");

        if (welcome_win) {
            void *fb = drm_window_get_fb(welcome_win);
            if (fb) {
                drm_fill_rect(fb, welcome_win->width * 4, 0, 0,
                              welcome_win->width, welcome_win->height, 0x003E2E2E);
                drm_fill_rect(fb, welcome_win->width * 4, 20, 30,
                              welcome_win->width - 40, 40, 0x002E3E5E);
                drm_fill_rect(fb, welcome_win->width * 4, 20, 90,
                              welcome_win->width - 40, 80, 0x003E2E4E);
            }
            drm_window_mark_dirty(welcome_win);
        }

        if (shell_win) {
            void *fb = drm_window_get_fb(shell_win);
            if (fb) {
                drm_fill_rect(fb, shell_win->width * 4, 0, 0,
                              shell_win->width, shell_win->height, 0x002E2E3E);
                drm_fill_rect(fb, shell_win->width * 4, 10, 10,
                              shell_win->width - 20, 20, 0x003E3E5E);
                drm_fill_rect(fb, shell_win->width * 4, 10, 40,
                              shell_win->width - 20, shell_win->height - 50, 0x002E3E3E);
            }
            drm_window_mark_dirty(shell_win);
        }

        if (welcome_win) drm_window_raise(welcome_win);

        drm_compositor_render();
        drm_compositor_swap();
    }

    /* Demo tasks */
    create_task(task_fn1);
    create_task(task_fn2);
    create_task(shell_main);

    /* Bootstrap task done — exit so the idle task takes over */
    do_exit_current(0);
}

/* ================================================================
 * kernel_main — Boot sequence with visual status reporting
 *
 * @magic:    Multiboot magic (0x2BADB002 for MB1, 0x36d76289 for MB2)
 * @mb_info:  Multiboot info structure pointer (physical address)
 * ================================================================ */
void kernel_main(uint32_t magic, void *mb_info) {
    /* Detect UEFI boot vs Multiboot boot */
    int is_uefi = (magic == UEFI_BOOT_MAGIC);
    struct uefi_boot_info *uefi_bi = (void *)0;
    if (is_uefi) {
        uefi_bi = (struct uefi_boot_info *)mb_info;
    }

    /* Early serial init — must be first for panic messages */
    serial_init();
    log_set_level(LOG_LEVEL_DEBUG);  /* Enable debug logging during boot */

    /* Stack protector must be initialized before any C code with stack arrays */
    stack_protect_init();

    /* Initialize kernel command line */
    cmdline_init("auroraos console=tty0 root=/dev/ram0 quiet");

    /* === Phase 1: Boot Splash Screen === */
    if (is_uefi && uefi_bi->fb_valid) {
        console_init_fb(uefi_bi->fb_addr, uefi_bi->fb_width, uefi_bi->fb_height,
                        uefi_bi->fb_pitch, uefi_bi->fb_bpp);
    } else {
        console_init();
    }
    console_clear();
    console_hide_cursor();

    /* Initialize DRM/KMS with UEFI GOP framebuffer if available */
    if (is_uefi && uefi_bi->fb_valid) {
        drm_init_gop((void *)(uintptr_t)uefi_bi->fb_addr,
                     uefi_bi->fb_width, uefi_bi->fb_height,
                     uefi_bi->fb_pitch, uefi_bi->fb_bpp);
    } else {
        drm_init();
    }

    /* GUI (v4.2.6) — Initialize compositor and input subsystem */
    drm_compositor_init();
    drm_input_init();

    /* Center logo vertically: 5 logo + 2 subtitle + 1 top-pad + 1 top-sep + 3 version + 1 bot-pad + 1 bot-sep = 15 */
    console_vcenter(15);
    boot_print_logo();

    /* Mirror logo to serial for remote verification */
    printk("\n");
    for (int i = 0; logo[i]; ++i) {
        printk(logo[i]);
        printk("\n");
    }
    printk(AURORAOS_VERSION);
    printk("\n");
    printk(AURORAOS_FULL_VERSION);
    printk("\n\n");

    console_show_cursor();

    /* === Phase 2: Hardware Initialization (with progress) === */
    console_write_ansi(BOOT_STAGE_LABEL);
    console_write_centered("Initializing system...");
    console_write_ansi(SGR_RESET);
    console_vpad(1);

    int boot_step = 0, boot_total = 25;  /* +3 for PCI, xHCI, HID (v4.2.6) */
    #define BOOT_STEP() do { \
        boot_step++; \
        console_write_ansi(BOOT_PROGRESS_FILL); \
        console_draw_progress_bar_styled(boot_step * 100 / boot_total, 40, \
            BOOT_PROGRESS_FILL, BOOT_PROGRESS_BG, CLR_MUTED, BOOT_PROGRESS_FILL); \
        console_write_ansi(SGR_RESET); \
        console_putc('\n'); \
    } while(0)

    console_status_ok("Serial port (COM1 115200 8N1)");
    BOOT_STEP();

    /* Physical memory — auto-detects Multiboot1 or Multiboot2 */
    uint64_t mem_mb = 64;
    if (is_uefi) {
        phys_mem_init_uefi(uefi_bi);
    } else {
        phys_mem_init(mb_info);
    }
    {
        uint64_t total, free, used;
        mem_get_stats(&total, &free, &used);
        mem_mb = total / (1024 * 1024);
    }
    console_write_ansi(BOOT_OK_FG);
    console_write(STATUS_OK_STR);
    console_write_ansi(SGR_RESET);
    console_write(" Physical memory: ");
    {
        char buf[16];
        uitoa(mem_mb, buf, sizeof(buf));
        console_write(buf);
        console_write(" MiB\n");
    }
    BOOT_STEP();

    slab_init();
    console_status_ok("Slab allocator (8 size classes)");
    BOOT_STEP();

    aslr_init();
    console_write_ansi(BOOT_OK_FG);
    console_write(STATUS_OK_STR);
    console_write_ansi(SGR_RESET);
    console_putc(' ');
    console_write("ASLR initialized (");
    console_write(aslr_prng_name());
    console_write(")");
    console_putc('\n');
    BOOT_STEP();

    /*
     * FIXED (v4.1.9): Initialize KASLR after ASLR is ready.
     * KASLR uses the ChaCha20 CSPRNG to randomize the kernel heap
     * base address and module load addresses.  (H-30: KASLR)
     */
    kaslr_init();
    console_write_ansi(BOOT_OK_FG);
    console_write(STATUS_OK_STR);
    console_write_ansi(SGR_RESET);
    console_putc(' ');
    console_write("KASLR initialized (slide=");
    {
        char slide_buf[32];
        uint64_t slide = kaslr_get_slide();
        console_write("0x");
        for (int nib = 60; nib >= 0; nib -= 4) {
            int val = (int)((slide >> nib) & 0xF);
            slide_buf[15 - nib/4] = (char)(val < 10 ? '0' + val : 'a' + val - 10);
        }
        slide_buf[16] = '\0';
        console_write(slide_buf);
    }
    console_write(")");
    console_putc('\n');
    BOOT_STEP();

    page_table_init();
    console_status_ok("Page tables (4-level, NX enabled)");
    BOOT_STEP();

    /*
     * Initialize ACPI subsystem after memory and page tables are ready.
     * ACPI discovers RSDP, parses RSDT/XSDT, caches MADT and FADT.
     * This must be called before smp_init() which uses MADT data.
     */
    acpi_init();
    console_status_ok("ACPI (MADT + FADT parsed)");
    BOOT_STEP();

    printk_console_ready();

    /* === Phase 3: Kernel Subsystems ===
     *
     * FIXED (v4.3.9): BOOT-09 — IDT must be initialized before any STI.
     * The boot sequence is:
     *   1. scheduler_init() — no STI
     *   2. syscall_init()   — no STI
     *   3. irq_init()       — sets up IDT gates, load_idt(), PIC remap,
     *                         then STI at the end.  This is the FIRST
     *                         STI in the kernel boot sequence.
     *   4. smp_init()       — after IDT is ready
     *   5. pit_init()       — after IDT is ready
     *
     * All STI instructions in sched.c (sti;hlt paths) are only reached
     * after irq_init() has enabled interrupts.  No STI exists in the
     * boot entry (entry.S) or early init code. */
    printk("scheduler_init...\n");
    scheduler_init();
    printk("scheduler_init done\n");
    console_status_ok("Scheduler (RR + idle task + PID bitmap)");
    BOOT_STEP();

    printk("syscall_init...\n");
    syscall_init();
    printk("syscall_init done\n");
    console_status_ok("SYSCALL/SYSRET MSRs configured");
    BOOT_STEP();

    printk("irq_init...\n");
    irq_init();
    printk("irq_init done\n");
    console_status_ok("IDT + PIC remap + keyboard driver");
    BOOT_STEP();

    /*
     * Initialize SMP after IRQ system is ready.
     * This parses ACPI MADT, detects CPUs, and starts APs.
     * Falls back to single-CPU mode if no ACPI/MADT found.
     */
    printk("smp_init...\n");
    smp_init(mb_info);
    printk("smp_init done\n");
    console_status_ok("SMP (detected CPUs)");
    BOOT_STEP();

    /*
     * rodata_protect must be called AFTER irq_init() so that the IDT
     * is set up to handle any page faults that may occur during page
     * table modification (e.g., TLB shootdown, split_huge_page).
     * Without the IDT, a page fault during split_huge_page would
     * cause a triple fault and silent hang.
     */
    printk("rodata_protect...\n");
    rodata_protect();
    printk("rodata_protect done\n");
    console_status_ok("Read-only data segment");
    BOOT_STEP();

    /* KGDB (v4.2.6) - Initialize kernel debugger after IDT is set up */
    kgdb_init();
    console_status_ok("KGDB v4.2.6 (breakpoints, single-step, backtrace)");
    BOOT_STEP();

    pit_init(100);
    console_status_ok("PIT timer (100 Hz)");
    BOOT_STEP();

    rtc_init();
    console_status_ok("CMOS RTC driver");
    BOOT_STEP();

    perf_init();
    console_status_ok("Performance counters");
    BOOT_STEP();

    sysctl_init();
    console_status_ok("Sysctl interface");
    BOOT_STEP();

    /* PCI enumeration - must be called before any PCI driver */
    pci_init();
    console_status_ok("PCI bus enumeration");
    BOOT_STEP();

    nvme_init();
    console_status_ok("NVMe driver (PCI enumeration)");
    BOOT_STEP();

    /* USB (v4.2.6) - xHCI controller and HID driver */
    xhci_init();
    console_status_ok("xHCI USB controller");
    BOOT_STEP();

    hid_init();
    console_status_ok("USB HID driver (keyboard/mouse)");
    BOOT_STEP();

    /* === Phase 3.5: File System === */
    ramdisk_init(0);  /* 16 MiB RAM disk for testing */
    fs_init();
    console_status_ok("VFS + RamFS mounted");
    BOOT_STEP();

    module_init();
    console_status_ok("Module loader (kernel symbol table)");
    BOOT_STEP();

    /* === Phase 3.6: Network Stack === */
    net_init();
    console_status_ok("TCP/IP network stack (ARP/IPv4/ICMP/UDP/TCP)");
    BOOT_STEP();

    /* === Phase 3.7: FAT32 Filesystem === */
    {
        struct block_device *fat32_bdev = block_dev_find("ramdisk0");
        if (fat32_bdev) {
            struct super_block *fat32_sb = fat32_mount(fat32_bdev);
            if (fat32_sb) {
                /*
                 * FIXED (v4.1.8): Check vfs_mount() return value.
                 * If mount fails, free the superblock to prevent
                 * memory leak. (C-6: mount error path memory leak)
                 */
                if (vfs_mount("/fat32", fat32_sb) < 0) {
                    log_printf(LOG_LEVEL_WARN, "FAT32 mount at /fat32 failed\n");
                    kfree(fat32_sb);
                    console_status_ok("FAT32 mount failed");
                } else {
                    console_status_ok("FAT32 filesystem mounted at /fat32");
                }
            } else {
                console_status_ok("FAT32 mount skipped (not a FAT32 image)");
            }
        } else {
            console_status_ok("FAT32 mount skipped (no block device)");
        }
    }
    BOOT_STEP();

    #undef BOOT_STEP

    /* Boot complete indicator */
    console_vpad(1);
    console_write_ansi(BOOT_OK_FG);
    console_write_centered("[ System Ready ]");
    console_write_ansi(SGR_RESET);
    console_vpad(2);

    /* Separator */
    console_draw_hr(SEP_LINE);
    console_putc('\n');

    /* FIXED (v4.3.9): BOOT-06 — Create a bootstrap task instead of
     * calling schedule() directly from the idle task context.
     * The bootstrap task runs selftest, creates GUI windows, and
     * launches demo tasks.  kernel_main() enters the idle loop. */
    create_task(bootstrap_task);

    /* FIXED (v4.3.9): BOOT-06 — Idle loop: call schedule() to yield to
     * runnable tasks, then HLT with interrupts enabled.  When the timer
     * interrupt fires, schedule_tick() sets need_resched, and the
     * interrupt return path calls schedule() via check_resched(). */
    while (1) {
        schedule();
        asm volatile ("sti; hlt" ::: "memory");
    }
}
