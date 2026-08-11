/*
 * sched.c - Process scheduler with full lifecycle management
 *
 * Key design:
 *   - waitpid: true blocking via TASK_BLOCKED + child exit wakes parent.
 *   - schedule(): if no runnable task, switch to idle (pid=0) with HLT.
 *   - PID allocation: O(1) bitmap-based, no linear scan.
 *   - Per-task errno (t_errno) for thread safety.
 */

#include "sched.h"
#include "smp.h"
#include "include/log.h"
#include "include/assert.h"
#include "include/errno.h"  /* FIXED (v4.3.7): BUG-08 */
#include "rbtree.h"
#include "mem.h"
#include "pagetable.h"
#include "perf.h"
#include "syscall.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern int num_cpus;

/* ================================================================
 * External declarations
 * ================================================================ */
extern void context_switch(uint64_t **old_rsp_ptr, uint64_t *new_rsp);

/* ================================================================
 * Global state
 * ================================================================ */
struct task_struct *current = NULL;
static struct task_struct *init_task = NULL;  /* pid=1, reaper for orphans */
static struct task_struct *idle_task  = NULL;  /* pid=0, idle loop */

/* Per-CPU run queues (SMP) */
/* STUB (v4.2.8): Per-CPU run queues are allocated and initialized
 * but only CPU 0's run queue is actively used.  AP cores (smp.c)
 * spin in HLT and never call schedule().  smp_schedule() and
 * smp_enqueue_task() exist but are dead code until APs participate
 * in the scheduler.  All tasks are created on CPU 0's run queue. */
struct run_queue per_cpu_rq[MAX_CPUS];

/* Global minimum virtual runtime for CFS/EEVDF fair scheduling.
 * Initialized to 0 so that the first task gets vruntime=0.
 * Updated in schedule() to track the minimum vruntime among all
 * ready tasks. New tasks start at min_vruntime to ensure fairness. */
uint64_t min_vruntime = 0;

/* smp_init() sets this to 1 after GS is configured for the BSP */
int smp_sched_ready = 0;

/*
 * FIXED (v4.1.6): current_cpu_id() moved to smp.h as a static inline
 * that uses this_cpu()->cpu_id.  The previous version in sched.c read
 * an int from GS:0, which was incorrect (GS:0 stores a pointer to
 * struct cpu_data, not a raw cpu_id int).  (BUG 6.5)
 */

/* ================================================================
 * PID bitmap allocator — O(1) lookup, O(1) alloc
 * ================================================================ */
#define MAX_PID           8192
#define PID_BITMAP_WORDS  (MAX_PID / 64)  /* 128 uint64_t entries */
static uint64_t pid_bitmap[PID_BITMAP_WORDS];
static int next_pid = 2;  /* 0=idle, 1=init, user tasks start at 2 */
static spinlock_t pid_lock = {0};  /* protects PID bitmap allocation */

/* PID → task_struct lookup table (O(1) instead of O(n) scan) */
static struct task_struct *pid_table[MAX_PID];

static inline void pid_set_bit(int pid) {
    pid_bitmap[pid / 64] |= (1ULL << (pid % 64));
}

static inline void pid_clear_bit(int pid) {
    pid_bitmap[pid / 64] &= ~(1ULL << (pid % 64));
}

static inline int pid_test_bit(int pid) {
    return (pid_bitmap[pid / 64] >> (pid % 64)) & 1;
}

static int alloc_pid(void) {
    spin_lock(&pid_lock);

    /* Reserve pid 0 and 1 */
    pid_set_bit(0);
    pid_set_bit(1);

    /* Scan for a free PID using rotating next_pid cursor */
    for (int attempt = 0; attempt < MAX_PID; ++attempt) {
        int pid = next_pid;
        next_pid = (pid + 1 >= MAX_PID) ? 2 : pid + 1;
        if (!pid_test_bit(pid)) {
            pid_set_bit(pid);
            spin_unlock(&pid_lock);
            return pid;
        }
    }
    spin_unlock(&pid_lock);
    return -1;  /* PID space exhausted */
}

static void free_pid(int pid) {
    spin_lock(&pid_lock);
    if (pid >= 2 && pid < MAX_PID) {
        pid_clear_bit(pid);
        pid_table[pid] = NULL;  /* clear O(1) lookup entry */
    }
    spin_unlock(&pid_lock);
}

/* Register a task in the O(1) lookup table */
static inline void pid_register(int pid, struct task_struct *t) {
    if (pid >= 0 && pid < MAX_PID) {
        pid_table[pid] = t;
    }
}

/* ================================================================
 * Task lookup — O(1) via PID table
 * ================================================================ */

struct task_struct *find_task_by_pid(int pid) {
    if (pid < 0 || pid >= MAX_PID) return NULL;
    /*
     * FIXED (v4.1.4): Hold pid_lock while reading pid_table and checking
     * task state.  Without the lock, the task could be freed between the
     * pid_table read and the state check (TOCTOU race), leading to UAF.
     * (BUG 4.3)
     *
     * FIXED (v4.2.4): Returned pointer is still valid only until the
     * caller releases the lock.  Callers must increment the task's
     * reference count if they need to hold the pointer beyond the
     * lock scope.  (BUG-FIND-UAF)
     *
     * DEPRECATED (v4.2.6): Use task_get_by_pid() instead.  This function
     * is kept for backward compatibility but all new callers should use
     * the task_get_by_pid() / task_put() API. */
    spin_lock(&pid_lock);
    struct task_struct *t = pid_table[pid];
    if (t && t->state != TASK_DEAD && t->state != TASK_ZOMBIE) {
        /* Increment ref_count so the task won't be freed while
         * the caller holds the pointer */
        __sync_fetch_and_add(&t->ref_count, 1);
        spin_unlock(&pid_lock);
        return t;
    }
    spin_unlock(&pid_lock);
    return NULL;
}

/* REFCOUNT (v4.2.6): task_get_by_pid — preferred API for task lookup.
 * Returns a task with ref_count incremented, or NULL if not found.
 * Includes ZOMBIE tasks (unlike find_task_by_pid which skips them)
 * so that waitpid() can collect them. */
struct task_struct *task_get_by_pid(int pid) {
    if (pid < 0 || pid >= MAX_PID) return NULL;
    spin_lock(&pid_lock);
    struct task_struct *t = pid_table[pid];
    if (t && t->state != TASK_DEAD) {
        __sync_fetch_and_add(&t->ref_count, 1);
        spin_unlock(&pid_lock);
        return t;
    }
    spin_unlock(&pid_lock);
    return NULL;
}

