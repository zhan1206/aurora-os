/*
 * kgdb.h - KGDB-style kernel debugger interface (v4.2.6)
 *
 * Provides a simple in-kernel debugger with breakpoints, single-step,
 * register dump, memory dump, stack backtrace, and symbol resolution.
 */
#ifndef KGDB_H
#define KGDB_H

#include <stdint.h>

/* KGDB (v4.2.6) */
#define KGDB_MAX_BREAKPOINTS 64

/* KGDB (v4.2.6) - Exception frame layout matching kgdb exc_common pushes.
 * Push order (bottom to top): rax, rcx, rdx, rsi, rdi, r8-r15, rbx, rbp */
struct kgdb_exc_frame {
    uint64_t rax, rcx, rdx, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rbx, rbp;
    uint64_t vector;
    uint64_t error_code;
    /* CPU interrupt frame */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/* KGDB (v4.2.6) - Breakpoint entry */
struct kgdb_bp {
    uint64_t addr;
    uint8_t  saved_byte;
    int      active;
};

/* KGDB (v4.2.6) - Symbol table entry */
struct kgdb_sym {
    const char *name;
    uint64_t    addr;
};

/* KGDB (v4.2.6) - Debugger state */
struct kgdb_state {
    struct kgdb_bp bps[KGDB_MAX_BREAKPOINTS];
    int            bp_count;
    int            single_step;
    uint64_t       step_bp_addr;
    int            initialized;
    int            in_handler;          /* re-entrancy guard */
};

/* KGDB (v4.2.6) - Public API */
void kgdb_init(void);
int  kgdb_breakpoint_set(uint64_t addr);
int  kgdb_breakpoint_clear(uint64_t addr);
void kgdb_handle_exception(struct kgdb_exc_frame *frame);
void kgdb_handle_debug(struct kgdb_exc_frame *frame);
int  kgdb_is_initialized(void);

/* KGDB (v4.2.6) - Symbol resolution */
void kgdb_symbol_register(const char *name, uint64_t addr);
const char *kgdb_lookup_symbol(uint64_t addr, uint64_t *offset);

#endif /* KGDB_H */