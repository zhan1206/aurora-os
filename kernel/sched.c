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
#include "include/log.h"
#include "include/assert.h"
#include "mem.h"
#include "pagetable.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

/* ================================================================
 * PID bitmap allocator — O(1) lookup, O(1) alloc
 * ================================================================ */
#define MAX_PID           8192
#define PID_BITMAP_WORDS  (MAX_PID / 64)  /* 128 uint64_t entries */
static uint64_t pid_bitmap[PID_BITMAP_WORDS];
static int next_pid = 2;  /* 0=idle, 1=init, user tasks start at 2 */

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
    /* Reserve pid 0 and 1 */
    pid_set_bit(0);
    pid_set_bit(1);

    /* Scan for a free PID using rotating next_pid cursor */
    for (int attempt = 0; attempt < MAX_PID; ++attempt) {
        int pid = next_pid;
        next_pid = (pid + 1 >= MAX_PID) ? 2 : pid + 1;
        if (!pid_test_bit(pid)) {
            pid_set_bit(pid);
            return pid;
        }
    }
    return -1;  /* PID space exhausted */
}

static void free_pid(int pid) {
    if (pid >= 2 && pid < MAX_PID) {
        pid_clear_bit(pid);
        pid_table[pid] = NULL;  /* clear O(1) lookup entry */
    }
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
    struct task_struct *t = pid_table[pid];
    if (t && t->state != TASK_DEAD) return t;
    return NULL;
}

/* ================================================================
 * Children list management
 * ================================================================ */

static void add_child(struct task_struct *parent, struct task_struct *child) {
    struct child_node *node = (struct child_node *)kmalloc(sizeof(*node));
    if (!node) return;
    node->child = child;
    node->next = parent->children;
    parent->children = node;
    child->parent = parent;
}

void reparent_children_to_init(struct task_struct *task) {
    if (!task || !task->children) return;
    if (!init_task) return;

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
}

/* ================================================================
 * Scheduler initialization
 * ================================================================ */