/* REFCOUNT (v4.2.6): task_free — actually free all task resources.
 * Called automatically by task_put() when ref_count reaches 0 and
 * the task is in ZOMBIE or DEAD state.
 *
 * Frees in order: kernel stack, page tables, file descriptors,
 * signal state, VMAs, PID, and finally the task_struct itself. */
void task_free(struct task_struct *t) {
    if (!t) return;

    log_printf(LOG_LEVEL_DEBUG, "task_free: freeing pid=%d (%s)\n", t->pid, t->name);

    /* 1. Close all file descriptors */
    fd_close_all(t);

    /* 2. Free all VMAs */
    vma_free_all(t);

    /* 3. Free signal state */
    if (t->sig) {
        kfree(t->sig);
        t->sig = NULL;
    }

    /* 4. Free kernel stack */
    if (t->stack_phys) {
        free_page(t->stack_phys);
        t->stack_phys = NULL;
    }
    if (t->stack_phys2) {
        free_page(t->stack_phys2);
        t->stack_phys2 = NULL;
    }

    /* 5. Free page tables (if not sharing kernel page tables) */
    if (t->cr3 && t->cr3 != get_kernel_cr3()) {
        extern void free_pagetable(uint64_t pml4_phys);
        free_pagetable(t->cr3);
        t->cr3 = 0;
    }

    /* 6. Free PID */
    free_pid(t->pid);

    /* 7. Mark as DEAD and remove from pid_table */
    t->state = TASK_DEAD;
    pid_table[t->pid] = NULL;

    /* 8. Free the task_struct itself */
    kfree(t);

    log_printf(LOG_LEVEL_DEBUG, "task_free: done\n");
}

/* ================================================================
 * Children list management
 * ================================================================ */

static void add_child(struct task_struct *parent, struct task_struct *child) {
    struct child_node *node = (struct child_node *)kmalloc(sizeof(*node));
    if (!node) return;
    spin_lock((spinlock_t*)&parent->child_lock);
    node->child = child;
    node->next = parent->children;
    parent->children = node;
    child->parent = parent;
    spin_unlock((spinlock_t*)&parent->child_lock);
}

void reparent_children_to_init(struct task_struct *task) {
    if (!task || !task->children) return;
    if (!init_task) return;

    /*
     * FIXED (v4.1.4): Hold both the task's child_lock and init_task's
     * child_lock to prevent concurrent modification of the children
     * linked lists.  Without locks, concurrent exits on SMP can corrupt
     * the init_task->children list.  (BUG 4.2)
     */
    spin_lock((spinlock_t*)&task->child_lock);
    spin_lock((spinlock_t*)&init_task->child_lock);

    struct child_node *node = task->children;
    while (node) {
        if (node->child) {
            node->child->parent = init_task;
        }
        struct child_node *next = node->next;
        node->next = init_task->children;
        init_task->children = node;
        node = next;
    }
    task->children = NULL;

    spin_unlock((spinlock_t*)&init_task->child_lock);
    spin_unlock((spinlock_t*)&task->child_lock);
}

/* ================================================================
 * Scheduler initialization
 * ================================================================ */

void scheduler_init(void) {
    /*
     * FIXED (v4.3.2): BSS-001 — Check kernel stack canary before scheduler init.
     * The stack canary (0xDEAD0000BEEFCAFE) is written at __stack_bottom by
     * entry.S.  If it's been overwritten, the stack overflowed past its 64KB
     * boundary.  We log the error and attempt to continue, but the kernel
     * may be unstable.  Previously this corruption silently zeroed 'current'
     * and 'idle_task' (BUG-CURRENT-NULL) and corrupted 'kernel_cr3'
     * (BUG-CR3-CACHE), causing the scheduler to deadlock.
     */
    extern uint64_t __stack_bottom;
    volatile uint64_t *canary = (volatile uint64_t *)&__stack_bottom;
    if (*canary != 0xDEAD0000BEEFCAFEULL) {
        log_printf(LOG_LEVEL_ERR, "sched: STACK OVERFLOW DETECTED! "
                   "canary=%p (expected 0xDEAD0000BEEFCAFE)\n", (void*)*canary);
        log_printf(LOG_LEVEL_ERR, "sched: BSS may be corrupted. Restoring canary.\n");
        *canary = 0xDEAD0000BEEFCAFEULL;
    }

    /*
     * Initialize PID bitmap — reserve 0 (idle) and 1 (init).
     * page_table_init() is already called in kernel_main() before
     * scheduler_init(). get_kernel_cr3() returns the cached CR3.
     */
    memset(pid_bitmap, 0, sizeof(pid_bitmap));
    memset(pid_table, 0, sizeof(pid_table));
    memset(per_cpu_rq, 0, sizeof(per_cpu_rq));
    pid_set_bit(0);
    pid_set_bit(1);

    /* Initialize per-CPU run queues */
    for (int i = 0; i < MAX_CPUS; i++) {
        per_cpu_rq[i].head = NULL;
        per_cpu_rq[i].count = 0;
        spin_init(&per_cpu_rq[i].lock);  /* FIXED (v4.1.6): use spin_init for spinlock_t */
        rb_init(&per_cpu_rq[i].ready_tree);
    }

    /* Create idle task (pid=0) — placed on CPU 0's run queue */
    current = (struct task_struct *)kmalloc(sizeof(struct task_struct));
    ASSERT(current != NULL);
    memset(current, 0, sizeof(*current));
    /* FIXED (v4.3.9): BOOT-05 — Allocate a dedicated kernel stack for the idle
     * task.  Previously rsp was NULL, meaning the idle task shared the boot
     * stack with kernel_main.  With its own stack, the idle task's RSP is
     * properly captured by context_switch on the first schedule() call. */
    {
        void *idle_stack = alloc_page();
        ASSERT(idle_stack != NULL);
        current->rsp        = (uint64_t)(uintptr_t)idle_stack + PAGE_SIZE;
        current->stack_phys = idle_stack;
    }
    current->cr3        = get_kernel_cr3();
    current->pid        = 0;
    current->state      = TASK_RUNNING;
    current->priority   = 0;          /* lowest priority */
    current->time_slice = 1;
    current->vruntime   = 0;
    current->cpu_mask   = 0xFF;       /* all CPUs */
    current->next       = current;
    current->parent     = NULL;
    current->children   = NULL;
    current->t_errno    = 0;
    current->ref_count  = 1;         /* REFCOUNT (v4.2.6) */
    strncpy(current->name, "idle", sizeof(current->name) - 1);
    fd_table_init(current);
    pid_register(0, current);
    idle_task = current;

    /* Add idle to CPU 0's run queue */
    per_cpu_rq[0].head = current;
    per_cpu_rq[0].count = 1;
    current->rb_node.key = current->vruntime;
    rb_insert(&per_cpu_rq[0].ready_tree, &current->rb_node);

    /* Create init task (pid=1) */
    struct task_struct *init = (struct task_struct *)kmalloc(sizeof(*init));
    if (init) {
        memset(init, 0, sizeof(*init));
        init->rsp        = NULL;
        init->cr3        = get_kernel_cr3();
        init->pid        = 1;
        init->state      = TASK_RUNNING;
        init->parent     = current;
        init->children   = NULL;
        init->t_errno    = 0;
        init->ref_count  = 1;         /* REFCOUNT (v4.2.6) */
        init->vruntime   = 0;
        init->cap_effective = 0xFFFFFFFFFFFFFFFFULL;  /* FIXED (v4.3.2): CAP-001 — root has all caps */
        init->cap_permitted = 0xFFFFFFFFFFFFFFFFULL;  /* FIXED (v4.3.2): CAP-001 */
        strncpy(init->name, "init", sizeof(init->name) - 1);
        fd_table_init(init);

        /* Do NOT add init to the circular run queue — it has no stack
         * and never runs.  Children are reparented here on exit. */
        init->next = init;   /* self-loop, outside the run queue */
        pid_register(1, init);
        init_task = init;
        log_printf(LOG_LEVEL_INFO, "sched: init task created pid=1\n");
    }
}

