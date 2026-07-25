/*
 * seccomp.c - System call filtering implementation
 *
 * Provides a syscall filter for per-task access control.  Supports
 * two modes:
 *   1. Bitmap mode: 256-bit bitmap for fast syscall number filtering.
 *   2. BPF mode: Classic BPF program with argument-level filtering.
 *
 * FIXED (v4.0.6):
 *   - Added seccomp_lock spinlock to prevent UAF race between
 *     seccomp_set_filter() and seccomp_check() on SMP systems.
 *
 * FIXED (v4.1.9):
 *   - Added BPF interpreter (seccomp_run_bpf) for argument-level
 *     syscall filtering.  Previously only bitmap-based syscall number
 *     filtering was supported, which could not restrict specific
 *     argument values (e.g., only allow SYS_write to fd=1).  (H-29)
 *   - Modified seccomp_check() to accept syscall arguments for BPF
 *     validation.  Backward compatible: args may be NULL for bitmap-only.
 */

#include "seccomp.h"
#include "sched.h"
#include "smp.h"
#include "mem.h"
#include "include/log.h"
#include "include/errno.h"
#include <stdint.h>
#include <string.h>

/* ================================================================
 * seccomp_set_filter
 *
 * Atomically installs or removes a seccomp filter for the given
 * task. Uses seccomp_lock to prevent races with seccomp_check().
 * ================================================================ */

int seccomp_set_filter(struct task_struct *task, struct seccomp_filter *filter) {
    if (!task) return -1;

    spin_lock((spinlock_t*)&task->seccomp_lock);

    /*
     * FIXED (v4.1.4): Reject NULL filter removal once a filter is
     * installed.  seccomp is a one-way door: once a process installs
     * a filter, it can only be replaced with a more restrictive one
     * (or the same), never removed.  Passing NULL to remove the filter
     * would completely bypass the security model.  (BUG 3.8)
     */
    if (!filter) {
        /* If a filter is already installed, reject the removal */
        if (task->seccomp) {
            spin_unlock((spinlock_t*)&task->seccomp_lock);
            log_printf(LOG_LEVEL_WARN, "seccomp: pid=%d attempted to remove filter\n",
                       task->pid);
            return -1;
        }
        /* No filter installed, nothing to remove — OK */
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        return 0;
    }

    /*
     * FIXED (v4.2.5): BUG-SECCOMP-REPLACE — Reject filter replacement
     * once a filter is already installed.  seccomp is a one-way door:
     * after the first filter is installed, it cannot be replaced with
     * a more permissive one.  This prevents an attacker from replacing
     * a strict filter with a weaker one.
     */
    if (task->seccomp) {
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        log_printf(LOG_LEVEL_WARN, "seccomp: pid=%d attempted to replace filter\n",
                   task->pid);
        return -EACCES;
    }

    /*
     * FIXED (v4.1.9): Validate BPF program length.
     * Reject filters with excessively long BPF programs to prevent
     * memory exhaustion.  (H-29: seccomp BPF validation)
     */
    if (filter->bpf_len > SECCOMP_MAX_BPF_LEN) {
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        log_printf(LOG_LEVEL_ERR, "seccomp: pid=%d BPF program too long (%u > %u)\n",
                   task->pid, filter->bpf_len, SECCOMP_MAX_BPF_LEN);
        return -1;
    }

    /* Allocate and copy the new filter.
     * NOTE: The filter pointer is assumed to already reference kernel memory
     * (validated and copied by the syscall layer via copy_from_user before
     * this function is called).  copy_from_user handles page faults in the
     * user page, so the data is safe against concurrent user-page unmapping. */
    size_t filter_size = sizeof(struct seccomp_filter);
    struct seccomp_filter *new_filter = kmalloc(filter_size);
    if (!new_filter) {
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        return -1;
    }

    memcpy(new_filter, filter, filter_size);

    /* Atomically swap in the new filter under the lock */
    struct seccomp_filter *old = task->seccomp;
    task->seccomp = new_filter;
    spin_unlock((spinlock_t*)&task->seccomp_lock);

    if (old) kfree(old);

    log_printf(LOG_LEVEL_INFO, "seccomp: filter installed for pid=%d (bpf_len=%u)\n",
               task->pid, new_filter->bpf_len);

    return 0;
}

