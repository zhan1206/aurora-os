/*
 * kgdb.c - KGDB-style kernel debugger implementation (v4.2.6)
 *
 * Features:
 *   - Breakpoint engine (INT3, save/restore original byte)
 *   - Single-step via RFLAGS.TF
 *   - Register dump, memory hex dump, stack backtrace
 *   - Symbol resolution for function names
 *   - Interactive command parser
 */
#include "kgdb.h"
#include "console.h"
#include "include/kstdio.h"
#include "include/log.h"
#include <stdint.h>
#include <string.h>

/* KGDB (v4.2.6) - Global debugger state */
static struct kgdb_state g_kgdb;

/* KGDB (v4.2.6) - Symbol table (simple static array) */
#define KGDB_MAX_SYMBOLS 256
static struct kgdb_sym g_symbols[KGDB_MAX_SYMBOLS];
static int g_symbol_count = 0;

/* KGDB (v4.2.6) - Stack bounds for backtrace termination */
static uint64_t g_stack_bottom = 0;
static uint64_t g_stack_top = 0;

/* ================================================================
 * KGDB (v4.2.6) - Initialization
 * ================================================================ */
void kgdb_init(void) {
    g_kgdb.bp_count = 0;
    g_kgdb.single_step = 0;
    g_kgdb.step_bp_addr = 0;
    g_kgdb.initialized = 1;
    g_kgdb.in_handler = 0;

    for (int i = 0; i < KGDB_MAX_BREAKPOINTS; i++) {
        g_kgdb.bps[i].active = 0;
        g_kgdb.bps[i].addr = 0;
        g_kgdb.bps[i].saved_byte = 0;
    }

    g_symbol_count = 0;

    /* KGDB (v4.2.6) - Estimate stack bounds from current RSP */
    uint64_t rsp_val;
    asm volatile ("mov %%rsp, %0" : "=r"(rsp_val));
    g_stack_bottom = rsp_val & ~0xFFFULL;        /* page-align down */
    g_stack_top = g_stack_bottom + 0x8000;        /* 32 KiB stack */

    console_write("\n[KGDB v4.2.6] Kernel debugger initialized\n");
}

int kgdb_is_initialized(void) {
    return g_kgdb.initialized;
}

/* ================================================================
 * KGDB (v4.2.6) - Symbol table
 * ================================================================ */
void kgdb_symbol_register(const char *name, uint64_t addr) {
    /* FIXED (v4.2.7): BUG-KGDB-SYMBOL-FULL — When the symbol table is full,
     * log a warning instead of silently returning.  This helps diagnose
     * boot-time symbol registration issues. */
    if (g_symbol_count >= KGDB_MAX_SYMBOLS) {
        log_printf(LOG_LEVEL_WARN, "kgdb: symbol table full, cannot register %s\n", name);
        return;
    }
    g_symbols[g_symbol_count].name = name;
    g_symbols[g_symbol_count].addr = addr;
    g_symbol_count++;
}

/* FIXED (v4.2.7): BUG-KGDB-SYMBOL-LOOKUP — Linear search over up to
 * KGDB_MAX_SYMBOLS (256) entries.  This is O(n) per lookup, which is
 * acceptable for the current symbol table size.  If the symbol count
 * grows significantly, a hash table or binary search on a sorted
 * array should be added. */
const char *kgdb_lookup_symbol(uint64_t addr, uint64_t *offset) {
    const char *best_name = NULL;
    uint64_t best_addr = 0;

    for (int i = 0; i < g_symbol_count; i++) {
        if (g_symbols[i].addr <= addr && g_symbols[i].addr > best_addr) {
            best_addr = g_symbols[i].addr;
            best_name = g_symbols[i].name;
        }
    }

    if (best_name && offset) {
        *offset = addr - best_addr;
    }
    return best_name;
}

/* ================================================================
 * KGDB (v4.2.6) - Breakpoint engine
 * ================================================================ */