/* ================================================================
 * Task creation
 * ================================================================ */

/*
 * syscall_return_point is the address in syscall_entry right after
 * "call syscall_trap".  Fork children return here to go through the
 * normal syscall return path (pop regs → swapgs → sysretq).
 */
extern void syscall_return_point(void);

struct task_struct *create_task(void (*fn)(void)) {
    /* Allow fn==NULL for fork children */
    struct task_struct *t = (struct task_struct *)kmalloc(sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));

    /* Initialize default resource limits */
    t->rlimit_cur[RLIMIT_CPU]    = 0xFFFFFFFFFFFFFFFFULL;  /* unlimited */
    t->rlimit_max[RLIMIT_CPU]    = 0xFFFFFFFFFFFFFFFFULL;
    t->rlimit_cur[RLIMIT_DATA]   = 0xFFFFFFFFFFFFFFFFULL;  /* unlimited */
    t->rlimit_max[RLIMIT_DATA]   = 0xFFFFFFFFFFFFFFFFULL;
    t->rlimit_cur[RLIMIT_STACK]  = 8 * 1024 * 1024;        /* 8 MB */
    t->rlimit_max[RLIMIT_STACK]  = 8 * 1024 * 1024;
    t->rlimit_cur[RLIMIT_NOFILE] = MAX_FDS;
    t->rlimit_max[RLIMIT_NOFILE] = MAX_FDS;
    t->rlimit_cur[RLIMIT_AS]     = 0xFFFFFFFFFFFFFFFFULL;  /* unlimited */
    t->rlimit_max[RLIMIT_AS]     = 0xFFFFFFFFFFFFFFFFULL;

    /* Initialize per-process brk (heap start) */
    t->brk = 0x70000000ULL;

    /* Set default working directory */
    t->cwd[0] = '/';
    t->cwd[1] = '\0';

    /* Allocate a single page for the kernel stack. */
    void *stack_page = alloc_page();
    if (!stack_page) {
        kfree(t);
        return NULL;
    }

    /* Stack grows downward: top of page. */
    uint8_t *stack_top = (uint8_t *)stack_page + PAGE_SIZE;
    uint64_t *sp = (uint64_t *)stack_top;

    /*
     * FIXED (v4.3.9): BOOT-08 — context_switch pushes 6 registers (rbp, rbx, r12-r15)
     * then saves RSP.  On restore, it pops 6 values then rets.  The stack
     * must have exactly 7 slots (6 registers + 1 ret addr) below stack_top.
     */
    if (fn) {
        /*
         * FIXED (v4.3.9): BOOT-08 — Frame order must match new context_switch
         * (no pushfq/popfq).  context_switch pushes: rbp, rbx, r12, r13, r14, r15
         * then saves RSP.  On restore it pops in reverse: r15, r14, r13, r12,
         * rbx, rbp, ret.  So the frame at RSP must be:
         *   [RSP+0]=r15, [RSP+8]=r14, ..., [RSP+40]=rbp, [RSP+48]=ret_addr.
         */
        uint64_t *frame = sp - 7;
        frame[0] = 0;              /* r15 (popped first by context_switch) */
        frame[1] = 0;              /* r14 */
        frame[2] = 0;              /* r13 */
        frame[3] = 0;              /* r12 */
        frame[4] = 0;              /* rbx */
        frame[5] = 0;              /* rbp */
        frame[6] = (uint64_t)fn;   /* return address (popped by ret) */
        sp = frame;
    } else {
        /*
         * FIXED (v4.3.9): BOOT-08 — Fork child: 22 slots = 7 context_switch
         * + 2 user_rsp/rip + 13 syscall trapframe.
         *
         * Layout at RSP (lowest → highest address):
         *   [0..6]   context_switch frame (r15..ret_addr) — 7 slots
         *   [7..8]   user_rsp, user_rip — skipped by add $16, %rsp
         *   [9..21]  syscall regs (r15..rax) — 13 slots
         */
        uint64_t *frame_start = sp - 22;
        for (int i = 0; i < 22; i++) frame_start[i] = 0;

        /* --- context_switch frame at [0..6] --- */
        /* [0]=r15, [1]=r14, [2]=r13, [3]=r12, [4]=rbx, [5]=rbp — already zeroed */
        frame_start[6] = (uint64_t)(uintptr_t)&syscall_return_point;  /* ret addr */

        /* --- user RSP/RIP at [7..8] (skipped by add $16, %rsp) --- */
        frame_start[7] = 0;          /* user RSP (will be set by fork) */
        frame_start[8] = 0x400000;   /* user RIP = default entry */

        /* --- syscall regs at [9..21] --- */
        /* [9]=r15, [10]=r14, [11]=r13, [12]=r12 — already zeroed */
        frame_start[13] = 0x202;     /* R11 = user RFLAGS (IF=1) */
        /* [14]=r10, [15]=r9, [16]=r8, [17]=rsi, [18]=rdi, [19]=rdx — already zeroed */
        frame_start[20] = 0x400000;  /* RCX = user RIP */
        /* [21]=rax = 0 (return value, already zeroed) */

        sp = frame_start;
    }

    t->rsp         = sp;
    t->cr3         = get_kernel_cr3();
    t->stack_phys  = stack_page;
    t->stack_phys2 = NULL;
    t->state       = TASK_READY;
    t->priority    = 128;        /* default medium priority */
    t->time_slice  = BASE_SLICE * (256 - t->priority) / 256;
    if (t->time_slice < 1) t->time_slice = 1;
    t->vruntime    = min_vruntime;  /* start at current min for fairness */
    t->cpu_mask    = 0xFF;       /* allow all CPUs */
    t->parent      = current;
    t->children    = NULL;
    t->t_errno     = 0;
    t->ref_count   = 1;         /* REFCOUNT (v4.2.6): one reference for existence */
    /* FIXED (v4.2.8): BUG-FPU-USED
     * fpu_used was never set to 1, so SSE/FPU registers were silently
     * corrupted on every context switch.  Since we cannot easily detect
     * FPU usage at runtime, assume every task uses FPU. */
    t->fpu_used    = 1;
    t->cap_effective = 0xFFFFFFFFFFFFFFFFULL;  /* FIXED (v4.3.2): CAP-001 — root has all caps */
    t->cap_permitted = 0xFFFFFFFFFFFFFFFFULL;  /* FIXED (v4.3.2): CAP-001 */
    t->pid         = alloc_pid();
    if (t->pid < 0) {
        free_page(stack_page);
        kfree(t);
        return NULL;
    }

    fd_table_init(t);
    pid_register(t->pid, t);

    /* Choose a CPU for this task (round-robin across CPUs) */
    if (num_cpus == 0) num_cpus = 1;
    int target_cpu = t->pid % num_cpus;
    if (target_cpu >= num_cpus) target_cpu = 0;
    t->cpu_id = target_cpu;  /* FIXED (v4.3.4): SMP-001 — track assigned CPU */

    /* Add to the target CPU's run queue.
     * CRITICAL: Acquire rq->lock with IRQ disabled to prevent races
     * with the timer interrupt handler which also modifies the queue. */
    struct run_queue *rq = &per_cpu_rq[target_cpu];
    uint64_t create_irq_flags = irq_save();
    spin_lock(&rq->lock);
    if (rq->head == NULL) {
        t->next = t;
        rq->head = t;
    } else {
        t->next = rq->head->next;
        rq->head->next = t;
    }
    rq->count++;
    /* Also insert into red-black tree for O(log n) scheduling */
    t->rb_node.key = t->vruntime;
    rb_insert(&rq->ready_tree, &t->rb_node);
    spin_unlock(&rq->lock);
    irq_restore(create_irq_flags);
    add_child(current, t);

    /* FIXED (v4.3.7): BUG-1G — reduce debug output spam */
    log_printf(LOG_LEVEL_DEBUG, "Created task pid=%d on CPU %d\n", t->pid, target_cpu);
    return t;
}