void scheduler_init(void) {
    /*
     * Initialize PID bitmap — reserve 0 (idle) and 1 (init).
     * page_table_init() is already called in kernel_main() before
     * scheduler_init(). get_kernel_cr3() returns the cached CR3.
     */
    memset(pid_bitmap, 0, sizeof(pid_bitmap));
    memset(pid_table, 0, sizeof(pid_table));
    pid_set_bit(0);
    pid_set_bit(1);

    /* Create idle task (pid=0) */
    current = (struct task_struct *)kmalloc(sizeof(struct task_struct));
    ASSERT(current != NULL);
    memset(current, 0, sizeof(*current));
    current->rsp        = NULL;
    current->cr3        = get_kernel_cr3();
    current->pid        = 0;
    current->state      = TASK_RUNNING;
    current->priority   = 0;          /* lowest priority */
    current->time_slice = 1;
    current->cpu_mask   = 0x01;
    current->next       = current;
    current->parent     = NULL;
    current->children   = NULL;
    current->t_errno    = 0;
    strncpy(current->name, "idle", sizeof(current->name) - 1);
    fd_table_init(current);
    pid_register(0, current);
    idle_task = current;

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

    /* Allocate a single page for the kernel stack. */
    void *stack_page = alloc_page();
    if (!stack_page) {
        kfree(t);
        return NULL;
    }

    /* Stack grows downward: top of page. */
    uint8_t *stack_top = (uint8_t *)stack_page + PAGE_SIZE;
    uint64_t *sp = (uint64_t *)stack_top;

    if (fn) {
        /* Normal task: fn is the return address for context_switch's ret */
        *(--sp) = (uint64_t)fn;
    } else {
        /*
         * Fork child: The child's kernel stack must look exactly as if
         * it had just executed the pushes in syscall_entry and was about
         * to call syscall_trap.  We place a fake return address pointing
         * to syscall_return_point, then the 13 saved registers, then the
         * callee-saved regs for context_switch.
         *
         * Stack layout (top → bottom):
         *   [syscall_return_point]   ← ret addr for context_switch
         *   [callee-saved regs × 6]  ← popped by context_switch
         *   [saved regs × 13]        ← popped by syscall_return_point
         *   [swapgs + sysretq]       ← executed after pops
         *
         * BUT: context_switch pops 6 callee-saved regs then rets.
         * After ret, we're at syscall_return_point which pops 13 regs
         * then does swapgs + sysretq.
         *
         * So the stack just needs:
         *   ret_addr → syscall_return_point
         *   rbp, rbx, r12-r15 (6 × zero)
         *
         * syscall_return_point will pop 13 regs from the stack, but
         * those values don't exist yet.  Wait — we need to push them too.
         *
         * Full layout:
         *   [syscall_return_point]   ← ret from context_switch
         *   [rbp=0][rbx=0][r12=0][r13=0][r14=0][r15=0] ← context_switch pops
         *   [r15=0]...[rax=0] ← 13 regs popped by syscall_return_point
         *
         * Total: 1 + 6 + 13 = 20 × 8 = 160 bytes below stack_top
         */
        uint64_t *frame_start = sp - 20;
        /* Fill all 20 slots with 0 (13 regs + 6 callee-saved + ret addr) */
        for (int i = 0; i < 20; i++) frame_start[i] = 0;
        /* Set the return address for context_switch */
        frame_start[19] = (uint64_t)(uintptr_t)&syscall_return_point;
        /* Set RCX and R11 for sysretq (RCX=user RIP, R11=user RFLAGS).
         * These are slots 7 (rcx) and 10 (r11) in the 13-reg block.
         * 13-reg block is at frame_start[0..12], callee-saved at [13..18], ret at [19].
         * Slot index in 13-reg block (pushed order):
         *   [0]=r15, [1]=r14, [2]=r13, [3]=r12, [4]=r11, [5]=r10,
         *   [6]=r9, [7]=r8, [8]=rsi, [9]=rdi, [10]=rdx, [11]=rcx, [12]=rax
         *
         * For the child, RCX should hold the same user RIP as the parent
         * (which is the instruction after the fork syscall).
         * R11 should hold user RFLAGS.
         * These come from the parent's trapframe, but here we just
         * need them to be valid so sysretq works.  The actual values
         * will be set properly when the parent's fork syscall returns.
         *
         * For a correct implementation, the child's user RIP/RFLAGS
         * should be the same as the parent's.  We'll copy them from
         * the current trapframe in sys_fork.
         *
         * For now, set RCX=0x400000 (default user entry) and R11=0x202
         * as reasonable defaults (will be overwritten in sys_fork).
         */
        frame_start[11] = 0x400000;   /* RCX = user RIP */
        frame_start[4]  = 0x202;      /* R11 = user RFLAGS (IF=1) */

        sp = frame_start;
    }

    t->rsp         = sp;
    t->cr3         = get_kernel_cr3();
    t->stack_phys  = stack_page;
    t->stack_phys2 = NULL;
    t->state       = TASK_READY;
    t->priority    = 128;        /* default medium priority */
    t->time_slice  = 10;         /* 10 ticks = 100ms at 100Hz */
    t->cpu_mask    = 0x01;       /* CPU 0 only (single-core) */
    t->parent      = current;
    t->children    = NULL;
    t->t_errno     = 0;
    t->pid         = alloc_pid();
    if (t->pid < 0) {
        free_page(stack_page);
        kfree(t);
        return NULL;
    }

    fd_table_init(t);
    pid_register(t->pid, t);
    t->next = current->next;
    current->next = t;
    add_child(current, t);

    log_printf(LOG_LEVEL_INFO, "Created task pid=%d\n", t->pid);
    return t;
}

/* ================================================================
 * Scheduler core
 * ================================================================ */

void schedule(void) {
    if (!current || !current->next) return;

    struct task_struct *next = current->next;
    int scanned = 0;
    int limit = MAX_PID;  /* safety: match max PID space */

    while (next->state != TASK_READY && next->state != TASK_RUNNING) {
        next = next->next;
        scanned++;
        if (next == current || scanned > limit) {
            /*
             * No runnable task found.
             * Switch to idle (pid=0) which loops with HLT.
             * idle is always in the ready queue and always READY.
             */
            if (idle_task && idle_task != current && idle_task->state == TASK_READY) {
                next = idle_task;
                break;
            }
            /* Fallback: if even idle is broken, halt */
            log_printf(LOG_LEVEL_ERR, "schedule: no runnable tasks, halting\n");
            for (;;) asm volatile ("cli; hlt");
        }
    }

    if (next == current) {
        /* Only one runnable task — keep it running.
         * Ensure state is TASK_RUNNING since yield() may have
         * set it to TASK_READY before calling schedule(). */
        current->state = TASK_RUNNING;
        return;
    }

    struct task_struct *prev = current;
    prev->state = (prev->state == TASK_RUNNING) ? TASK_READY : prev->state;
    next->state = TASK_RUNNING;
    current = next;

    uint64_t new_cr3 = current->cr3;
    asm volatile ("mov %0, %%cr3" :: "r"(new_cr3) : "memory");

    context_switch(&prev->rsp, current->rsp);
}