int kgdb_breakpoint_set(uint64_t addr) {
    if (g_kgdb.bp_count >= KGDB_MAX_BREAKPOINTS) return -1;

    /* Check for duplicate */
    for (int i = 0; i < KGDB_MAX_BREAKPOINTS; i++) {
        if (g_kgdb.bps[i].active && g_kgdb.bps[i].addr == addr) return 0;
    }

    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < KGDB_MAX_BREAKPOINTS; i++) {
        if (!g_kgdb.bps[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* FIXED (v4.2.7): BUG-KGDB-SMP-BP - SMP protection for breakpoint
     * modification.  Other CPUs may be executing the same address.
     * __sync_synchronize() provides a full memory barrier before and
     * after code modification.  bp_count is updated atomically.
     * NOTE: Full SMP safety requires pausing other CPUs (IPI) before
     * modifying code bytes; this is a best-effort fix for single-CPU
     * or cooperative multi-CPU environments. */
    uint8_t *ptr = (uint8_t *)addr;
    __sync_synchronize();
    g_kgdb.bps[slot].saved_byte = *ptr;
    *ptr = 0xCC;
    __sync_synchronize();

    g_kgdb.bps[slot].addr = addr;
    g_kgdb.bps[slot].active = 1;
    __sync_fetch_and_add(&g_kgdb.bp_count, 1);

    return 0;
}

int kgdb_breakpoint_clear(uint64_t addr) {
    for (int i = 0; i < KGDB_MAX_BREAKPOINTS; i++) {
        if (g_kgdb.bps[i].active && g_kgdb.bps[i].addr == addr) {
            /* FIXED (v4.2.7): BUG-KGDB-SMP-BP - Restore original byte with
             * SMP protection.  See kgdb_breakpoint_set for details. */
            uint8_t *ptr = (uint8_t *)addr;
            __sync_synchronize();
            *ptr = g_kgdb.bps[i].saved_byte;
            __sync_synchronize();

            g_kgdb.bps[i].active = 0;
            g_kgdb.bps[i].addr = 0;
            g_kgdb.bps[i].saved_byte = 0;
            __sync_fetch_and_sub(&g_kgdb.bp_count, 1);
            return 0;
        }
    }
    return -1;
}

/* KGDB (v4.2.6) - Find breakpoint at address */
static int kgdb_find_bp(uint64_t addr) {
    for (int i = 0; i < KGDB_MAX_BREAKPOINTS; i++) {
        if (g_kgdb.bps[i].active && g_kgdb.bps[i].addr == addr) return i;
    }
    return -1;
}

/* ================================================================
 * KGDB (v4.2.6) - Single-step support
 * ================================================================ */
void kgdb_single_step_enable(void) {
    g_kgdb.single_step = 1;
}

void kgdb_single_step_disable(void) {
    g_kgdb.single_step = 0;
}

/* KGDB (v4.2.6) - Set TF bit in saved RFLAGS on the stack */
static void kgdb_set_tf(struct kgdb_exc_frame *frame) {
    frame->rflags |= (1ULL << 8);  /* TF = bit 8 */
}

/* KGDB (v4.2.6) - Clear TF bit in saved RFLAGS on the stack */
static void kgdb_clear_tf(struct kgdb_exc_frame *frame) {
    frame->rflags &= ~(1ULL << 8);
}

/* ================================================================
 * KGDB (v4.2.6) - Register dump
 * ================================================================ */
static void kgdb_print_regs(struct kgdb_exc_frame *frame) {
    console_write("\n--- Register Dump ---\n");

    char buf[32];
    const char *hex;

    #define KGDB_REG(name, val) do { \
        console_write("  " name ": 0x"); \
        hex = uitoa_hex(val, buf, sizeof(buf)); \
        console_write(hex); \
        console_write("\n"); \
    } while(0)

    KGDB_REG("RAX", frame->rax);
    KGDB_REG("RBX", frame->rbx);
    KGDB_REG("RCX", frame->rcx);
    KGDB_REG("RDX", frame->rdx);
    KGDB_REG("RSI", frame->rsi);
    KGDB_REG("RDI", frame->rdi);
    KGDB_REG("RBP", frame->rbp);
    KGDB_REG("RSP", frame->rsp);
    KGDB_REG("R8 ", frame->r8);
    KGDB_REG("R9 ", frame->r9);
    KGDB_REG("R10", frame->r10);
    KGDB_REG("R11", frame->r11);
    KGDB_REG("R12", frame->r12);
    KGDB_REG("R13", frame->r13);
    KGDB_REG("R14", frame->r14);
    KGDB_REG("R15", frame->r15);
    KGDB_REG("RIP", frame->rip);
    KGDB_REG("RFL", frame->rflags);
    KGDB_REG("CS ", frame->cs);
    KGDB_REG("SS ", frame->ss);

    #undef KGDB_REG

    /* KGDB (v4.2.6) - Show symbol for RIP */
    uint64_t offset;
    const char *sym = kgdb_lookup_symbol(frame->rip, &offset);
    if (sym) {
        console_write("  RIP is in ");
        console_write(sym);
        console_write("+0x");
        hex = uitoa_hex(offset, buf, sizeof(buf));
        console_write(hex);
        console_write("\n");
    }
}

/* ================================================================
 * KGDB (v4.2.6) - Code dump at RIP
 * ================================================================ */
static void kgdb_print_code(uint64_t rip) {
    console_write("\n--- Code at RIP ---\n  ");
    char buf[16];
    for (int i = 0; i < 16; i++) {
        uint8_t byte = *((volatile uint8_t *)(rip + i));
        /* Convert byte to hex chars */
        uint8_t hi = (byte >> 4) & 0xF;
        uint8_t lo = byte & 0xF;
        buf[0] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
        buf[1] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
        buf[2] = ' ';
        buf[3] = '\0';
        console_write(buf);
        if (i == 7) {
            console_write(" ");
        }
    }
    console_write("\n");
}

/* ================================================================
 * KGDB (v4.2.6) - Memory hex dump
 * ================================================================ */
static void kgdb_hexdump(uint64_t addr, int count) {
    if (count <= 0 || count > 256) count = 256;

    char buf[32];
    const char *hex;

    for (int row = 0; row < count; row += 16) {
        /* Address */
        hex = uitoa_hex(addr + row, buf, sizeof(buf));
        console_write("  ");
        console_write(hex);
        console_write("  ");

        /* Hex bytes */
        for (int col = 0; col < 16 && (row + col) < count; col++) {
            if (col == 8) console_write(" ");
            uint8_t byte = *((volatile uint8_t *)(addr + row + col));
            uint8_t hi = (byte >> 4) & 0xF;
            uint8_t lo = byte & 0xF;
            buf[0] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
            buf[1] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
            buf[2] = ' ';
            buf[3] = '\0';
            console_write(buf);
        }

        /* ASCII */
        console_write(" |");
        for (int col = 0; col < 16 && (row + col) < count; col++) {
            uint8_t byte = *((volatile uint8_t *)(addr + row + col));
            char c = (byte >= 32 && byte < 127) ? (char)byte : '.';
            console_putc(c);
        }
        console_write("|\n");
    }
}

/* ================================================================
 * KGDB (v4.2.6) - Stack backtrace (walk RBP chain)
 * ================================================================ */
static void kgdb_backtrace(struct kgdb_exc_frame *frame) {
    console_write("\n--- Stack Backtrace ---\n");

    uint64_t rbp = frame->rbp;
    int depth = 0;
    const int max_depth = 32;

    char buf[32];
    const char *hex;

    while (rbp != 0 && depth < max_depth) {
        /* Check if RBP is within stack bounds */
        if (rbp < g_stack_bottom || rbp >= g_stack_top) {
            console_write("  (RBP out of stack range)\n");
            break;
        }

        /* Frame layout: [RBP] = previous RBP, [RBP+8] = return address */
        uint64_t prev_rbp = *((volatile uint64_t *)rbp);
        uint64_t ret_addr = *((volatile uint64_t *)(rbp + 8));

        if (ret_addr == 0) break;

        /* Print frame */
        hex = uitoa_hex(ret_addr, buf, sizeof(buf));
        console_write("  #");
        {
            char dbuf[8];
            itoa(depth, dbuf, sizeof(dbuf));
            console_write(dbuf);
        }
        console_write("  0x");
        console_write(hex);

        /* Symbol lookup */
        uint64_t offset;
        const char *sym = kgdb_lookup_symbol(ret_addr, &offset);
        if (sym) {
            console_write("  in ");
            console_write(sym);
            console_write("+0x");
            hex = uitoa_hex(offset, buf, sizeof(buf));
            console_write(hex);
        }

        console_write("\n");

        /* Validate next RBP */
        if (prev_rbp <= rbp) {
            console_write("  (RBP did not advance, stopping)\n");
            break;
        }

        rbp = prev_rbp;
        depth++;
    }

    if (depth == 0) {
        console_write("  (no frame chain found)\n");
    }
}

/* ================================================================
 * KGDB (v4.2.6) - Simple command-line input (polling with IRQ safety)
 *
 * FIXED (v4.2.7): BUG-KGDB-CLI-INPUT - Now enables interrupts (sti)
 * during polling so the keyboard IRQ handler can deliver keystrokes.
 * After input is received, cli() is called to return to the debugger's
 * interrupt-disabled context.
 * ================================================================ */
static int kgdb_readline(char *buf, size_t bufsz) {
    /* Print prompt */
    console_write("\nkgdb> ");

    /* FIXED (v4.2.7): BUG-KGDB-CLI-INPUT - Enable interrupts during
     * polling so the keyboard IRQ handler can fire and buffer input.
     * Previously cli() was called before console_getline(), which
     * prevented the keyboard interrupt from delivering keystrokes.
     * Now we sti() first, poll, and only cli() after input is received
     * to return to the debugger's interrupt-disabled context. */
    asm volatile ("sti");

    /* Poll for input */
    while (1) {
        int len = console_getline(buf, bufsz);

        if (len > 0) {
            /* Disable interrupts again for debugger operations */
            asm volatile ("cli");
            return len;
        }

        /* Small delay to let keyboard IRQs fire and buffer input */
        for (volatile int i = 0; i < 50000; i++) {
            asm volatile ("" ::: "memory");
        }
    }
}

/* ================================================================
 * KGDB (v4.2.6) - Command parser
 * ================================================================ */
static int kgdb_parse_command(struct kgdb_exc_frame *frame, const char *cmd) {
    /* Skip leading whitespace */
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') return 0;  /* empty line, stay in debugger */

    /* KGDB (v4.2.6) - Help */
    if (cmd[0] == 'h' && (cmd[1] == ' ' || cmd[1] == '\0' ||
        (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p'))) {
        console_write("\nKGDB Commands:\n");
        console_write("  c / continue     - Resume execution\n");
        console_write("  s / step         - Single step (execute one instruction)\n");
        console_write("  r / regs         - Print all registers\n");
        console_write("  m <addr> [cnt]   - Hex dump memory (default 16 bytes)\n");
        console_write("  b <addr>         - Set breakpoint\n");
        console_write("  d <addr>         - Delete breakpoint\n");
        console_write("  bt / backtrace   - Stack backtrace (RBP chain)\n");
        console_write("  q / quit         - Resume execution\n");
        console_write("  h / help         - Show this help\n");
        return 0;
    }

    /* KGDB (v4.2.6) - Continue / Quit */
    if ((cmd[0] == 'c' && (cmd[1] == ' ' || cmd[1] == '\0')) ||
        (cmd[0] == 'q' && (cmd[1] == ' ' || cmd[1] == '\0'))) {
        return 1;  /* exit debugger, resume */
    }

    /* KGDB (v4.2.6) - Step */
    if (cmd[0] == 's' && (cmd[1] == ' ' || cmd[1] == '\0')) {
        kgdb_set_tf(frame);
        kgdb_single_step_enable();
        return 1;  /* exit debugger, single-step */
    }

    /* KGDB (v4.2.6) - Registers */
    if (cmd[0] == 'r' && (cmd[1] == ' ' || cmd[1] == '\0')) {
        kgdb_print_regs(frame);
        return 0;
    }

    /* KGDB (v4.2.6) - Backtrace */
    if (cmd[0] == 'b' && cmd[1] == 't' && (cmd[2] == ' ' || cmd[2] == '\0')) {
        kgdb_backtrace(frame);
        return 0;
    }

    /* KGDB (v4.2.6) - Memory dump */
    if (cmd[0] == 'm') {
        const char *p = cmd + 1;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') {
            /* Default: dump at RIP */
            kgdb_hexdump(frame->rip, 16);
            return 0;
        }
        /* Parse address */
        uint64_t addr = 0;
        while (*p && *p != ' ' && *p != '\t') {
            char c = *p;
            uint8_t nib = 0;
            if (c >= '0' && c <= '9') nib = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') nib = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nib = (uint8_t)(c - 'A' + 10);
            else break;
            addr = (addr << 4) | nib;
            p++;
        }
        /* Parse count */
        while (*p == ' ' || *p == '\t') p++;
        int count = 16;
        if (*p) {
            count = 0;
            while (*p >= '0' && *p <= '9') {
                count = count * 10 + (int)(*p - '0');
                p++;
            }
            if (count <= 0) count = 16;
        }
        kgdb_hexdump(addr, count);
        return 0;
    }

    /* KGDB (v4.2.6) - Set breakpoint */
    if (cmd[0] == 'b' && cmd[1] != 't') {
        const char *p = cmd + 1;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') {
            console_write("  Usage: b <addr>\n");
            return 0;
        }
        uint64_t addr = 0;
        while (*p && *p != ' ' && *p != '\t') {
            char c = *p;
            uint8_t nib = 0;
            if (c >= '0' && c <= '9') nib = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') nib = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nib = (uint8_t)(c - 'A' + 10);
            else break;
            addr = (addr << 4) | nib;
            p++;
        }
        if (kgdb_breakpoint_set(addr) == 0) {
            console_write("  Breakpoint set at 0x");
            char buf[32];
            console_write(uitoa_hex(addr, buf, sizeof(buf)));
            console_write("\n");
        } else {
            console_write("  Failed to set breakpoint\n");
        }
        return 0;
    }

    /* KGDB (v4.2.6) - Delete breakpoint */
    if (cmd[0] == 'd') {
        const char *p = cmd + 1;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') {
            console_write("  Usage: d <addr>\n");
            return 0;
        }
        uint64_t addr = 0;
        while (*p && *p != ' ' && *p != '\t') {
            char c = *p;
            uint8_t nib = 0;
            if (c >= '0' && c <= '9') nib = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') nib = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nib = (uint8_t)(c - 'A' + 10);
            else break;
            addr = (addr << 4) | nib;
            p++;
        }
        if (kgdb_breakpoint_clear(addr) == 0) {
            console_write("  Breakpoint deleted at 0x");
            char buf[32];
            console_write(uitoa_hex(addr, buf, sizeof(buf)));
            console_write("\n");
        } else {
            console_write("  No breakpoint at that address\n");
        }
        return 0;
    }

    /* Unknown command */
    console_write("  Unknown command: ");
    console_write(cmd);
    console_write("\n  Type 'h' for help.\n");
    return 0;
}