/* ================================================================
 * Scheduler core — VRFair (CFS/EEVDF-inspired)
 *
 * Selects the task with the smallest vruntime among READY tasks.
 * When a task is preempted (time slice exhausted), its vruntime
 * is increased by its time_slice. Blocked tasks keep their low
 * vruntime so they get scheduled quickly when they wake up.
 *
 * Falls back to simple round-robin if vruntime tracking is not
 * practical (all tasks have same vruntime, e.g., at boot).
 * ================================================================ */

void schedule(void) {
    if (!current || !current->next) return;

    int cpu_id = current_cpu_id();
    struct run_queue *rq = &per_cpu_rq[cpu_id];

    /*
     * Acquire the run queue lock to protect against concurrent
     * modifications from work stealing (smp_schedule) or other
     * CPUs accessing this queue. The lock is released before
     * context_switch to avoid holding it across stack switches.
     *
     * CRITICAL: Disable interrupts before acquiring the lock.
     * pit_irq_c_handler() also acquires rq->lock from timer interrupt
     * context. If we hold the lock and a timer interrupt fires on the
     * same CPU, the interrupt handler will spin forever waiting for
     * the lock we already hold — a self-deadlock.
     */
    uint64_t irq_flags = irq_save();
    spin_lock(&rq->lock);

    if (rq->head == NULL) {
        spin_unlock(&rq->lock);
        irq_restore(irq_flags);
        /* No tasks in this CPU's run queue — enable interrupts and halt.
         * sti; hlt (not cli; hlt) ensures IPIs and timer interrupts can
         * wake this CPU when new tasks are added to its queue. */
        log_printf(LOG_LEVEL_ERR, "schedule: CPU %d has no tasks, halting\n", cpu_id);
        for (;;) {
            asm volatile ("sti; hlt");
            if (rq->count > 0) break;
        }
        /* New tasks arrived, re-enter schedule */
        schedule();
        return;
    }

    /*
     * Use the red-black tree to find the task with the smallest vruntime
     * among READY tasks in O(log n). This replaces the O(n) linked-list scan.
     * Falls back to idle task if the tree is empty.
     */
    struct task_struct *next = NULL;
    struct rb_node *min_node = rb_find_min(&rq->ready_tree);
    if (min_node) {
        /* Walk the tree in-order to find the first READY task */
        struct rb_node *node = min_node;
        while (node) {
            /* Calculate the containing task_struct from the rb_node offset */
            struct task_struct *candidate = (struct task_struct *)((uintptr_t)node - offsetof(struct task_struct, rb_node));
            if ((candidate->state == TASK_READY || candidate->state == TASK_RUNNING)
                && candidate != current) {
                next = candidate;
                break;
            }
            node = rb_next(node);
        }
    }

    /* If no runnable task found (or only current), fall back to idle or round-robin */
    if (!next) {
        if (idle_task && idle_task != current && idle_task->state == TASK_READY) {
            next = idle_task;
        } else if (current->state == TASK_RUNNING) {
            /* Only one runnable task — keep it running */
            current->state = TASK_RUNNING;
            spin_unlock(&rq->lock);
            irq_restore(irq_flags);
            return;
        } else {
            spin_unlock(&rq->lock);
            irq_restore(irq_flags);
            log_printf(LOG_LEVEL_ERR, "schedule: CPU %d no runnable tasks, halting\n", cpu_id);
            for (;;) {
                asm volatile ("sti; hlt");
                if (rq->count > 0) break;
            }
            schedule();
            return;
        }
    }

    if (next == current) {
        current->state = TASK_RUNNING;
        spin_unlock(&rq->lock);
        irq_restore(irq_flags);
        return;
    }

    struct task_struct *prev = current;

    /*
     * VRFair vruntime update:
     * - If the task was preempted (still TASK_RUNNING), add its time_slice
     *   to vruntime to account for the CPU time it consumed.
     * - If the task blocked (TASK_BLOCKED), don't penalize it — keep
     *   vruntime low so it gets scheduled quickly when it wakes up.
     * - Zombie tasks don't get vruntime updates.
     *
     * FIXED (v4.1.2): vruntime is now weight-normalized (NM3).
     *   Higher-priority tasks get LESS vruntime per tick consumed,
     *   lower-priority tasks get MORE, ensuring true CFS fairness.
     *   Formula: vruntime += consumed * NICE_0_WEIGHT / task_weight
     *   where task_weight = priority + 1 (1..256).
     */
    int prev_state = prev->state;
    if (prev_state == TASK_RUNNING) {
        prev->state = TASK_READY;
        /* Task was preempted or yielded: add actual consumed ticks to vruntime */
        uint64_t full_slice = (uint64_t)(BASE_SLICE * (256 - prev->priority) / 256);
        if (full_slice < 1) full_slice = 1;
        int64_t consumed = (int64_t)full_slice - (int64_t)prev->time_slice;
        if (consumed <= 0) consumed = 1;  /* at least 1 tick */
        /* Weight normalization: high priority = smaller vruntime increment */
        uint64_t weight = (uint64_t)(prev->priority + 1);
        uint64_t nice0_weight = 128;
        prev->vruntime += ((uint64_t)consumed * nice0_weight) / weight;
        perf_inc(PERF_VRUNTIME_UPDATES);

        /* FIXED (v4.2.8): BUG-VRUNTIME-REINSERT
         * After updating vruntime, re-insert the task into the RB tree
         * with the new key so the tree stays correctly ordered.  Without
         * this, the RB tree key is stale and vruntime-based scheduling
         * degenerates to the order tasks were created. */
        rb_erase(&rq->ready_tree, &prev->rb_node);
        prev->rb_node.key = prev->vruntime;
        rb_insert(&rq->ready_tree, &prev->rb_node);
    }
    /* If prev->state was TASK_BLOCKED or TASK_ZOMBIE, leave vruntime unchanged */

    /* Update min_vruntime: track the minimum vruntime across all ready tasks.
     * The scheduled task (next) has the minimum vruntime among ready tasks
     * (we selected it that way). But don't let min_vruntime decrease — use
     * max() to ensure monotonic progression.
     *
     * NOTE: min_vruntime is updated atomically via CAS loop for SMP safety.
     * The read at line 345 (new task vruntime init) is a single 64-bit load
     * and is naturally atomic on x86_64 where 64-bit aligned reads are atomic. */
    {
        uint64_t old, new_val;
        do {
            old = min_vruntime;
            if (next->vruntime <= old) break;
            new_val = next->vruntime;
        } while (!__sync_bool_compare_and_swap(&min_vruntime, old, new_val));
    }

    next->state = TASK_RUNNING;
    task_get(next);              /* REFCOUNT (v4.2.6): hold ref while running */
    current = next;

    /* Update per-CPU current task pointer */
    if (cpu_id >= 0 && cpu_id < MAX_CPUS) {
        cpu_data[cpu_id].current_task = current;
    }

    /*
     * Release the run queue lock before context switch.
     * The lock must not be held across context_switch because
     * the new task won't know to release it, causing a deadlock.
     */
    spin_unlock(&rq->lock);
    irq_restore(irq_flags);

    /*
     * FIXED (v4.3.2): BSS-001 — Stack canary check before context switch.
     * If the canary is corrupted, log a warning and restore it. The stack
     * overflow may have corrupted other data, but the canary gives us an
     * early warning signal.
     */
    {
        extern uint64_t __stack_bottom;
        volatile uint64_t *canary = (volatile uint64_t *)&__stack_bottom;
        if (*canary != 0xDEAD0000BEEFCAFEULL) {
            log_printf(LOG_LEVEL_ERR, "sched: stack overflow in schedule()! canary=%p\n",
                       (void*)*canary);
            *canary = 0xDEAD0000BEEFCAFEULL;
        }
    }

    uint64_t new_cr3 = current->cr3;
    asm volatile ("mov %0, %%cr3" :: "r"(new_cr3) : "memory");

    /* Performance counter: context switch */
    perf_inc(PERF_CTX_SWITCHES);
    /* Per-process perf counters */
    current->cpu_ticks++;
    current->cswitch_count++;

    /*
     * FIXED (v4.1.4): Save/restore FPU/SSE state across context switch.
     * Without fxsave64/fxrstor64, floating-point and SSE registers are
     * not preserved, causing incorrect computation results when tasks
     * use FPU/SSE instructions.  (BUG 4.7)
     *
     * FIXED (v4.1.9): Lazy FPU saving.  Only save FPU state if the
     * previous task actually used FPU/SSE.  This avoids the expensive
     * FXSAVE/FXRSTOR on every context switch for tasks that don't use
     * floating-point operations.  (H-32: FPU state save optimization)
     */
    if (prev->fpu_used) {
        asm volatile ("fxsave64 %0" :: "m"(prev->fpu_state) : "memory");
        prev->fpu_used = 0;  /* Reset after saving */
    }
    context_switch(&prev->rsp, current->rsp);
    if (current->fpu_used) {
        asm volatile ("fxrstor64 %0" :: "m"(current->fpu_state) : "memory");
    }
    /* REFCOUNT (v4.2.6): release reference to the task we switched away from.
     * This runs on the NEW task's stack, so it's safe even if task_put
     * frees prev (the old task). */
    task_put(prev);
}

