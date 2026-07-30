<!-- FIXED (v4.3.6): DOC-001 -->
# AuroraOS Architecture Design Decisions

## 1. Module Signing: ECDSA P-256

### Decision
Use ECDSA on the NIST P-256 (secp256r1) curve for kernel module signature verification.

### Rationale
- P-256 offers 128-bit security level, sufficient for hobby OS
- ECDSA signatures are compact (64 bytes for r||s)
- No external dependencies: all big-integer and EC operations are self-implemented
- Replaced XOR-based placeholder (v4.2.0)

### Implementation
- File: `kernel/module_sign.c` (1300+ lines)
- SHA-256 hash of module data
- ECDSA_verify(P256, pubkey, SHA-256(data), (r, s))
- Solinas reduction for fast modular arithmetic
- Runtime key generation at boot (module_sign_init)
- Enabled via `-DMODULE_SIGN_CHECK` in Makefile

### Limitations
- Private key management is external (signing tool not included)
- No CRL/revocation mechanism
- No key rotation support

## 2. SMP Scheduling: Per-CPU Runqueues

### Decision
Each CPU has its own runqueue with task stealing for load balancing.

### Rationale
- Per-CPU queues avoid lock contention on the global runqueue
- Task stealing is simple and effective for hobby OS scale
- AP cores participate in full scheduling (not just idle)

### Implementation
- File: `kernel/smp.c`
- `per_cpu_rq[NCPU]`: per-CPU runqueues
- `ap_idle_loop()`: AP cores call schedule() + smp_steal_task()
- `smp_steal_task()`: steals highest-vruntime task from CPU0
- IPI for TLB shootdown and reschedule

### Limitations
- No NUMA awareness
- No CFS-style load tracking across CPUs
- Task stealing only from CPU0 (not from other APs)

## 3. TCP Congestion Control: NewReno

### Decision
Implement NewReno as the primary congestion control algorithm.

### Rationale
- NewReno is simple, well-understood, and widely deployed
- Handles partial ACKs in Fast Recovery (unlike basic Reno)
- Sufficient for hobby OS networking needs

### Implementation
- File: `kernel/net/tcp_cong.c`
- States: SLOW_START, AVOIDANCE, FAST_RECOVERY, LOSS
- SACK support for selective acknowledgment
- RTT estimation via EWMA
- Retransmit timeout with exponential backoff

### Limitations
- No CUBIC or BBR
- No ECN support
- No pacing

## 4. BSS Stack Overflow: Root Cause Fix

### Decision
Move kernel stack from .bss to a separate .stack section with guard page.

### Rationale
- Original 32KB stack in .bss overflowed into adjacent BSS variables
- This caused BUG-CURRENT-NULL and BUG-CR3-CACHE
- Moving to .stack section with 64KB + 4KB guard page prevents overflow

### Implementation
- File: `linker.ld` — `.stack` section after `.bss`, 4KB guard page
- File: `kernel/entry.S` — stack canary 0xDEAD0000BEEFCAFE at boot
- File: `kernel/sched.c` — canary check in scheduler_init() and schedule()

## 5. Seccomp: Per-Task BPF Filter

### Decision
Per-task seccomp filter using BPF bytecode, installed via prctl(PR_SET_SECCOMP).

### Rationale
- Thread-safe: per-task filter storage with seccomp_lock
- BPF provides flexible filtering (syscall number + arguments)
- Minimal overhead: check at syscall entry point

### Implementation
- File: `kernel/seccomp.c`
- `prctl(PR_SET_SECCOMP, filter_ptr)` installs filter
- `seccomp_check(task, syscall_num, args)` called at syscall entry
- Returns SECCOMP_RET_ALLOW or SECCOMP_RET_KILL

## 6. KASLR: Kernel Address Space Layout Randomization

### Decision
Randomize kernel .text, .data, heap, stack, and module base addresses.

### Rationale
- Makes ROP/JOP attacks harder
- ChaCha20-based random number generation
- Slide capped to available physical memory

### Implementation
- File: `kernel/aslr.c`
- `kaslr_randomize_kernel_base()`: randomizes .text/.data within 0-510MB
- New PML4 mapping with random offset
- Fallback to fixed address if not enough RAM

## 7. User Memory Access: Safe Copy Wrappers

### Decision
All copy_from_user/copy_to_user must go through safe_copy_* wrappers that validate pointer ranges.

### Rationale
- Prevents TOCTOU attacks
- Catches NULL pointer dereferences
- Prevents kernel address space access through user pointers

### Implementation
- File: `kernel/include/user_access.h`
- `validate_user_range()`: checks NULL, kernel-space, overflow
- `safe_copy_from_user()` / `safe_copy_to_user()`: validated wrappers

## 8. Panic: Register Dump + Stack Backtrace

### Decision
On kernel panic, dump all registers and walk the stack frame chain.

### Rationale
- Essential for debugging
- Frame pointer walking works with -fno-omit-frame-pointer
- No external debugger needed for basic crash analysis

### Implementation
- File: `kernel/panic.c`
- `panic_dump_registers()`: CR2, CR3, RIP, RSP, RBP
- `panic_stack_backtrace()`: walk frame pointer chain (max 32 frames)
- `panic_crash_dump()`: unified crash dump with all information