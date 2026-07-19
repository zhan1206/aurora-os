/*
 * sched.h - Process/task control block and scheduler interface
 *
 * Architecture:
 *   - Tasks are organized in a circular linked list (ready queue).
 *   - PID 0: idle task (always READY, loops with HLT).
 *   - PID 1: init task (never runs, reaps orphaned children).
 *   - PIDs 2+: user tasks, allocated via bitmap for O(1) lookup.
 *   - Scheduling: VRFair (CFS/EEVDF-inspired) with vruntime tracking.
 *     Falls back to Round-Robin for compatibility.
 *   - Per-task errno (t_errno) for thread safety.
 *   - COW-aware fork: child shares parent's user pages via clone_current_pml4().
 *   - waitpid: true blocking via TASK_BLOCKED; child exit wakes parent.
 *
 * Locking:
 *   - Single-core: no locks needed for scheduler data structures.
 *   - Interrupt safety: schedule() is called from yield() (syscall context)
 *     or check_resched() (IRQ context). No reentrancy issues on single-core.
 *
 * FIXED (v4.1.4): Added VMA (Virtual Memory Area) tracking to prevent
 *   lazy allocation from bypassing guard pages.  The page fault handler
 *   now checks that the fault address falls within a registered VMA
 *   before allocating a page.  (BUG 3.1)
 */
#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>
#include "types.h"
#include "rbtree.h"
#include "include/trapframe.h"

#define MAX_FDS 16

/* Spinlock type definition (with macro guard to avoid circular dependency
 * with smp.h which also defines spinlock_t). */
#ifndef SPINLOCK_T_DEFINED
#define SPINLOCK_T_DEFINED
typedef struct spinlock {
    volatile uint32_t locked;
} spinlock_t;
#endif /* SPINLOCK_T_DEFINED */

/* CFS/EEVDF-inspired scheduling constants */
#define BASE_SLICE 10  /* base time slice in ticks */

/* Task states */
typedef enum {
    TASK_RUNNING = 0,  /* currently executing */
    TASK_READY   = 1,  /* runnable, in ready queue */
    TASK_BLOCKED = 2,  /* waiting for event */
    TASK_ZOMBIE  = 3,  /* exited, awaiting parent waitpid */
    TASK_DEAD    = 4,  /* fully cleaned up */
} task_state_t;

/* Task flags */
#define TASK_SECCOMP  0x100  /* task has seccomp filter installed */

/* Resource limit indices (rlimit_cur/rlimit_max arrays in task_struct) */
#define RLIMIT_CPU      0
#define RLIMIT_DATA     2
#define RLIMIT_STACK    3
#define RLIMIT_NOFILE   7
#define RLIMIT_AS       9

/* Forward declaration for linked list */
struct task_struct;

/* Children list node */
struct child_node {
    struct task_struct *child;
    struct child_node *next;
};

/*
 * Virtual Memory Area (VMA) - tracks a contiguous region of virtual
 * address space with uniform permissions.  Used by the page fault
 * handler to validate fault addresses before lazy allocation.
 *
 * FIXED (v4.1.4): Added to prevent lazy allocation from bypassing
 * guard pages.  (BUG 3.1)
 */
#define VM_READ       0x1
#define VM_WRITE      0x2
#define VM_EXEC       0x4
#define VM_GROWSDOWN  0x100   /* stack-like: grows downward on fault */

struct vm_area {
    uint64_t vm_start;        /* start address (page-aligned) */
    uint64_t vm_end;          /* end address (page-aligned, exclusive) */
    uint64_t vm_flags;        /* VM_* flags */
    struct vm_area *next;     /* linked list */
};

/* Task control block */
struct task_struct {
    /* --- Context --- */
    uint64_t *rsp;             /* saved stack pointer */
    uint64_t  cr3;             /* physical PML4 base for this task */
    void     *stack_phys;      /* physical base of stack (first page) */
    void     *stack_phys2;     /* second page of stack (for freeing) */
    struct trapframe *current_tf; /* FIXED (v4.1.4): per-task trapframe ptr (was global, BUG 4.4)
                                   * FIXED (v4.1.6): changed from void* to proper type (BUG 6.2) */

    /* --- Identity --- */
    int       pid;             /* process ID */
    char      name[32];        /* process name */

    /* --- State machine --- */
    task_state_t state;        /* current task state */
    int       exit_code;       /* exit status (valid when ZOMBIE) */

    /* --- Process tree --- */
    struct task_struct *parent;          /* parent process */
    struct child_node   *children;       /* linked list of children */
    struct task_struct  *sibling_next;   /* next sibling */

    /* --- Scheduling --- */
    struct task_struct *next;  /* ready queue link (circular, kept for compatibility) */
    struct rb_node    rb_node; /* red-black tree node for vruntime-ordered ready queue */
    int       priority;        /* scheduling priority (0=lowest, 255=highest) */
    int       time_slice;      /* remaining ticks in current time slice */
    int       cpu_mask;        /* allowed CPU mask (bitmap, for SMP) */
    uint64_t  vruntime;        /* virtual runtime for CFS/EEVDF fair scheduling */
    int       need_resched;    /* set to 1 when this task should be preempted */
    int       preempt_count;   /* preemption disable count (>0 = preemption disabled) */

    /* --- Fork state --- */
    int       is_fork_child;   /* 1 if this task is a fresh fork child (returns 0) */

    /* --- File descriptors --- */
    uintptr_t fd_table[MAX_FDS];

    /* --- Signals --- */
    struct signal_state *sig;  /* per-task signal state (lazy alloc) */

