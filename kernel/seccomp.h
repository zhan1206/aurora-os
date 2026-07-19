/*
 * seccomp.h - System call access control (seccomp-like filtering)
 *
 * Provides a syscall filter for per-task access control.  Each task
 * can have an optional seccomp_filter that specifies which syscalls
 * are allowed.
 *
 * Two filter modes:
 *   1. Bitmap mode: 256-bit bitmap (4 × 64-bit words).  Bit i
 *      set to 1 means syscall i is allowed.  Simple and fast.
 *   2. BPF mode: Classic BPF program that can inspect syscall arguments
 *      (arg0-arg5) in addition to the syscall number.  More powerful
 *      but requires a BPF interpreter.
 *
 * Default behaviour: no filter = all syscalls allowed.
 *
 * FIXED (v4.1.9): Added BPF filter mode with argument-level filtering.
 * Previously only bitmap-based syscall number filtering was supported,
 * which could not restrict specific argument values.  (H-29: seccomp
 * BPF argument verification)
 */
#ifndef SECCOMP_H
#define SECCOMP_H

#include <stdint.h>

/* Forward declaration */
struct task_struct;

/* ================================================================
 * Classic BPF instruction set
 * ================================================================ */

/* BPF instruction classes */
#define BPF_LD    0x00  /* Load into A register */
#define BPF_LDX   0x01  /* Load into X register */
#define BPF_ST    0x02  /* Store A */
#define BPF_STX   0x03  /* Store X */
#define BPF_ALU   0x04  /* ALU operation on A */
#define BPF_JMP   0x05  /* Jump */
#define BPF_RET   0x06  /* Return */
#define BPF_MISC  0x07  /* Miscellaneous */

/* BPF ld/ldx modes */
#define BPF_IMM   0x00  /* Immediate */
#define BPF_ABS   0x20  /* Absolute offset */
#define BPF_IND   0x40  /* Indirect */
#define BPF_MEM   0x60  /* Memory */
#define BPF_LEN   0x80  /* Packet length */
#define BPF_MSH   0xA0  /* Mesh */

/* BPF ALU operations */
#define BPF_ADD   0x00
#define BPF_SUB   0x10
#define BPF_MUL   0x20
#define BPF_DIV   0x30
#define BPF_OR    0x40
#define BPF_AND   0x50
#define BPF_LSH   0x60
#define BPF_RSH   0x70
#define BPF_NEG   0x80
#define BPF_MOD   0x90
#define BPF_XOR   0xA0

/* BPF jump conditions */
#define BPF_JA    0x00  /* Unconditional */
#define BPF_JEQ   0x10  /* Jump if A == k */
#define BPF_JGT   0x20  /* Jump if A > k */
#define BPF_JGE   0x30  /* Jump if A >= k */
#define BPF_JSET  0x40  /* Jump if A & k */

/* BPF source operands */
#define BPF_K     0x00  /* Use immediate value k */
#define BPF_X     0x08  /* Use X register */

/* BPF R-Val (return value size) */
#define BPF_W     0x00  /* 32-bit word */
#define BPF_H     0x08  /* 16-bit halfword */
#define BPF_B     0x10  /* 8-bit byte */

/* BPF misc */
#define BPF_TAX   0x00  /* Transfer A to X */
#define BPF_TXA   0x80  /* Transfer X to A */

/* ================================================================
 * BPF instruction
 * ================================================================ */

struct sock_filter {
    uint16_t code;   /* opcode */
    uint8_t  jt;     /* jump if true */
    uint8_t  jf;     /* jump if false */
    uint32_t k;      /* generic multiuse field */
};

/* ================================================================
 * seccomp_data - data available to BPF filter
 * ================================================================ */

/*
 * Structure passed to the BPF filter.  The BPF program can read
 * any field via BPF_ABS offset.  Layout matches Linux seccomp_data
 * for compatibility with tools that generate BPF filters.
 */
struct seccomp_data {
    int      nr;                   /* [0]  syscall number */
    uint32_t arch;                 /* [4]  AUDIT_ARCH_X86_64 */
    uint64_t instruction_pointer;  /* [8]  RIP at syscall */
    uint64_t args[6];              /* [16] syscall arguments */
};

/* seccomp_data offsets for BPF_ABS loads */
#define SECCOMP_DATA_NR_OFFSET    0
#define SECCOMP_DATA_ARCH_OFFSET  4
#define SECCOMP_DATA_IP_OFFSET    8
#define SECCOMP_DATA_ARG0_OFFSET  16
#define SECCOMP_DATA_ARG1_OFFSET  24
#define SECCOMP_DATA_ARG2_OFFSET  32
#define SECCOMP_DATA_ARG3_OFFSET  40
#define SECCOMP_DATA_ARG4_OFFSET  48
#define SECCOMP_DATA_ARG5_OFFSET  56

/* Architecture identifier (matches Linux AUDIT_ARCH_X86_64) */
#define AUDIT_ARCH_X86_64  0xC000003E

/* ================================================================
 * seccomp return values
 * ================================================================ */

#define SECCOMP_RET_KILL_PROCESS  0x80000000U
#define SECCOMP_RET_KILL_THREAD   0x00000000U
#define SECCOMP_RET_ALLOW         0x7FFF0000U

/* ================================================================
 * Seccomp filter (extended with BPF support)
 * ================================================================ */

/*
 * seccomp_filter: Combined bitmap + BPF filter.
 *
 * Modes:
 *   - bpf_len == 0: bitmap-only mode.  syscall_mask[4] is used.
 *   - bpf_len > 0:  BPF mode.  The BPF program is executed against
 *                    seccomp_data.  bitmap is still checked first as
 *                    a fast-path optimization.
 */
#define SECCOMP_MAX_BPF_LEN  4096

struct seccomp_filter {
    uint64_t syscall_mask[4];     /* bitmap: 256 bits (fast-path) */
    uint16_t bpf_len;             /* number of BPF instructions (0 = none) */
    struct sock_filter bpf_prog[SECCOMP_MAX_BPF_LEN];  /* BPF program */
};

/* ================================================================
 * API
 * ================================================================ */

/*
 * seccomp_set_filter: Install a syscall filter for a task.
 * @task:    Target task (must be current or a child).
 * @filter:  Filter to install (copied into kernel memory).
 * Returns:  0 on success, -1 on error.
 */
int seccomp_set_filter(struct task_struct *task, struct seccomp_filter *filter);

/*
 * seccomp_check: Check if a syscall is allowed by the task's filter.
 * @task:        Task to check.
 * @syscall_num: Syscall number to test.
 * @args:        Syscall arguments (arg0-arg5) for BPF validation.
 *               May be NULL if only bitmap check is needed.
 * Returns:      0 if allowed, -1 if denied.
 */
int seccomp_check(struct task_struct *task, int syscall_num, uint64_t args[6]);

/*
 * seccomp_run_bpf: Execute a BPF program against seccomp_data.
 * @prog:  BPF program instructions.
 * @len:   Number of instructions.
 * @data:  seccomp_data to filter against.
 * Returns: 0 for ALLOW, -1 for KILL.
 */
int seccomp_run_bpf(const struct sock_filter *prog, uint16_t len,
                     const struct seccomp_data *data);

#endif /* SECCOMP_H */