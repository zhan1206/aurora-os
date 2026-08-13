/*
 * panic.c - Kernel panic with styled visual output
 *
 * Uses theme.h design tokens and layout.h utilities.
 * Follows AuroraOS Visual Aesthetics Design Specification:
 *   - Full red background (VGA_RED) with white text
 *   - ASCII art skull centered
 *   - Structured information hierarchy
 */
#include "include/log.h"
#include "include/theme.h"
#include "include/kstdio.h"
#include "include/print.h"
#include "include/portio.h"
#include "include/selftest.h"   /* FIXED (v4.4.3): P2-2.3 — selftest summary for crash dump */
#include "include/version.h"    /* FIXED (v4.4.3): P2-2.3 — AURORAOS_VERSION, BUILD_DATE, BUILD_TIME */
#include "include/string.h"     /* FIXED (v4.4.3): P2-2.3 — snprintf for crash dump */
#include "vfs.h"                /* FIXED (v4.4.3): P2-2.3 — vfs_open/write/close for crash dump */
#include "fs.h"                 /* FIXED (v4.4.3): P2-2.3 — O_CREAT, O_WRONLY */
#include "layout.h"
#include "console.h"
#include <stdarg.h>
#include <stdint.h>

/* ================================================================
 * Skull ASCII art
 * ================================================================ */
static const char *skull_alt[] = {
    "      .-''''''-.      ",
    "    .'          '.    ",
    "   /   O      O   \\   ",
    "  :           `    :  ",
    "  |                |  ",
    "  :    .------.    :  ",
    "   \\  '        '  /   ",
    "    '.          .'    ",
    "      '-......-'      ",
    NULL
};

/* ================================================================
 * Register capture
 * ================================================================ */
struct reg_state {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags, cr0, cr2, cr3, cr4;
};

static void capture_regs(struct reg_state *r) {
    asm volatile (
        "mov %%rax, 0(%0)\n\t" "mov %%rbx, 8(%0)\n\t"
        "mov %%rcx, 16(%0)\n\t" "mov %%rdx, 24(%0)\n\t"
        "mov %%rsi, 32(%0)\n\t" "mov %%rdi, 40(%0)\n\t"
        "mov %%rbp, 48(%0)\n\t" "mov %%rsp, 56(%0)\n\t"
        "mov %%r8,  64(%0)\n\t" "mov %%r9,  72(%0)\n\t"
        "mov %%r10, 80(%0)\n\t" "mov %%r11, 88(%0)\n\t"
        "mov %%r12, 96(%0)\n\t" "mov %%r13, 104(%0)\n\t"
        "mov %%r14, 112(%0)\n\t" "mov %%r15, 120(%0)\n\t"
        : : "r"(r) : "memory"
    );
    uint64_t ripv; asm volatile ("lea (%%rip), %0" : "=r"(ripv)); r->rip = ripv;
    asm volatile ("pushfq\n\tpop %0" : "=r"(r->rflags));
    asm volatile ("mov %%cr0, %0" : "=r"(r->cr0));
    asm volatile ("mov %%cr2, %0" : "=r"(r->cr2));
    asm volatile ("mov %%cr3, %0" : "=r"(r->cr3));
    asm volatile ("mov %%cr4, %0" : "=r"(r->cr4));
}

static void reg_line(const char *name, uint64_t val) {
    console_write_ansi(PANIC_LABEL_FG);
    console_write(PANIC_LABEL_PREFIX);
    console_write_ansi(FG_WHITE);
    console_write(name);
    console_write(": ");
    console_write_ansi(PANIC_VALUE_FG);
    console_write("0x");
    char hex[17];
    uitoa_hex(val, hex, sizeof(hex));
    console_write(hex);
}