/* ================================================================
 * KGDB (v4.2.6) - Command loop
 * ================================================================ */
static void kgdb_command_loop(struct kgdb_exc_frame *frame) {
    char buf[256];

    while (1) {
        int len = kgdb_readline(buf, sizeof(buf));
        if (len <= 0) continue;

        int exit_loop = kgdb_parse_command(frame, buf);
        if (exit_loop) break;
    }

    /* KGDB (v4.2.6) - iretq will restore RFLAGS (including IF) from the
     * saved frame, so we don't need to explicitly cli here. */
}

/* ================================================================
 * KGDB (v4.2.6) - INT3 exception handler (breakpoint hit)
 *
 * Called from assembly when INT3 (vector 3) fires.
 * frame->rip points to the instruction AFTER the INT3 byte (0xCC),
 * so the actual breakpoint address is frame->rip - 1.
 * ================================================================ */
void kgdb_handle_exception(struct kgdb_exc_frame *frame) {
    if (!g_kgdb.initialized) return;

    /* Re-entrancy guard */
    if (g_kgdb.in_handler) {
        /* Oops — recursive debugger entry. Just continue. */
        return;
    }
    g_kgdb.in_handler = 1;

    /* KGDB (v4.2.6) - The CPU pushes RIP pointing AFTER the INT3 byte.
     * Adjust RIP back to the breakpoint address. */
    uint64_t bp_addr = frame->rip - 1;
    frame->rip = bp_addr;

    /* Check if this is one of our breakpoints */
    int bp_idx = kgdb_find_bp(bp_addr);

    if (bp_idx >= 0) {
        /* KGDB (v4.2.6) - Our breakpoint: restore original byte,
         * single-step over it, then re-insert 0xCC. */
        uint8_t *ptr = (uint8_t *)bp_addr;
        *ptr = g_kgdb.bps[bp_idx].saved_byte;

        /* Set TF for single-step */
        kgdb_set_tf(frame);
        g_kgdb.single_step = 1;
        g_kgdb.step_bp_addr = bp_addr;

        g_kgdb.in_handler = 0;
        return;  /* iretq will execute the original instruction, then #DB fires */
    }

    /* KGDB (v4.2.6) - Not our breakpoint (manual INT3 or INT3 instruction).
     * Enter command loop directly. */
    console_write("\n");
    console_write("--- KGDB Breakpoint Hit ---\n");
    {
        char buf[32];
        const char *hex = uitoa_hex(bp_addr, buf, sizeof(buf));
        console_write("  Address: 0x");
        console_write(hex);
        console_write("\n");
        uint64_t offset;
        const char *sym = kgdb_lookup_symbol(bp_addr, &offset);
        if (sym) {
            console_write("  Symbol:  ");
            console_write(sym);
            console_write("+0x");
            hex = uitoa_hex(offset, buf, sizeof(buf));
            console_write(hex);
            console_write("\n");
        }
    }
    kgdb_print_regs(frame);
    kgdb_print_code(bp_addr);
    kgdb_backtrace(frame);

    kgdb_command_loop(frame);

    g_kgdb.in_handler = 0;
}