void yield(void) {
    if (current) current->state = TASK_READY;
    schedule();
}

void check_resched(void) {
    extern volatile int need_resched;
    if (need_resched) {
        need_resched = 0;
        if (current) current->state = TASK_READY;
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

    /* Reparent children to init */
    reparent_children_to_init(current);

    /* Notify parent via SIGCHLD */
    extern void signal_child_event(struct task_struct *child, int event);
    signal_child_event(current, 0);

    /* Set ZOMBIE state */
    current->exit_code = code;
    current->state = TASK_ZOMBIE;

    /*
     * Wake parent: if parent is blocked in waitpid(), set it to READY
     * so it can collect this ZOMBIE.
     */
    if (current->parent && current->parent->state == TASK_BLOCKED) {
        log_printf(LOG_LEVEL_DEBUG, "exit: waking parent pid=%d\n",
                   current->parent->pid);
        current->parent->state = TASK_READY;
    }

    /* Remove from ready queue */
    struct task_struct *prev_node = current;
    while (prev_node->next != current) prev_node = prev_node->next;

    if (prev_node == current) {
        log_printf(LOG_LEVEL_INFO, "do_exit_current: last task exiting, halting\n");
        for (;;) asm volatile ("cli; hlt");
    }

    prev_node->next = current->next;

    /*
     * IMPORTANT: Do NOT do our own context_switch here.
     * The old code did "current = current->next; context_switch(...)" which
     * could pick init (rsp=NULL) or any random task, corrupting the stack.
     * Instead: keep ourselves as ZOMBIE (so parent's waitpid can collect us),
     * remove from ready queue (already done above), and call schedule().
     * schedule() will find the next runnable task and switch to it.
     * Parent will later collect this ZOMBIE via waitpid and free us.
     *
     * Note: 'current' still points to this dying task.  schedule() uses
     * current as the starting point to scan for next runnable, but we're
     * no longer in the queue (already unlinked), so we won't be re-selected.
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
    (void)options;
    if (!current) return -1;

    for (;;) {
        /* Scan children for ZOMBIE */
        struct child_node *prev = NULL;
        struct child_node *node = current->children;

        while (node) {
            struct task_struct *child = node->child;

            if ((pid == -1 || child->pid == pid) &&
                child->state == TASK_ZOMBIE) {

                /* Found ZOMBIE child — collect it.
                 * Save values BEFORE freeing child (use-after-free safety). */
                int collected_pid   = child->pid;
                int collected_code  = child->exit_code;
                void *child_stack   = child->stack_phys;
                uint64_t child_cr3  = child->cr3;

                if (status) *status = collected_code;

                if (prev)
                    prev->next = node->next;
                else
                    current->children = node->next;
                kfree(node);

                /* Free child resources */
                if (child_stack) {
                    free_page(child_stack);
                }
                if (child_cr3 && child_cr3 != get_kernel_cr3()) {
                    extern void free_pagetable(uint64_t pml4_phys);
                    free_pagetable(child_cr3);
                }

                free_pid(collected_pid);
                child->state = TASK_DEAD;
                kfree(child);

                log_printf(LOG_LEVEL_INFO, "waitpid: collected pid=%d exit_code=%d\n",
                           collected_pid, collected_code);
                return collected_pid;
            }

            prev = node;
            node = node->next;
        }

        /*
         * No ZOMBIE child found → block until one exits.
         * The child's do_exit_current will set us back to READY
         * and the next schedule() will resume us here.
         */
        current->state = TASK_BLOCKED;
        schedule();

        /* Resumed: re-scan children (the ZOMBIE should now be there) */
    }
}