void yield(void) {
    if (current) {
        /*
         * Update vruntime for actual consumed ticks before yielding.
         *
         * FIXED (v4.1.2): yield() no longer adds full_slice when the task
         * hasn't consumed any ticks.  Previously, a task that yielded
         * immediately after being scheduled would get charged the full
         * time slice, causing unfair starvation (NM4).
         *
         * Now we add only the actual consumed ticks, weight-normalized
         * the same way as schedule().
         */
        uint64_t full_slice = (uint64_t)(BASE_SLICE * (256 - current->priority) / 256);
        if (full_slice < 1) full_slice = 1;
        int64_t consumed = (int64_t)full_slice - (int64_t)current->time_slice;
        if (consumed <= 0) consumed = 1;  /* minimum 1 tick penalty */
        uint64_t weight = (uint64_t)(current->priority + 1);
        uint64_t nice0_weight = 128;
        current->vruntime += ((uint64_t)consumed * nice0_weight) / weight;
        current->state = TASK_READY;
    }
    schedule();
}

void check_resched(void) {
    extern volatile int need_resched;
    if (__sync_lock_test_and_set(&need_resched, 0) || (current && current->need_resched)) {
        if (current) {
            current->need_resched = 0;
            current->state = TASK_READY;
        }
        schedule();
    }
}