/* ================================================================
 * KGDB (v4.2.6) - Debug exception handler (#DB, vector 1)
 *
 * Called from assembly when #DB fires.
 * In our case, #DB is used for single-step (TF=1).
 * After single-stepping over a breakpoint, re-insert 0xCC.
 * ================================================================ */
void kgdb_handle_debug(struct kgdb_exc_frame *frame) {
    if (!g_kgdb.initialized) return;

    /* KGDB (v4.2.6) - If not in single-step mode, ignore */
    if (!g_kgdb.single_step) {
        return;
    }

    /* KGDB (v4.2.6) - Re-insert 0xCC at the breakpoint we stepped over */
    if (g_kgdb.step_bp_addr != 0) {
        int bp_idx = kgdb_find_bp(g_kgdb.step_bp_addr);
        if (bp_idx >= 0) {
            uint8_t *ptr = (uint8_t *)g_kgdb.step_bp_addr;
            *ptr = 0xCC;
        }
        g_kgdb.step_bp_addr = 0;
    }

    /* Clear TF and single-step mode */
    kgdb_clear_tf(frame);
    kgdb_single_step_disable();

    /* Enter command loop */
    console_write("\n--- KGDB Single-Step Complete ---\n");
    {
        char buf[32];
        const char *hex = uitoa_hex(frame->rip, buf, sizeof(buf));
        console_write("  RIP: 0x");
        console_write(hex);
        console_write("\n");
    }
    kgdb_print_regs(frame);
    kgdb_print_code(frame->rip);

    kgdb_command_loop(frame);
}