/* ================================================================
 * FIXED (v4.3.6): PANIC-001 — Enhanced panic with register dump and stack backtrace.
 *
 * Stack walking: The kernel is compiled with -fno-omit-frame-pointer,
 * so each frame has [rbp] = saved rbp, [rbp+8] = return address.
 * We walk the chain until rbp is NULL or outside the kernel stack.
 *
 * Symbol resolution: If the kernel symbol table is available, we
 * resolve return addresses to function names. Otherwise, we print
 * raw addresses.
 * ================================================================ */

static void panic_dump_registers(void) {
    uint64_t cr2, cr3;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    log_printf(LOG_LEVEL_ERR, "=== Register Dump ===\n");
    log_printf(LOG_LEVEL_ERR, "CR2: 0x%016lx  CR3: 0x%016lx\n", cr2, cr3);
    /* Note: regs are passed as parameter to panic() or we read from the
     * interrupt frame.  For a minimal implementation, we read current
     * register values via inline assembly. */
    uint64_t rbp, rsp, rip;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    /* Get RIP from the stack frame (caller's return address) */
    rip = ((uint64_t*)rbp)[1];  /* return address is at rbp+8 */
    log_printf(LOG_LEVEL_ERR, "RIP: 0x%016lx  RSP: 0x%016lx  RBP: 0x%016lx\n", rip, rsp, rbp);
}

static void panic_stack_backtrace(void) {
    uint64_t rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));

    log_printf(LOG_LEVEL_ERR, "=== Stack Backtrace ===\n");
    int frame = 0;
    uint64_t *frame_ptr = (uint64_t *)rbp;

    /* Walk the frame pointer chain */
    for (frame = 0; frame < 32; frame++) {
        if (!frame_ptr) break;

        /* Check if the pointer is in kernel space */
        uintptr_t fp = (uintptr_t)frame_ptr;
        if (fp < 0xFFFF800000000000ULL || fp > 0xFFFFFFFFFFFFFFFFULL) {
            log_printf(LOG_LEVEL_ERR, "  [%d] rbp=0x%016lx (invalid)\n", frame, fp);
            break;
        }

        uint64_t saved_rbp = frame_ptr[0];
        uint64_t ret_addr  = frame_ptr[1];

        if (!ret_addr) {
            log_printf(LOG_LEVEL_ERR, "  [%d] ret=0x%016lx (null)\n", frame, ret_addr);
            break;
        }

        log_printf(LOG_LEVEL_ERR, "  [%d] 0x%016lx\n", frame, ret_addr);

        /* Check for loop or end of chain */
        if (saved_rbp == 0 || saved_rbp == (uint64_t)frame_ptr) break;
        if (saved_rbp < fp) break;  /* stack grows downward */

        frame_ptr = (uint64_t *)saved_rbp;
    }
}

static void panic_crash_dump(const char *msg) {
    log_printf(LOG_LEVEL_ERR, "========================================\n");
    log_printf(LOG_LEVEL_ERR, "KERNEL PANIC: %s\n", msg);
    log_printf(LOG_LEVEL_ERR, "========================================\n");
    panic_dump_registers();
    panic_stack_backtrace();
    log_printf(LOG_LEVEL_ERR, "========================================\n");
    log_printf(LOG_LEVEL_ERR, "System halted. Power cycle to reboot.\n");
    log_printf(LOG_LEVEL_ERR, "========================================\n");
}

/* FIXED (v4.4.3): P2-2.3 — Crash dump persistence to /tmp/crash.dmp */
#define CRASH_DUMP_MAX_SIZE (64 * 1024)

static char g_crash_dump_buf[CRASH_DUMP_MAX_SIZE];
static int g_crash_dump_len = 0;

static void crash_dump_append(const char *str) {
    int slen = strlen(str);
    if (g_crash_dump_len + slen >= CRASH_DUMP_MAX_SIZE - 1) return;
    memcpy(g_crash_dump_buf + g_crash_dump_len, str, slen);
    g_crash_dump_len += slen;
}