    /* --- Memory management --- */
    uint64_t  brk;             /* program break (heap end) for this process */
    uint64_t  mmap_base;       /* next mmap allocation address (ASLR-randomized) */
    struct vm_area *vm_areas;  /* FIXED (v4.1.4): VMA linked list for guard page support (BUG 3.1) */

    /* --- Environment variables --- */
    char      env_keys[16][64];   /* environment variable keys */
    char      env_vals[16][256];  /* environment variable values */
    int       env_count;          /* number of environment variables */

    /* --- Current working directory --- */
    char      cwd[256];        /* current working directory path */

    /* --- Error handling --- */
    int       t_errno;         /* per-task errno (thread-safe) */

    /* --- FPU/SSE state --- */
    /*
     * FIXED (v4.1.4): Per-task FPU/SSE save area for context switch.
     * FXSAVE64 requires 512 bytes with 16-byte alignment.  Without this,
     * floating-point/SSE registers are not preserved across context
     * switches, causing incorrect computation results.  (BUG 4.7)
     *
     * FIXED (v4.1.9): Added fpu_used flag for lazy FPU saving.
     * FPU state is only saved when the task has actually used FPU/SSE
     * instructions.  This avoids the expensive FXSAVE/FXRSTOR on every
     * context switch.  (H-32: FPU state save)
     */
    uint8_t   fpu_state[512] __attribute__((aligned(16)));
    int       fpu_used;        /* 1 if task has used FPU/SSE instructions */

    /* --- Sleep/wakeup --- */
    uint64_t  sleep_until;     /* absolute tick when this task should wake up (0 = not sleeping) */

    /* --- Security --- */
    struct seccomp_filter *seccomp;  /* syscall filter (NULL = all allowed) */
    int       seccomp_lock; /* spinlock for seccomp filter access (FIX: UAF race) */

    /* --- Child list protection --- */
    int       child_lock;      /* spinlock for children list (FIX: waitpid race) */

    /* --- Performance counters --- */
    uint64_t  syscall_count;   /* total syscalls made by this task */
    uint64_t  page_fault_count; /* total page faults this task */
    uint64_t  cpu_ticks;       /* total ticks this task has run */
    uint64_t  cswitch_count;   /* number of context switches into this task */

    /* --- Resource limits --- */
    uint64_t  rlimit_cur[16];  /* soft resource limits (RLIMIT_*) */
    uint64_t  rlimit_max[16];  /* hard resource limits (RLIMIT_*) */
};

#include "signal.h"

/* ============ Per-CPU Run Queue (SMP) ============ */

#ifndef MAX_CPUS
#define MAX_CPUS 8
#endif

struct run_queue {
    struct task_struct *head;  /* circular linked list of ready tasks */
    struct rb_root ready_tree; /* red-black tree for O(log n) vruntime-ordered scheduling */
    int count;                 /* number of tasks in queue */
    spinlock_t lock;           /* FIXED (v4.1.6): proper spinlock_t instead of int */
};

/* ============ Scheduler API ============ */

void scheduler_init(void);
struct task_struct *create_task(void (*fn)(void));
void schedule(void);
void yield(void);
void check_resched(void);

/*
 * schedule_tick: Called from the timer interrupt (PIT/APIC) on each tick.
 * Decrements the current task's time_slice and sets need_resched when
 * the time slice is exhausted. This implements preemptive scheduling.
 */
void schedule_tick(void);

/*
 * preempt_disable / preempt_enable: Control preemption nesting.
 * When preempt_count > 0, the current task cannot be preempted.
 * preempt_enable() checks need_resched and calls schedule() if needed.
 */
void preempt_disable(void);
void preempt_enable(void);

/*
 * smp_schedule: Load-balance tasks across CPUs.
 * Migrates tasks from overloaded CPUs to idle ones.
 * Called periodically from the timer interrupt or IPI.
 */
void smp_schedule(int my_cpu_id);

/*
 * smp_enqueue_task: Add a task to a specific CPU's run queue.
 * Used for CPU affinity and cross-CPU task migration.
 */
void smp_enqueue_task(struct task_struct *t, int cpu_id);

/*
 * smp_dequeue_task: Remove a task from a specific CPU's run queue.
 */
void smp_dequeue_task(struct task_struct *t, int cpu_id);

/* ============ Process lifecycle ============ */

void do_exit_current(int code);

/*
 * waitpid: Wait for a child process to exit.
 * @pid:  PID to wait for, or -1 for any child.
 * @status: output parameter for exit code.
 * @options: 0 for blocking wait.
 */
int waitpid(int pid, int *status, int options);

/* ============ Process tree ============ */

void reparent_children_to_init(struct task_struct *task);
struct task_struct *find_task_by_pid(int pid);

/* ============ File descriptor API ============ */

void fd_table_init(struct task_struct *t);
int fd_alloc(struct task_struct *t, void *filp);
void *fd_get(struct task_struct *t, int fd);
int fd_close(struct task_struct *t, int fd);
int fd_open_path(struct task_struct *t, const char *path);
ssize_t fd_read_fd(struct task_struct *t, int fd, void *buf, size_t count);
int fd_dup(struct task_struct *t, int oldfd);
int fd_dup2(struct task_struct *t, int oldfd, int newfd);
void fd_close_all(struct task_struct *t);

/* ============ Globals ============ */

extern struct task_struct *current;
extern struct run_queue per_cpu_rq[MAX_CPUS];
extern uint64_t min_vruntime;  /* minimum virtual runtime across all tasks */
extern int smp_sched_ready;    /* set to 1 after smp_init configures GS */

#endif /* SCHED_H */