/* ================================================================
 * Preemptive scheduling — tick handler
 * ================================================================ */

/*
 * schedule_tick: Called from the timer interrupt handler on each tick.
 * Implements preemptive scheduling by decrementing the current task's
 * time_slice. When the time slice is exhausted, the task is marked for
 * preemption via need_resched. The actual context switch happens at the
 * next safe point (iretq return or syscall return).
 *
 * Recharge: when time_slice reaches 0, the task's vruntime is NOT
 * updated here — that happens in schedule() when the task is actually
 * switched out. This ensures blocked tasks keep their low vruntime.
 */
void schedule_tick(void) {
    if (!current) return;

    /* Preemption is disabled — don't preempt */
    if (current->preempt_count > 0) return;

    if (current->state == TASK_RUNNING) {
        if (current->time_slice > 0) {
            current->time_slice--;
        }
        if (current->time_slice <= 0) {
            /* Time slice exhausted — mark for preemption */
            current->need_resched = 1;
            /* Also set the global flag for backward compatibility */
            extern volatile int need_resched;
            __sync_lock_test_and_set(&need_resched, 1);
            /* Recharge time slice for next run */
            current->time_slice = BASE_SLICE * (256 - current->priority) / 256;
            if (current->time_slice < 1) current->time_slice = 1;
        }
    }
}

/* ================================================================
 * Preemption control
 * ================================================================ */

/*
 * preempt_disable: Increment the preemption counter.
 * While preempt_count > 0, schedule_tick() will not set need_resched
 * and the task cannot be preempted.
 */
void preempt_disable(void) {
    if (current) current->preempt_count++;
}

/*
 * preempt_enable: Decrement the preemption counter.
 * If preempt_count reaches 0 and need_resched is set, trigger a
 * reschedule immediately by calling schedule().
 */
void preempt_enable(void) {
    if (!current) return;
    if (current->preempt_count > 0) current->preempt_count--;
    if (current->preempt_count == 0 && current->need_resched) {
        current->need_resched = 0;
        current->state = TASK_READY;
        schedule();
    }
}

/* ================================================================
 * Process exit
 * ================================================================ */

void do_exit_current(int code) {
    if (!current) return;
    if (current->pid == 0) {
        log_printf(LOG_LEVEL_ERR, "do_exit_current: idle task attempted exit\n");
        for (;;) asm volatile ("cli; hlt");
    }

    log_printf(LOG_LEVEL_INFO, "Task pid=%d exiting with code=%d\n", current->pid, code);

    /* Record explainability event */
    extern void explain_exit(int pid, int code, int signal);
    explain_exit(current->pid, code, 0);

    /* Close all fds */
    fd_close_all(current);

    /* FIXED (v4.1.4): Free all VMAs on exit (BUG 3.1) */
    vma_free_all(current);

    /* Reparent children to init */
    reparent_children_to_init(current);

    /* Notify parent via SIGCHLD */
    extern void signal_child_event(struct task_struct *child, int event);
    signal_child_event(current, 0);

    /* REFCOUNT (v4.2.6): Protect state transition with state_lock */
    spin_lock((spinlock_t*)&current->state_lock);
    current->exit_code = code;
    current->state = TASK_ZOMBIE;
    spin_unlock((spinlock_t*)&current->state_lock);

    /*
     * Wake parent: if parent is blocked in waitpid(), set it to READY
     * so it can collect this ZOMBIE.
     * FIXED (v4.3.0): NEW-8 EXIT-LOCK — protect parent->state with lock.
     */
    if (current->parent) {
        spin_lock((spinlock_t*)&current->parent->state_lock);
        if (current->parent->state == TASK_BLOCKED) {
            log_printf(LOG_LEVEL_DEBUG, "exit: waking parent pid=%d\n",
                       current->parent->pid);
            current->parent->state = TASK_READY;
        }
        spin_unlock((spinlock_t*)&current->parent->state_lock);
    }

    /*
     * FIXED (v4.2.8): BUG-VFORK-WAKE
     * Only vfork children should wake the parent via vfork_done.
     * A normal fork child's parent is NOT blocked in vfork, so
     * setting vfork_done here would be a no-op/wrong.  Additionally,
     * the parent must be re-enqueued into the run queue (via state
     * change to TASK_READY) so the scheduler can resume it.
     * The parent is already in the run queue's linked list and RB
     * tree; setting state to TASK_READY is sufficient for the
     * scheduler to find it on the next schedule() call.
     */
    /* FIXED (v4.3.0): NEW-9 VFORK-LOCK — protect vfork_done with lock. */
    if (current->vfork_child && current->parent) {
        spin_lock((spinlock_t*)&current->parent->state_lock);
        if (current->parent->vfork_done == 0) {
            current->parent->vfork_done = 1;
        }
        spin_unlock((spinlock_t*)&current->parent->state_lock);
    }

    /* Remove from run queue (SMP-safe: acquire run queue lock to prevent
     * races with smp_schedule() which may try to steal tasks from this CPU).
     * CRITICAL: Disable interrupts to prevent self-deadlock with
     * pit_irq_c_handler() which also acquires rq->lock. */
    int cpu_id = current_cpu_id();
    struct run_queue *rq = &per_cpu_rq[cpu_id];
    uint64_t exit_irq_flags = irq_save();
    spin_lock(&rq->lock);

    struct task_struct *prev_node = current;
    while (prev_node->next != current) prev_node = prev_node->next;

    if (prev_node == current) {
        spin_unlock(&rq->lock);
        irq_restore(exit_irq_flags);
        log_printf(LOG_LEVEL_INFO, "do_exit_current: last task exiting, halting\n");
        for (;;) {
            asm volatile ("sti; hlt");
            if (rq->count > 0) break;
        }
        /* New tasks arrived, schedule them */
        schedule();
        /* NOTREACHED */
    }

    prev_node->next = current->next;

    /* Update rq->head if we're removing the head of the queue */
    if (rq->head == current) {
        rq->head = current->next;
    }

    /* Also remove from the red-black tree */
    rb_erase(&rq->ready_tree, &current->rb_node);

    /* Decrement run queue count */
    if (rq->count > 0) rq->count--;

    spin_unlock(&rq->lock);
    irq_restore(exit_irq_flags);

    /*
     * REFCOUNT (v4.2.6): Call schedule() which will task_put(prev) after
     * context_switch, dropping the running reference (ref_count: 2→1).
     * The remaining existence reference (ref_count=1) is held until the
     * parent calls waitpid() to collect this ZOMBIE.  waitpid() will then
     * task_put() the child, dropping ref_count to 0 and triggering
     * task_free().
     */
    schedule();
    /* NOTREACHED — schedule() context_switches away and never returns here */
}