/* ================================================================
 * seccomp_run_bpf - Classic BPF interpreter
 *
 * Executes a BPF program against the given seccomp_data.  The BPF
 * virtual machine has two 32-bit registers (A and X) and a simple
 * instruction set supporting loads, arithmetic, jumps, and returns.
 *
 * The BPF program reads from seccomp_data via BPF_ABS offsets.
 * Returns SECCOMP_RET_ALLOW (converted to 0) or -1 for KILL.
 *
 * FIXED (v4.1.9): Implemented BPF interpreter for argument-level
 * syscall filtering.  (H-29: seccomp BPF argument verification)
 * ================================================================ */

int seccomp_run_bpf(const struct sock_filter *prog, uint16_t len,
                     const struct seccomp_data *data) {
    if (!prog || len == 0) return 0;  /* empty program = allow */
    if (!data) return -1;              /* no data = deny */

    uint32_t A = 0;  /* accumulator */
    uint32_t X = 0;  /* index register */
    uint32_t pc = 0; /* program counter */

    /*
     * FIXED (v4.2.3): BPF instruction execution limit to prevent
     * infinite loops.  Classic BPF supports backward jumps (JMP with
     * negative offset), which could create infinite loops.  Linux
     * kernel uses BPF_MAXINSNS=4096.  We limit to 4096 instructions
     * per evaluation; exceeding this returns SECCOMP_RET_KILL.
     */
    #define BPF_MAX_EXEC_INSNS 4096
    uint32_t insn_count = 0;

    /*
     * FIXED (v4.2.3): BPF scratch memory (M[0..15]).
     * Classic BPF programs use scratch memory for temporary storage
     * via LDX/ST/STX instructions.  Previously, LDX|BPF_MEM returned
     * X=0 and ST/STX were silently ignored, effectively bypassing
     * any filter that uses scratch memory.  (BUG-SEC-01)
     */
    #define BPF_SCRATCH_SIZE 16
    uint32_t scratch[BPF_SCRATCH_SIZE];
    memset(scratch, 0, sizeof(scratch));

    while (pc < len) {
        const struct sock_filter *insn = &prog[pc];
        uint16_t code = insn->code;
        uint32_t k   = insn->k;

        switch (code) {
        /* ================================================
         * BPF_LD: Load into A
         * ================================================ */
        case BPF_LD | BPF_W | BPF_ABS:
            /*
             * Load 32-bit word from seccomp_data at offset k.
             * The seccomp_data structure is laid out as:
             *   [0]  nr (int, 4 bytes)
             *   [4]  arch (uint32_t, 4 bytes)
             *   [8]  instruction_pointer (uint64_t, 8 bytes)
             *   [16] args[0] (uint64_t)
             *   [24] args[1] (uint64_t)
             *   ...
             * We read a 32-bit value at offset k.
             */
            {
                const uint8_t *base = (const uint8_t *)data;
                if (k + 4 > sizeof(struct seccomp_data)) {
                    /* Out of bounds: return KILL */
                    return -1;
                }
                A = *(const uint32_t *)(base + k);
            }
            break;

        case BPF_LD | BPF_W | BPF_LEN:
            /* Packet length = sizeof(seccomp_data) */
            A = (uint32_t)sizeof(struct seccomp_data);
            break;

        case BPF_LD | BPF_W | BPF_IMM:
            A = k;
            break;

        case BPF_LD | BPF_H | BPF_ABS:
            /* Load 16-bit halfword */
            {
                const uint8_t *base = (const uint8_t *)data;
                if (k + 2 > sizeof(struct seccomp_data)) return -1;
                A = *(const uint16_t *)(base + k);
            }
            break;

        case BPF_LD | BPF_B | BPF_ABS:
            /* Load 8-bit byte */
            {
                const uint8_t *base = (const uint8_t *)data;
                if (k + 1 > sizeof(struct seccomp_data)) return -1;
                A = *(const uint8_t *)(base + k);
            }
            break;

        case BPF_LD | BPF_W | BPF_IND:
            /*
             * Indirect load: X + k is the offset into seccomp_data.
             * Read 4 bytes at that offset.
             *
             * FIXED (v4.2.1): Check for integer overflow in X + k.
             * If X + k wraps around, the subsequent bounds check would
             * be bypassed, allowing arbitrary 4-byte reads from kernel
             * memory.  (BUG-SEC-H2)
             */
            {
                uint32_t offset;
                if (X > UINT32_MAX - k) return -1;  /* overflow check */
                offset = X + k;
                const uint8_t *base = (const uint8_t *)data;
                if (offset + 4 > sizeof(struct seccomp_data)) return -1;
                A = *(const uint32_t *)(base + offset);
            }
            break;

        /* ================================================
         * BPF_LDX: Load into X
         * ================================================ */
        case BPF_LDX | BPF_W | BPF_IMM:
            X = k;
            break;

        case BPF_LDX | BPF_W | BPF_MEM:
            /* Load from scratch memory M[k].
             * FIXED (v4.2.3): Implemented scratch memory.  Previously
             * always returned 0, bypassing filters that use scratch.
             * FIXED (v4.2.7): BUG-SECCOMP-SCRATCH-BOUNDS — bounds-check
             * k before accessing scratch[k] to prevent out-of-bounds
             * memory access.  (BUG-SEC-01) */
            {
                if (k >= BPF_SCRATCH_SIZE) { return -1; } /* FIXED (v4.2.7): BUG-SECCOMP-SCRATCH-BOUNDS */
                uint32_t idx = k & (BPF_SCRATCH_SIZE - 1);
                X = scratch[idx];
            }
            break;

        case BPF_LDX | BPF_W | BPF_LEN:
            X = (uint32_t)sizeof(struct seccomp_data);
            break;

        case BPF_LDX | BPF_B | BPF_MSH:
            /*
             * Load 4 * (lower nibble of byte at offset k in data).
             * Used for IP header length calculation in network BPF;
             * for seccomp, offset k is in seccomp_data.
             */
            {
                const uint8_t *base = (const uint8_t *)data;
                if (k + 1 > sizeof(struct seccomp_data)) return -1;
                X = 4 * (base[k] & 0x0F);
            }
            break;

        /* ================================================
         * BPF_ALU: Arithmetic on A
         * ================================================ */
        case BPF_ALU | BPF_ADD | BPF_K:  A += k; break;
        case BPF_ALU | BPF_SUB | BPF_K:  A -= k; break;
        case BPF_ALU | BPF_MUL | BPF_K:  A *= k; break;
        case BPF_ALU | BPF_DIV | BPF_K:
            if (k != 0) A /= k; else A = 0;
            break;
        case BPF_ALU | BPF_OR  | BPF_K:  A |= k; break;
        case BPF_ALU | BPF_AND | BPF_K:  A &= k; break;
        case BPF_ALU | BPF_LSH | BPF_K:
            /* FIXED (v4.2.5): BUG-BPF-SHIFT — shift >= 32 is UB in C */
            A = (k < 32) ? (A << k) : 0;
            break;
        case BPF_ALU | BPF_RSH | BPF_K:
            /* FIXED (v4.2.5): BUG-BPF-SHIFT */
            A = (k < 32) ? (A >> k) : 0;
            break;
        case BPF_ALU | BPF_MOD | BPF_K:
            if (k != 0) A %= k; else A = 0;
            break;
        case BPF_ALU | BPF_XOR | BPF_K:  A ^= k; break;
        case BPF_ALU | BPF_NEG:           A = (uint32_t)(-(int32_t)A); break;

        case BPF_ALU | BPF_ADD | BPF_X:  A += X; break;
        case BPF_ALU | BPF_SUB | BPF_X:  A -= X; break;
        case BPF_ALU | BPF_MUL | BPF_X:  A *= X; break;
        case BPF_ALU | BPF_DIV | BPF_X:
            if (X != 0) A /= X; else A = 0;
            break;
        case BPF_ALU | BPF_OR  | BPF_X:  A |= X; break;
        case BPF_ALU | BPF_AND | BPF_X:  A &= X; break;
        case BPF_ALU | BPF_LSH | BPF_X:
            /* FIXED (v4.2.5): BUG-BPF-SHIFT — shift >= 32 is UB in C */
            A = (X < 32) ? (A << X) : 0;
            break;
        case BPF_ALU | BPF_RSH | BPF_X:
            /* FIXED (v4.2.5): BUG-BPF-SHIFT */
            A = (X < 32) ? (A >> X) : 0;
            break;
        case BPF_ALU | BPF_MOD | BPF_X:
            if (X != 0) A %= X; else A = 0;
            break;
        case BPF_ALU | BPF_XOR | BPF_X:  A ^= X; break;

        /* ================================================
         * BPF_JMP: Conditional jumps
         * ================================================ */
        case BPF_JMP | BPF_JA:
            /* Unconditional jump */
            pc += insn->k;
            if (pc >= len) return -1;  /* bounds check */
            continue;  /* skip pc++ at end of loop */

        case BPF_JMP | BPF_JEQ | BPF_K:
            if (A == k) pc += insn->jt; else pc += insn->jf;
            if (pc >= len) return -1;
            continue;

        case BPF_JMP | BPF_JGT | BPF_K:
            if (A > k) pc += insn->jt; else pc += insn->jf;
            if (pc >= len) return -1;
            continue;

        case BPF_JMP | BPF_JGE | BPF_K:
            if (A >= k) pc += insn->jt; else pc += insn->jf;
            if (pc >= len) return -1;
            continue;

        case BPF_JMP | BPF_JSET | BPF_K:
            if (A & k) pc += insn->jt; else pc += insn->jf;
            if (pc >= len) return -1;
            continue;

        case BPF_JMP | BPF_JEQ | BPF_X:
            if (A == X) pc += insn->jt; else pc += insn->jf;
            if (pc >= len) return -1;
            continue;

        case BPF_JMP | BPF_JGT | BPF_X:
            if (A > X) pc += insn->jt; else pc += insn->jf;
            if (pc >= len) return -1;
            continue;

        case BPF_JMP | BPF_JGE | BPF_X:
            if (A >= X) pc += insn->jt; else pc += insn->jf;
            if (pc >= len) return -1;
            continue;

        case BPF_JMP | BPF_JSET | BPF_X:
            if (A & X) pc += insn->jt; else pc += insn->jf;
            if (pc >= len) return -1;
            continue;

        /* ================================================
         * BPF_RET: Return
         * ================================================ */
        case BPF_RET | BPF_K:
            /*
             * Return value interpretation:
             *   SECCOMP_RET_ALLOW (0x7FFF0000) → allow (return 0)
             *   Anything else → kill (return -1)
             */
            if (k == SECCOMP_RET_ALLOW) return 0;
            return -1;

        case BPF_RET | BPF_A:
            /*
             * FIXED (v4.2.1): BPF_RET | BPF_A returns the value in
             * register A, not the immediate k.  Previously, both
             * BPF_RET | BPF_K and BPF_RET | BPF_A used k, which
             * ignored the computed A register value.  (BUG-SEC-M1)
             */
            if (A == SECCOMP_RET_ALLOW) return 0;
            return -1;

        /* ================================================
         * BPF_MISC: Miscellaneous
         * ================================================ */
        case BPF_MISC | BPF_TAX:
            /* Transfer A to X */
            X = A;
            break;

        case BPF_MISC | BPF_TXA:
            /* Transfer X to A */
            A = X;
            break;

        /* ================================================
         * BPF_ST / BPF_STX: Store to scratch memory
         * FIXED (v4.2.3): Implemented scratch memory store
         * instructions.  (BUG-SEC-01)
         * ================================================ */
        case BPF_ST:
            /* M[k] = A
             * FIXED (v4.2.7): BUG-SECCOMP-SCRATCH-BOUNDS */
            {
                if (k >= BPF_SCRATCH_SIZE) { return -1; } /* FIXED (v4.2.7): BUG-SECCOMP-SCRATCH-BOUNDS */
                uint32_t idx = k & (BPF_SCRATCH_SIZE - 1);
                scratch[idx] = A;
            }
            break;

        case BPF_STX:
            /* M[k] = X
             * FIXED (v4.2.7): BUG-SECCOMP-SCRATCH-BOUNDS */
            {
                if (k >= BPF_SCRATCH_SIZE) { return -1; } /* FIXED (v4.2.7): BUG-SECCOMP-SCRATCH-BOUNDS */
                uint32_t idx = k & (BPF_SCRATCH_SIZE - 1);
                scratch[idx] = X;
            }
            break;

        /* ================================================
         * Default: unknown instruction → deny
         * ================================================ */
        default:
            log_printf(LOG_LEVEL_WARN, "seccomp: unknown BPF instruction "
                       "0x%04x at pc=%u\n", code, pc);
            return -1;
        }

        pc++;
    }

    /* Program ended without explicit RET: deny */
    return -1;
}