/* ================================================================
 * KGDB_RSP (v4.2.7) - GDB Remote Serial Protocol support
 *
 * Implements a minimal GDB stub over a serial line.  The protocol
 * uses ASCII hex encoding with `$` packet start, `#` checksum, and
 * `+` / `-` acknowledgements.
 *
 * For integration: connect a serial port (e.g. COM1) to GDB via
 * `target remote /dev/ttyS0` or `target remote localhost:1234` if
 * the serial port is forwarded through QEMU's `-serial tcp::1234`.
 * ================================================================ */

/* KGDB_RSP (v4.2.7) - Convert a nibble (0-15) to hex character */
static char kgdb_nibble_to_hex(uint8_t nib) {
    return (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
}

/* KGDB_RSP (v4.2.7) - Convert a hex character to nibble value */
static int kgdb_hex_to_nibble(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

/* KGDB_RSP (v4.2.7) - Compute GDB RSP checksum (sum of all bytes mod 256) */
static uint8_t kgdb_rsp_checksum(const char *data, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += (uint8_t)data[i];
    }
    return sum;
}

/* KGDB_RSP (v4.2.7) - GDB RSP state machine */
#define KGDB_RSP_BUF_SIZE 1024
static char    kgdb_rsp_buf[KGDB_RSP_BUF_SIZE];
static int     kgdb_rsp_len = 0;
static int     kgdb_rsp_in_packet = 0;
static int     kgdb_rsp_escape = 0;
static int     kgdb_rsp_in_checksum = 0;
static uint8_t kgdb_rsp_checksum_rx = 0;
static int     kgdb_rsp_checksum_count = 0;

/*
 * KGDB_RSP (v4.2.7) - Send a response packet to GDB.
 * Format: $<data>#<checksum>
 * Automatically sends ACK (+) before the packet.
 */
static void kgdb_rsp_send(const char *data) {
    /* Send acknowledgement */
    console_putc('+');

    /* Send packet start */
    console_putc('$');

    /* Send data */
    int len = 0;
    uint8_t csum = 0;
    while (data[len] != '\0') {
        console_putc(data[len]);
        csum += (uint8_t)data[len];
        len++;
    }

    /* Send checksum */
    console_putc('#');
    console_putc(kgdb_nibble_to_hex((csum >> 4) & 0xF));
    console_putc(kgdb_nibble_to_hex(csum & 0xF));
}

/* KGDB_RSP (v4.2.7) - Send a simple OK response */
static void kgdb_rsp_send_ok(void) {
    kgdb_rsp_send("OK");
}

/* KGDB_RSP (v4.2.7) - Send an error response */
static void kgdb_rsp_send_error(uint8_t err) {
    char buf[4];
    buf[0] = 'E';
    buf[1] = kgdb_nibble_to_hex((err >> 4) & 0xF);
    buf[2] = kgdb_nibble_to_hex(err & 0xF);
    buf[3] = '\0';
    kgdb_rsp_send(buf);
}

/* KGDB_RSP (v4.2.7) - Read registers and format as hex string */
static void kgdb_rsp_read_regs(char *out, int out_sz) {
    struct kgdb_exc_frame frame;
    /* Read current register values from the CPU */
    asm volatile (
        "mov %%rax, %0\n"
        "mov %%rbx, %1\n"
        "mov %%rcx, %2\n"
        "mov %%rdx, %3\n"
        "mov %%rsi, %4\n"
        "mov %%rdi, %5\n"
        "mov %%rbp, %6\n"
        "mov %%rsp, %7\n"
        "mov %%r8,  %8\n"
        "mov %%r9,  %9\n"
        "mov %%r10, %10\n"
        "mov %%r11, %11\n"
        "mov %%r12, %12\n"
        "mov %%r13, %13\n"
        "mov %%r14, %14\n"
        "mov %%r15, %15\n"
        : "=r"(frame.rax), "=r"(frame.rbx), "=r"(frame.rcx), "=r"(frame.rdx),
          "=r"(frame.rsi), "=r"(frame.rdi), "=r"(frame.rbp), "=r"(frame.rsp),
          "=r"(frame.r8),  "=r"(frame.r9),  "=r"(frame.r10), "=r"(frame.r11),
          "=r"(frame.r12), "=r"(frame.r13), "=r"(frame.r14), "=r"(frame.r15)
        :
        : "memory"
    );

    /* Read RIP */
    asm volatile ("lea (%%rip), %0" : "=r"(frame.rip));

    /* Read RFLAGS */
    asm volatile (
        "pushfq\n"
        "pop %0\n"
        : "=r"(frame.rflags)
        :
        : "memory"
    );

    uint64_t *regs = (uint64_t *)&frame;
    int pos = 0;

    /* GDB expects registers in order: RAX, RBX, RCX, RDX, RSI, RDI,
     * RBP, RSP, R8-R15, RIP, EFLAGS, CS, SS, DS, ES, FS, GS */
    uint64_t reg_order[] = {
        frame.rax, frame.rbx, frame.rcx, frame.rdx,
        frame.rsi, frame.rdi, frame.rbp, frame.rsp,
        frame.r8,  frame.r9,  frame.r10, frame.r11,
        frame.r12, frame.r13, frame.r14, frame.r15,
        frame.rip, frame.rflags,
        0x10,  /* CS = 0x10 (kernel code segment) */
        0x18,  /* SS = 0x18 (kernel data segment) */
        0x18,  /* DS */
        0x18,  /* ES */
        0x18,  /* FS */
        0x18   /* GS */
    };

    int num_regs = (int)(sizeof(reg_order) / sizeof(reg_order[0]));
    for (int i = 0; i < num_regs && pos < out_sz - 20; i++) {
        uint64_t val = reg_order[i];
        /* GDB stores registers in target byte order (little-endian for x86_64) */
        for (int b = 0; b < 8; b++) {
            uint8_t byte = (uint8_t)(val & 0xFF);
            out[pos++] = kgdb_nibble_to_hex((byte >> 4) & 0xF);
            out[pos++] = kgdb_nibble_to_hex(byte & 0xF);
            val >>= 8;
        }
    }
    out[pos] = '\0';
    (void)regs; /* suppress unused warning */
}

/* KGDB_RSP (v4.2.7) - Write registers from hex string */
static void kgdb_rsp_write_regs(const char *hex) {
    /* Minimal implementation: accept the data but don't modify registers
     * in the running kernel context.  A full implementation would
     * modify the saved register frame on the stack. */
    (void)hex;
}

/* KGDB_RSP (v4.2.7) - Read memory and format as hex */
static void kgdb_rsp_read_mem(uint64_t addr, uint32_t len, char *out, int out_sz) {
    int pos = 0;
    for (uint32_t i = 0; i < len && pos < out_sz - 3; i++) {
        uint8_t byte = *((volatile uint8_t *)(uintptr_t)(addr + i));
        out[pos++] = kgdb_nibble_to_hex((byte >> 4) & 0xF);
        out[pos++] = kgdb_nibble_to_hex(byte & 0xF);
    }
    out[pos] = '\0';
}

/* KGDB_RSP (v4.2.7) - Write memory from hex string */
static int kgdb_rsp_write_mem(uint64_t addr, uint32_t len, const char *hex) {
    for (uint32_t i = 0; i < len; i++) {
        int hi = kgdb_hex_to_nibble(hex[i * 2]);
        int lo = kgdb_hex_to_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        uint8_t byte = (uint8_t)((hi << 4) | lo);
        *((volatile uint8_t *)(uintptr_t)(addr + i)) = byte;
    }
    return 0;
}

/* KGDB_RSP (v4.2.7) - Parse a hex number from the command buffer */
static int kgdb_rsp_parse_hex(const char **p, uint64_t *val) {
    *val = 0;
    int count = 0;
    while (**p) {
        int nib = kgdb_hex_to_nibble(**p);
        if (nib < 0) break;
        *val = (*val << 4) | (uint64_t)nib;
        (*p)++;
        count++;
    }
    return count;
}

/* KGDB_RSP (v4.2.7) - Process a complete GDB RSP command packet */
static void kgdb_rsp_process_command(const char *cmd) {
    if (*cmd == '\0') {
        kgdb_rsp_send_ok();
        return;
    }

    switch (cmd[0]) {
    case '?': {
        /* Report signal — we report SIGTRAP (5) since we stopped at a breakpoint */
        kgdb_rsp_send("S05");
        break;
    }

    case 'g': {
        /* Read all registers */
        char regs[1024];
        kgdb_rsp_read_regs(regs, sizeof(regs));
        kgdb_rsp_send(regs);
        break;
    }

    case 'G': {
        /* Write all registers */
        kgdb_rsp_write_regs(cmd + 1);
        kgdb_rsp_send_ok();
        break;
    }

    case 'm': {
        /* Read memory: m addr,len */
        const char *p = cmd + 1;
        uint64_t addr = 0;
        uint64_t len = 0;
        if (kgdb_rsp_parse_hex(&p, &addr) == 0) { kgdb_rsp_send_error(0x22); break; }
        if (*p == ',') p++;
        if (kgdb_rsp_parse_hex(&p, &len) == 0) { kgdb_rsp_send_error(0x22); break; }
        if (len > 512) len = 512;  /* Limit response size */
        {
            char *mem_buf = (char *)kmalloc((size_t)(len * 2 + 1));
            if (mem_buf) {
                kgdb_rsp_read_mem(addr, (uint32_t)len, mem_buf, (int)(len * 2 + 1));
                kgdb_rsp_send(mem_buf);
                kfree(mem_buf);
            } else {
                kgdb_rsp_send_error(0x0C);  /* ENOMEM */
            }
        }
        break;
    }

    case 'M': {
        /* Write memory: M addr,len:data */
        const char *p = cmd + 1;
        uint64_t addr = 0;
        uint64_t len = 0;
        if (kgdb_rsp_parse_hex(&p, &addr) == 0) { kgdb_rsp_send_error(0x22); break; }
        if (*p == ',') p++;
        if (kgdb_rsp_parse_hex(&p, &len) == 0) { kgdb_rsp_send_error(0x22); break; }
        if (*p == ':') p++;
        if (kgdb_rsp_write_mem(addr, (uint32_t)len, p) == 0) {
            kgdb_rsp_send_ok();
        } else {
            kgdb_rsp_send_error(0x0E);  /* EFAULT */
        }
        break;
    }

    case 's': {
        /* Single step — enable TF and resume */
        /* The caller checks return value to resume execution */
        break;
    }

    case 'c': {
        /* Continue — resume execution */
        break;
    }

    case 'Z': {
        /* Set breakpoint: Z0,addr,kind */
        if (cmd[1] == '0' && cmd[2] == ',') {
            const char *p = cmd + 3;
            uint64_t addr = 0;
            if (kgdb_rsp_parse_hex(&p, &addr) > 0) {
                if (kgdb_breakpoint_set(addr) == 0) {
                    kgdb_rsp_send_ok();
                } else {
                    kgdb_rsp_send_error(0x0C);  /* ENOMEM / no slots */
                }
            } else {
                kgdb_rsp_send_error(0x22);  /* EINVAL */
            }
        } else {
            /* Other Z types (Z1=hw watchpoint, Z2=read watchpoint, etc.) */
            kgdb_rsp_send("");  /* Not supported */
        }
        break;
    }

    case 'z': {
        /* Remove breakpoint: z0,addr,kind */
        if (cmd[1] == '0' && cmd[2] == ',') {
            const char *p = cmd + 3;
            uint64_t addr = 0;
            if (kgdb_rsp_parse_hex(&p, &addr) > 0) {
                if (kgdb_breakpoint_clear(addr) == 0) {
                    kgdb_rsp_send_ok();
                } else {
                    kgdb_rsp_send_error(0x22);  /* EINVAL */
                }
            } else {
                kgdb_rsp_send_error(0x22);
            }
        } else {
            kgdb_rsp_send("");  /* Not supported */
        }
        break;
    }

    default:
        /* Unknown command — send empty response (GDB interprets as unsupported) */
        kgdb_rsp_send("");
        break;
    }
}

/*
 * kgdb_handle_remote: Process a single byte from the GDB RSP serial stream.
 *
 * Returns 1 if the caller should continue polling, 0 if execution should
 * resume (after a 'c' or 's' command).
 */
int kgdb_handle_remote(char c) {
    /* Handle GDB interrupt (Ctrl-C = 0x03) */
    if (c == 0x03) {
        /* GDB sent Ctrl-C — we should stop and enter the debugger */
        kgdb_rsp_send("S02");  /* SIGINT */
        return 1;
    }

    if (!kgdb_rsp_in_packet) {
        if (c == '$') {
            /* Start of packet */
            kgdb_rsp_in_packet = 1;
            kgdb_rsp_len = 0;
            kgdb_rsp_escape = 0;
            kgdb_rsp_in_checksum = 0;
            kgdb_rsp_checksum_rx = 0;
            kgdb_rsp_checksum_count = 0;
        } else if (c == '+') {
            /* ACK from GDB — ignore */
        } else if (c == '-') {
            /* NAK from GDB — should resend, but we'll ignore for simplicity */
        }
        return 1;
    }

    /* Inside a packet */
    if (kgdb_rsp_escape) {
        /* Escape sequence: }^X means XOR with 0x20 */
        kgdb_rsp_escape = 0;
        c ^= 0x20;
    } else if (c == '}') {
        kgdb_rsp_escape = 1;
        return 1;
    } else if (c == '#') {
        /* End of packet body, now expect 2 checksum bytes */
        kgdb_rsp_checksum_rx = kgdb_rsp_checksum(kgdb_rsp_buf, kgdb_rsp_len);
        kgdb_rsp_checksum_count = 0;
        kgdb_rsp_in_checksum = 1;
        return 1;
    }

    /* Check if we're in checksum mode */
    if (kgdb_rsp_in_checksum) {
        int nib = kgdb_hex_to_nibble(c);
        if (nib >= 0) {
            kgdb_rsp_checksum_count++;
            if (kgdb_rsp_checksum_count == 2) {
                /* Packet complete */
                kgdb_rsp_buf[kgdb_rsp_len] = '\0';
                kgdb_rsp_in_packet = 0;
                kgdb_rsp_in_checksum = 0;

                /* Process the command */
                int should_resume = 0;
                if (kgdb_rsp_len > 0) {
                    char first = kgdb_rsp_buf[0];
                    if (first == 'c' || first == 's') {
                        should_resume = 1;
                    }
                }
                kgdb_rsp_process_command(kgdb_rsp_buf);

                if (should_resume) {
                    return 0;  /* Resume execution */
                }
            }
        }
        return 1;
    }

    /* Normal packet data */
    if (kgdb_rsp_len < KGDB_RSP_BUF_SIZE - 1) {
        kgdb_rsp_buf[kgdb_rsp_len++] = c;
    }

    return 1;
}