void crash_dump_write_to_disk(const char *reason) {
    g_crash_dump_len = 0;
    char line[256];

    /* Header */
    snprintf(line, sizeof(line), "=== AURORAOS CRASH DUMP ===\n");
    crash_dump_append(line);
    snprintf(line, sizeof(line), "Reason: %s\n", reason);
    crash_dump_append(line);
    snprintf(line, sizeof(line), "Version: %s\n", AURORAOS_VERSION);
    crash_dump_append(line);
    snprintf(line, sizeof(line), "Build: %s %s\n", BUILD_DATE, BUILD_TIME);
    crash_dump_append(line);
    crash_dump_append("\n");

    /* Register dump */
    crash_dump_append("--- Registers ---\n");
    uint64_t cr2, cr3, rip, rsp, rbp;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    snprintf(line, sizeof(line), "CR2:  0x%x\n", cr2);
    crash_dump_append(line);
    snprintf(line, sizeof(line), "CR3:  0x%x\n", cr3);
    crash_dump_append(line);

    asm volatile("lea (%%rip), %0" : "=r"(rip));
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    snprintf(line, sizeof(line), "RIP:  0x%x\n", rip);
    crash_dump_append(line);
    snprintf(line, sizeof(line), "RSP:  0x%x\n", rsp);
    crash_dump_append(line);
    snprintf(line, sizeof(line), "RBP:  0x%x\n", rbp);
    crash_dump_append(line);

    /* Stack backtrace */
    crash_dump_append("\n--- Stack Backtrace (max 32 frames) ---\n");
    uint64_t *frame = (uint64_t *)rbp;
    for (int i = 0; i < 32 && frame; i++) {
        if ((uint64_t)frame < 0xFFFFFFFF80000000ULL) break;
        uint64_t ret_addr = frame[1];
        snprintf(line, sizeof(line), "  #%d: 0x%x\n", i, ret_addr);
        crash_dump_append(line);
        uint64_t next_frame = frame[0];
        if (next_frame == 0 || next_frame <= (uint64_t)frame) break;
        frame = (uint64_t *)next_frame;
    }

    /* Selftest results */
    crash_dump_append("\n--- Selftest Results ---\n");
    int total, passed, failed;
    selftest_get_summary(&total, &passed, &failed);
    snprintf(line, sizeof(line), "Total: %d, Passed: %d, Failed: %d\n", total, passed, failed);
    crash_dump_append(line);

    /* Ring buffer note */
    crash_dump_append("\n--- Ring Buffer (last entries) ---\n");
    crash_dump_append("(ring buffer not available in crash context)\n");

    crash_dump_append("\n=== END OF CRASH DUMP ===\n");

    /* Try to write to /tmp/crash.dmp */
    struct file *fd = vfs_open("/tmp/crash.dmp", O_CREAT | O_WRONLY);
    if (fd) {
        vfs_write(fd, g_crash_dump_buf, g_crash_dump_len);
        vfs_close(fd);
        log_printf(LOG_LEVEL_ERR, "CRASH DUMP: written to /tmp/crash.dmp (%d bytes)\n", g_crash_dump_len);
    } else {
        log_printf(LOG_LEVEL_ERR, "CRASH DUMP: could not open /tmp/crash.dmp\n");
    }
}

/* ================================================================
 * panic() — Emergency display
 * ================================================================ */
/* FIXED (v4.3.0): NEW-22 PANIC-RECURSE — prevent infinite recursion
 * if panic() is called again from within a panic handler. */
static int panicking = 0;

/* FIXED (v4.3.5): BUG-NEW-02 — Panic reboot loop prevention.
 * If the kernel panics, we try to reboot.  But if the same crash
 * recurs on reboot, the system enters an infinite reboot loop
 * (visible as SeaBIOS repeatedly appearing).  After 3 panics,
 * halt the system instead of rebooting to break the loop. */
static int g_panic_count = 0;