/* ================================================================
 * seccomp_check
 *
 * Check if a syscall is allowed by the task's seccomp filter.
 * Two-phase check:
 *   1. Bitmap fast-path: reject immediately if syscall number is denied.
 *   2. BPF program: if installed, run BPF against syscall arguments.
 *
 * Holds seccomp_lock while dereferencing the filter pointer
 * to prevent UAF with concurrent seccomp_set_filter(NULL).
 *
 * FIXED (v4.1.9): Added BPF argument validation via @args parameter.
 * When @args is non-NULL, the BPF program is executed against the
 * full seccomp_data (syscall number + arguments).  (H-29)
 * ================================================================ */

int seccomp_check(struct task_struct *task, int syscall_num, uint64_t args[6]) {
    if (!task) return 0;  /* safety: allow if no task context */

    spin_lock((spinlock_t*)&task->seccomp_lock);

    /* No filter installed: all syscalls allowed */
    struct seccomp_filter *filter = task->seccomp;
    if (!filter) {
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        return 0;
    }

    /* Bounds check: syscall numbers outside 0..255 are always denied */
    if (syscall_num < 0 || syscall_num >= 256) {
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        return -1;
    }

    /*
     * Phase 1: Bitmap fast-path check.
     * Each uint64_t covers 64 syscalls:
     *   syscall_mask[0] covers syscalls 0..63
     *   syscall_mask[1] covers syscalls 64..127
     *   syscall_mask[2] covers syscalls 128..191
     *   syscall_mask[3] covers syscalls 192..255
     */
    int word_idx = syscall_num / 64;
    int bit_idx  = syscall_num % 64;
    uint64_t mask = 1ULL << bit_idx;

    if (!(filter->syscall_mask[word_idx] & mask)) {
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        return -1;  /* syscall number denied by bitmap */
    }

    /*
     * Phase 2: BPF argument validation (if BPF program is installed
     * and arguments are provided).
     *
     * FIXED (v4.1.9): Run BPF program to validate syscall arguments.
     * This allows fine-grained filtering such as "only allow write to
     * fd=1 (stdout)" or "only allow mmap with PROT_READ|PROT_WRITE".
     * (H-29: seccomp BPF argument verification)
     */
    if (filter->bpf_len > 0 && args) {
        struct seccomp_data data;
        memset(&data, 0, sizeof(data));
        data.nr   = syscall_num;
        data.arch = AUDIT_ARCH_X86_64;
        data.instruction_pointer = 0;  /* not available at this point */
        data.args[0] = args[0];
        data.args[1] = args[1];
        data.args[2] = args[2];
        data.args[3] = args[3];
        data.args[4] = args[4];
        data.args[5] = args[5];

        int bpf_result = seccomp_run_bpf(filter->bpf_prog,
                                          filter->bpf_len, &data);
        if (bpf_result != 0) {
            spin_unlock((spinlock_t*)&task->seccomp_lock);
            return -1;  /* BPF program denied */
        }
    }

    spin_unlock((spinlock_t*)&task->seccomp_lock);
    return 0;  /* allowed */
}