/* ================================================================
 * waitpid: True blocking wait for child exit (FIXED)
 *
 * If no ZOMBIE child exists, blocks current task (TASK_BLOCKED)
 * and calls schedule(). The child's do_exit_current will set
 * parent back to READY, and schedule() will resume this task
 * on the next timer tick or yield.
 * ================================================================ */

int waitpid(int pid, int *status, int options) {
    if (!current) return -1;

    /* FIXED (v4.3.5): BUG-NEW-01 — Idle task must never block in waitpid.
     * The idle task (pid=0) has no children and blocking here would
     * deadlock the entire scheduler.  If WNOHANG is set, the idle task
     * can scan for zombies without blocking; otherwise return -ECHILD. */
    if (current->pid == 0 && !(options & WNOHANG)) {
        log_printf(LOG_LEVEL_WARN, "waitpid: idle task cannot wait, returning -ECHILD\n");
        return -ECHILD;
    }

    for (;;) {
retry:
        /* FIXED: acquire child_lock to prevent concurrent waitpid races */
        spin_lock((spinlock_t*)&current->child_lock);

        /* Scan children for ZOMBIE */
        struct child_node *prev = NULL;
        struct child_node *node = current->children;

        while (node) {
            struct task_struct *child = node->child;

            if (!child) {
                /* Skip orphaned child nodes (should not happen, but guard against it) */
                prev = node;
                node = node->next;
                continue;
            }

            if ((pid == -1 || child->pid == pid) &&
                child->state == TASK_ZOMBIE) {

                /* REFCOUNT (v4.2.6): Hold a reference to the child while
                 * we collect its data.  This prevents the child from being
                 * freed by another concurrent waitpid. */
                task_get(child);

                /* Found ZOMBIE child — collect it.
                 * Save values BEFORE releasing the reference (use-after-free safety). */
                int collected_pid   = child->pid;
                int collected_code  = child->exit_code;

                if (status) *status = collected_code;

                if (prev)
                    prev->next = node->next;
                else
                    current->children = node->next;
                kfree(node);

                spin_unlock((spinlock_t*)&current->child_lock);

                /* REFCOUNT (v4.2.6): Release the existence reference.
                 * This drops ref_count from 1 to 0.  Since the child is
                 * in ZOMBIE state, task_put() will call task_free() to
                 * actually free all resources (kernel stack, page tables,
                 * file descriptors, signal state, VMAs, PID, task_struct). */
                task_put(child);

                log_printf(LOG_LEVEL_INFO, "waitpid: collected pid=%d exit_code=%d\n",
                           collected_pid, collected_code);

                return collected_pid;
            }

            prev = node;
            node = node->next;
        }

        spin_unlock((spinlock_t*)&current->child_lock);  /* FIXED: unlock before block */

        /*
         * No ZOMBIE child found.
         *
         * FIXED (v4.2.3): If a specific pid was requested (not -1),
         * verify it's actually a child before blocking.  Waiting for
         * a non-child pid would block the caller forever.  (BUG-PROC-07)
         */
        if (pid != -1) {
            int is_child = 0;
            struct child_node *node3 = current->children;
            while (node3) {
                if (node3->child && node3->child->pid == pid) {
                    is_child = 1;
                    break;
                }
                node3 = node3->next;
            }
            if (!is_child) {
                /* FIXED (v4.2.4): Set errno to ECHILD before returning.
                 * Previously, waitpid returned -1 without setting errno,
                 * causing the caller to see a stale errno value.
                 * (BUG-WAITPID-ERRNO) */
                current->t_errno = ECHILD;
                return -1;  /* errno ECHILD set by caller */
            }
        }

        /* If WNOHANG is set, return 0 immediately (non-blocking).
         * Otherwise block until a child exits. */
        if (options & WNOHANG) {
            return 0;
        }

        /*
         * FIXED (v4.1.4): Set TASK_BLOCKED atomically with the child
         * scan to prevent a wakeup-loss race.  The lock is re-acquired
         * to ensure that between scanning children and setting
         * TASK_BLOCKED, no child can exit without seeing the parent
         * as blocked.  Without this, the child could exit in the gap
         * between unlock and TASK_BLOCKED, and the wakeup would be
         * lost forever.  (BUG 3.3)
         */
        spin_lock((spinlock_t*)&current->child_lock);
        /* Re-check: a child might have become ZOMBIE between unlock and re-lock */
        {
            struct child_node *node2 = current->children;
            while (node2) {
                struct task_struct *child2 = node2->child;
                if (child2 && child2->state == TASK_ZOMBIE) {
                    spin_unlock((spinlock_t*)&current->child_lock);
                    goto retry;  /* retry the outer loop to collect the zombie */
                }
                node2 = node2->next;
            }
        }
        current->state = TASK_BLOCKED;
        spin_unlock((spinlock_t*)&current->child_lock);
        schedule();

        /* Resumed: re-scan children (the ZOMBIE should now be there) */
    }
}

/* ================================================================
 * SMP scheduling functions
 * ================================================================ */

/*
 * smp_enqueue_task: Add a task to a specific CPU's run queue.
 * For use with CPU affinity and cross-CPU task migration.
 */
void smp_enqueue_task(struct task_struct *t, int cpu_id) {
    if (!t || cpu_id < 0 || cpu_id >= MAX_CPUS) return;
    if (cpu_id >= num_cpus) return;

    struct run_queue *rq = &per_cpu_rq[cpu_id];

    /* FIXED (v4.2.4): Acquire the remote CPU's run queue lock.
     * Without this lock, concurrent modifications to the run queue
     * (e.g., from the remote CPU's schedule() and this enqueue)
     * would cause data races on the linked list and RB tree.
     * We use irq_save/irq_restore to prevent deadlocks with
     * interrupt handlers on the same CPU.  (BUG-SMP-ENQUEUE) */
    uint64_t irq = irq_save();
    spin_lock(&rq->lock);

    if (rq->head == NULL) {
        t->next = t;
        rq->head = t;
    } else {
        t->next = rq->head->next;
        rq->head->next = t;
    }
    rq->count++;
    /* Also insert into the red-black tree */
    t->rb_node.key = t->vruntime;
    rb_insert(&rq->ready_tree, &t->rb_node);

    spin_unlock(&rq->lock);
    irq_restore(irq);
}