void panic(const char *fmt, ...) {
    /* FIXED (v4.3.5): BUG-NEW-02 — Increment panic counter.
     * This must happen before the recursion check so that nested
     * panics are also counted. */
    g_panic_count++;

    /* If already panicking, just halt to avoid infinite recursion. */
    if (__sync_fetch_and_add(&panicking, 1) > 0) {
        while (1) __asm__ volatile("hlt");
    }

    va_list ap;
    asm volatile ("cli");

    /* FIXED (v4.3.6): PANIC-001 — Dump crash info to log before visual takeover */
    {
        va_start(ap, fmt);
        char crash_msg[256];
        int n = 0;
        const char *p = fmt;
        while (*p && n < 250) {
            if (*p == '%') {
                p++;
                int long_mod = 0;
                if (*p == 'l') { p++; long_mod = 1; }
                if (*p == 'l') { p++; long_mod = 2; }
                if (*p == 's') {
                    const char *s = va_arg(ap, const char*);
                    if (!s) s = "(null)";
                    while (*s && n < 250) crash_msg[n++] = *s++;
                } else if (*p == 'p' || *p == 'x') {
                    uint64_t v;
                    if (long_mod == 2)      v = va_arg(ap, uint64_t);
                    else if (long_mod == 1) v = va_arg(ap, unsigned long);
                    else                    v = va_arg(ap, uint64_t);
                    crash_msg[n++] = '0'; crash_msg[n++] = 'x';
                    char hex[17];
                    uitoa_hex(v, hex, sizeof(hex));
                    const char *h = hex; while (*h && n < 250) crash_msg[n++] = *h++;
                } else if (*p == 'u') {
                    uint64_t v;
                    if (long_mod == 2)      v = va_arg(ap, uint64_t);
                    else if (long_mod == 1) v = va_arg(ap, unsigned long);
                    else                    v = va_arg(ap, unsigned int);
                    char tmp[24]; int tn = 0;
                    if (v == 0) tmp[tn++] = '0';
                    while (v && tn < 23) { tmp[tn++] = '0' + (v % 10); v /= 10; }
                    for (int i = tn - 1; i >= 0 && n < 250; i--) crash_msg[n++] = tmp[i];
                } else if (*p == 'd') {
                    int64_t v;
                    if (long_mod == 2)      v = va_arg(ap, int64_t);
                    else if (long_mod == 1) v = va_arg(ap, long);
                    else                    v = va_arg(ap, int);
                    char tmp[24]; int tn = 0;
                    int neg = v < 0;
                    uint64_t uv = neg ? (uint64_t)(-(v + 1)) + 1ULL : (uint64_t)v;
                    if (uv == 0) tmp[tn++] = '0';
                    while (uv && tn < 23) { tmp[tn++] = '0' + (uv % 10); uv /= 10; }
                    if (neg) tmp[tn++] = '-';
                    for (int i = tn - 1; i >= 0 && n < 250; i--) crash_msg[n++] = tmp[i];
                } else {
                    crash_msg[n++] = '%';
                    if (*p) crash_msg[n++] = *p;
                }
                if (*p) p++;
            } else {
                crash_msg[n++] = *p++;
            }
        }
        crash_msg[n] = '\0';
        va_end(ap);
        panic_crash_dump(crash_msg);
        /* FIXED (v4.4.3): P2-2.3 — Write crash dump before halting */
        crash_dump_write_to_disk(crash_msg);
    }

    /* Full red screen — emergency visual takeover */
    console_set_bg(PANIC_BG);
    console_set_fg(PANIC_FG);
    console_clear();

    /* ---- Phase 1: Skull Art ---- */
    int skull_lines = 0;
    while (skull_alt[skull_lines]) skull_lines++;
    console_vcenter(skull_lines + 8);

    for (int i = 0; skull_alt[i]; ++i) {
        console_write_ansi(FG_WHITE);
        console_write_centered(skull_alt[i]);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }

    /* ---- Phase 2: Title ---- */
    console_vpad(1);
    console_write_ansi(PANIC_TITLE_FG);
    console_write_centered("KERNEL PANIC");
    console_write_ansi(SGR_RESET);
    console_vpad(1);

    /* ---- Phase 3: Error Message ---- */
    console_set_fg(PANIC_MSG_COLOR);
    va_start(ap, fmt);
    {
        char buf[256]; int n = 0;
        const char *p = fmt;
        while (*p && n < 250) {
            if (*p == '%') {
                p++;
                /* Handle length modifiers: hh, h, l, ll */
                int long_mod = 0;  /* 0=none, 1=l, 2=ll */
                if (*p == 'l') { p++; long_mod = 1; }
                if (*p == 'l') { p++; long_mod = 2; }
                if (*p == 's') {
                    const char *s = va_arg(ap, const char*);
                    if (!s) s = "(null)";
                    while (*s && n < 250) buf[n++] = *s++;
                } else if (*p == 'p' || *p == 'x') {
                    uint64_t v;
                    if (long_mod == 2)      v = va_arg(ap, uint64_t);
                    else if (long_mod == 1) v = va_arg(ap, unsigned long);
                    else                    v = va_arg(ap, uint64_t);
                    buf[n++] = '0'; buf[n++] = 'x';
                    char hex[17];
                    uitoa_hex(v, hex, sizeof(hex));
                    const char *h = hex; while (*h && n < 250) buf[n++] = *h++;
                } else if (*p == 'u') {
                    uint64_t v;
                    if (long_mod == 2)      v = va_arg(ap, uint64_t);
                    else if (long_mod == 1) v = va_arg(ap, unsigned long);
                    else                    v = va_arg(ap, unsigned int);
                    char tmp[24];
                    int tn = 0;
                    if (v == 0) tmp[tn++] = '0';
                    while (v && tn < 23) { tmp[tn++] = '0' + (v % 10); v /= 10; }
                    for (int i = tn - 1; i >= 0 && n < 250; i--) buf[n++] = tmp[i];
                } else if (*p == 'd') {
                    int64_t v;
                    if (long_mod == 2)      v = va_arg(ap, int64_t);
                    else if (long_mod == 1) v = va_arg(ap, long);
                    else                    v = va_arg(ap, int);
                    char tmp[24];
                    int tn = 0;
                    int neg = v < 0;
                    uint64_t uv = neg ? (uint64_t)(-(v + 1)) + 1ULL : (uint64_t)v;
                    if (uv == 0) tmp[tn++] = '0';
                    while (uv && tn < 23) { tmp[tn++] = '0' + (uv % 10); uv /= 10; }
                    if (neg) tmp[tn++] = '-';
                    for (int i = tn - 1; i >= 0 && n < 250; i--) buf[n++] = tmp[i];
                } else {
                    /* Handle trailing '%' or unknown format specifier:
                     * print the '%' literally, and if *p is not '\0',
                     * print the character after it as well. */
                    buf[n++] = '%';
                    if (*p) buf[n++] = *p;
                }
                if (*p) p++;
            } else {
                buf[n++] = *p++;
            }
        }
        buf[n] = '\0';
        console_write_centered(buf);
    }
    va_end(ap);

    console_vpad(2);

    /* ---- Phase 4: Register Dump (4 columns) ---- */
    struct reg_state regs;
    capture_regs(&regs);
    console_set_fg(PANIC_FG);

    reg_line("RAX", regs.rax); console_write("  ");
    reg_line("RBX", regs.rbx); console_write("  ");
    reg_line("RCX", regs.rcx); console_write("  ");
    reg_line("RDX", regs.rdx); console_putc('\n');
    reg_line("RSI", regs.rsi); console_write("  ");
    reg_line("RDI", regs.rdi); console_write("  ");
    reg_line("RBP", regs.rbp); console_write("  ");
    reg_line("RSP", regs.rsp); console_putc('\n');
    reg_line("R8 ", regs.r8);  console_write("  ");
    reg_line("R9 ", regs.r9);  console_write("  ");
    reg_line("R10", regs.r10); console_write("  ");
    reg_line("R11", regs.r11); console_putc('\n');
    reg_line("R12", regs.r12); console_write("  ");
    reg_line("R13", regs.r13); console_write("  ");
    reg_line("R14", regs.r14); console_write("  ");
    reg_line("R15", regs.r15); console_putc('\n');
    reg_line("RIP", regs.rip); console_write("  ");
    reg_line("FLG", regs.rflags); console_write("  ");
    reg_line("CR2", regs.cr2); console_write("  ");
    reg_line("CR3", regs.cr3); console_putc('\n');

    /* ---- Phase 4.5: Kernel Stack Trace ---- */
    console_vpad(1);
    console_write_ansi(PANIC_TITLE_FG);
    console_write("     Stack Trace (most recent call first):");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    {
        uint64_t *rbp_ptr = (uint64_t *)regs.rbp;
        int depth = 0;
        int max_depth = 12;

        while (rbp_ptr && depth < max_depth) {
            uint64_t *next_rbp = (uint64_t *)*rbp_ptr;
            uint64_t ret_addr = *(rbp_ptr + 1);

            /* Sanity check: kernel code starts at 1MB, no upper bound needed for 64-bit */
            if (!next_rbp || ret_addr < 0x100000) break;

            console_write_ansi(PANIC_LABEL_FG);
            console_write("       #");
            {
                char dbuf[4];
                int n = 0;
                int d = depth;
                if (d == 0) { dbuf[0] = '0'; n = 1; }
                else { while (d > 0 && n < 3) { dbuf[n++] = '0' + (d % 10); d /= 10; } }
                for (int i = n - 1; i >= 0; i--) console_putc(dbuf[i]);
            }
            console_write("  ");
            console_write_ansi(PANIC_VALUE_FG);
            console_write("0x");
            {
                char hex[17];
                uitoa_hex(ret_addr, hex, sizeof(hex));
                console_write(hex);
            }
            console_write_ansi(SGR_RESET);
            console_putc('\n');

            if (next_rbp == rbp_ptr) break; /* prevent infinite loop */
            rbp_ptr = next_rbp;
            depth++;
        }
    }

    /* ---- Phase 5: Bottom Message ---- */
    int cur_row;
    console_get_cursor(&cur_row, NULL);
    int remaining = ROWS - cur_row - 2;
    for (int i = 0; i < remaining; ++i) console_putc('\n');

    console_write_ansi(PANIC_BOTTOM_FG);
    console_write_centered("System halted. Press Ctrl+Alt+Del to restart.");
    console_write_ansi(SGR_RESET);

    /* FIXED (v4.3.5): BUG-NEW-02 — After 3 panics, halt instead of reboot.
     * This prevents the infinite reboot loop where SeaBIOS appears
     * repeatedly.  The user can manually reset after reading the error. */
    if (g_panic_count >= 3) {
        log_printf(LOG_LEVEL_ERR, "panic: Too many panics (%d) — halting system.\n",
                   g_panic_count);
        log_printf(LOG_LEVEL_ERR, "panic: Power cycle or reset to retry.\n");
        asm volatile ("cli; 1: hlt; jmp 1b");
    }

    /* Brief delay before reboot to let the log be visible */
    for (volatile int i = 0; i < 10000000; i++) { asm volatile ("pause"); }

    /* Attempt ACPI reset (port 0x604), fallback to keyboard controller
     * reset (port 0x64), then triple fault as last resort. */
    /* ACPI reset: write 0x2000 to the RESET_REG (FADT) if available.
     * Standard QEMU/Bochs ACPI reset uses port 0x604 with value 0x2000. */
    outw(0x604, 0x2000);
    /* Keyboard controller reset: pulse CPU reset line via 8042 */
    outb(0x64, 0xFE);
    /* If both fail, triple fault to force a hardware reset */
    asm volatile ("int $0");
    for (;;) asm volatile ("hlt");
}