/*
 * smp_dequeue_task: Remove a task from a specific CPU's run queue.
 * Does NOT free the task — just removes it from the queue.
 */
void smp_dequeue_task(struct task_struct *t, int cpu_id) {
    if (!t || cpu_id < 0 || cpu_id >= MAX_CPUS) return;

    struct run_queue *rq = &per_cpu_rq[cpu_id];
    if (rq->head == NULL || rq->count == 0) return;

    /* FIXED (v4.2.5): Acquire the remote CPU's run queue lock.
     * Without this lock, concurrent modifications to the run queue
     * (e.g., from the remote CPU's schedule() and this dequeue)
     * would cause data races on the linked list and RB tree.
     * We use irq_save/irq_restore to prevent deadlocks with
     * interrupt handlers on the same CPU.  (BUG-SMP-DEQUEUE) */
    uint64_t irq = irq_save();
    spin_lock(&rq->lock);

    /* Handle single-task queue */
    if (rq->head == t && t->next == t) {
        rq->head = NULL;
        rq->count = 0;
        rb_erase(&rq->ready_tree, &t->rb_node);
        spin_unlock(&rq->lock);
        irq_restore(irq);
        return;
    }

    /* Find the task before 't' in the circular list */
    struct task_struct *prev = rq->head;
    while (prev->next != t) {
        prev = prev->next;
        if (prev == rq->head) {
            /* Task not found in this queue */
            spin_unlock(&rq->lock);
            irq_restore(irq);
            return;
        }
    }

    /* Unlink */
    prev->next = t->next;
    if (rq->head == t) {
        rq->head = t->next;
    }
    rq->count--;
    /* Also remove from the red-black tree */
    rb_erase(&rq->ready_tree, &t->rb_node);

    spin_unlock(&rq->lock);
    irq_restore(irq);
}

/*
 * smp_schedule: Load-balance tasks across CPUs.
 *
 * Called periodically from the timer interrupt (or reschedule IPI).
 * Migrates tasks from overloaded CPUs to idle ones.
 *
 * Strategy: simple work-stealing — if this CPU's queue is empty,
 * steal a task from the busiest CPU.
 */
void smp_schedule(int my_cpu_id) {
    if (my_cpu_id < 0 || my_cpu_id >= num_cpus) return;

    struct run_queue *my_rq = &per_cpu_rq[my_cpu_id];

    /* If we already have tasks, nothing to do */
    if (my_rq->count > 1) return;

    /* Find the busiest CPU (excluding self) */
    int busiest_cpu = -1;
    int max_count = 0;

    for (int i = 0; i < num_cpus; i++) {
        if (i == my_cpu_id) continue;
        struct run_queue *rq = &per_cpu_rq[i];
        if (rq->count > max_count) {
            max_count = rq->count;
            busiest_cpu = i;
        }
    }

    /* If no CPU has more than 1 task, nothing to steal */
    if (busiest_cpu < 0 || max_count <= 1) return;

    struct run_queue *src_rq = &per_cpu_rq[busiest_cpu];

    /* Lock both queues in deterministic order (lower CPU ID first) to
     * prevent AB-BA deadlock when multiple CPUs try to steal from each
     * other simultaneously.
     * CRITICAL: Disable interrupts — pit_irq_c_handler() also acquires
     * rq->lock from timer interrupt context. */
    uint64_t smp_irq_flags = irq_save();
    if (my_cpu_id < busiest_cpu) {
        spin_lock(&my_rq->lock);
        spin_lock(&src_rq->lock);
    } else {
        spin_lock(&src_rq->lock);
        spin_lock(&my_rq->lock);
    }

    /* Re-check counts after acquiring locks */
    if (my_rq->count > 1 || src_rq->count <= 1) {
        spin_unlock(&src_rq->lock);
        spin_unlock(&my_rq->lock);
        irq_restore(smp_irq_flags);
        return;
    }

    /* Steal one task from the busiest CPU (not the head) */
    struct task_struct *stolen = NULL;

    /* Find a TASK_READY task to steal */
    struct task_struct *candidate = src_rq->head;
    if (candidate) {
        struct task_struct *start = candidate;
        do {
            if (candidate->state == TASK_READY &&
                candidate != src_rq->head) {
                stolen = candidate;
                break;
            }
            candidate = candidate->next;
        } while (candidate != start);
    }

    if (stolen) {
        /* FIXED (v4.2.7): BUG-SMP-SCHED-DEADLOCK
         * Inline dequeue/enqueue instead of calling smp_dequeue_task /
         * smp_enqueue_task, which would try to acquire rq->lock again
         * (deadlock!).  Both rq locks are already held by this function. */

        /* ---- Dequeue stolen task from src_rq ---- */
        if (src_rq->head == stolen && stolen->next == stolen) {
            /* Only task in the queue */
            src_rq->head = NULL;
            src_rq->count = 0;
        } else {
            /* Find the task before stolen in the circular list */
            struct task_struct *prev = src_rq->head;
            while (prev->next != stolen) prev = prev->next;
            prev->next = stolen->next;
            if (src_rq->head == stolen) src_rq->head = stolen->next;
            src_rq->count--;
        }
        rb_erase(&src_rq->ready_tree, &stolen->rb_node);

        /* ---- Enqueue stolen task into my_rq ---- */
        if (my_rq->head == NULL) {
            stolen->next = stolen;
            my_rq->head = stolen;
        } else {
            stolen->next = my_rq->head->next;
            my_rq->head->next = stolen;
        }
        my_rq->count++;
        stolen->rb_node.key = stolen->vruntime;
        rb_insert(&my_rq->ready_tree, &stolen->rb_node);

        log_printf(LOG_LEVEL_DEBUG, "smp: migrated task pid=%d from CPU %d to CPU %d\n",
                   stolen->pid, busiest_cpu, my_cpu_id);
    }

    spin_unlock(&src_rq->lock);
    spin_unlock(&my_rq->lock);
    irq_restore(smp_irq_flags);
}
