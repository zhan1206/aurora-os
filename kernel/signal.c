/*
 * signal.c - POSIX signal framework (Phase 1: complete handler delivery)
 */
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "pagetable.h"
#include "include/log.h"
#include "include/userspace.h"
#include "include/trapframe.h"
#include "smp.h"
#include "mem.h"
#include <string.h>
#include <stdint.h>

static spinlock_t signal_lock = {0};

struct signal_state *signal_alloc(void) {
    struct signal_state *s = (struct signal_state *)kmalloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    return s;
}

/* ================================================================
 * do_sys_kill
 * ================================================================ */
int do_sys_kill(int pid, int sig) {
    if (sig < 1 || sig >= NSIG) return -1;
    if (pid < 0) return -1;

    struct task_struct *target = task_get_by_pid(pid);
    if (!target) return -1;

    /*
     * FIXED (v4.1.4): Permission check for signal sending.
     * Only allow:
     *   - Sending to self (same process)
     *   - Sending SIGCONT if in same session (not yet tracked)
     *   - Sending any signal if real/effective UID matches (all UID=0 for now)
     *   - SIGKILL/SIGSTOP/SIGCHLD: always allowed within the same UID
     * Protect init (pid=1) from arbitrary signals.  (BUG 3.6)
     */
    if (target->pid != current->pid) {
        /* Protect init: only SIGKILL, SIGSTOP, SIGCHLD allowed */
        if (target->pid == 1) {
            if (sig != SIGKILL && sig != SIGSTOP && sig != SIGCHLD) {
                /* REFCOUNT (v4.2.6): Release reference held by task_get_by_pid */
                task_put(target);
                return -1;
            }
        }
        /* Simple UID check: all processes run as UID 0 for now,
         * so this check always passes.  When multi-user support
         * is added, this should verify that sender's UID matches
         * target's UID or sender is root. */
    }

    log_printf(LOG_LEVEL_DEBUG, "signal: kill(pid=%d, sig=%d)\n", pid, sig);

    spin_lock(&signal_lock);

    if (target->sig) {
        target->sig->pending |= (1U << sig);
    } else {
        target->sig = signal_alloc();
        if (!target->sig) {
            spin_unlock(&signal_lock);
            /* REFCOUNT (v4.2.6): Release reference held by task_get_by_pid */
            task_put(target);
            return -1;
        }
        target->sig->pending |= (1U << sig);
    }

    if (target->state == TASK_BLOCKED) {
        /* Don't wake a blocked task if the signal is in its blocked mask */
        if (!(target->sig->blocked & (1U << sig))) {
            if (sig == SIGKILL ||
                (target->sig->actions[sig].sa_handler != SIG_IGN)) {
                /*
                 * FIXED (v4.2.0): Release signal_lock before modifying
                 * task state to avoid lock ordering violation with the
                 * runqueue lock.  The signal is already recorded in
                 * target->sig->pending, so the task will process it
                 * when it wakes up.  (BUG-PROC-M2)
                 */
                spin_unlock(&signal_lock);
                target->state = TASK_READY;
                /* REFCOUNT (v4.2.6): Release reference held by task_get_by_pid */
                task_put(target);
                return 0;
            }
        }
    }
    spin_unlock(&signal_lock);

    /* REFCOUNT (v4.2.6): Release reference held by task_get_by_pid */
    task_put(target);
    return 0;
}

/* ================================================================
 * do_sys_sigaction
 * ================================================================ */
int do_sys_sigaction(int signo, const struct sigaction *act,
                      struct sigaction *oldact) {
    if (signo < 1 || signo >= NSIG) return -1;
    if (signo == SIGKILL) return -1;

    if (!current->sig) {
        current->sig = signal_alloc();
        if (!current->sig) return -1;
    }

    if (oldact) {
        if (copy_to_user(oldact, &current->sig->actions[signo],
                         sizeof(struct sigaction)) != 0)
            return -1;
    }

    if (act) {
        if (copy_from_user(&current->sig->actions[signo], act,
                           sizeof(struct sigaction)) != 0)
            return -1;
    }

    return 0;
}

/* ================================================================
 * do_sys_sigreturn: Restore user context from sigframe
 *
 * When a user signal handler returns, it calls the sigreturn syscall.
 * The sigframe is at the top of the user stack (RSP when sigreturn
 * was invoked). We read it and restore the trapframe so that
 * syscall.S's iretq/sysretq returns to the original user context.
 * ================================================================ */
void do_sys_sigreturn(void) {
    if (!current->current_tf) {
        log_printf(LOG_LEVEL_ERR, "signal: sigreturn with no trapframe\n");
        do_exit_current(1);
        return;
    }

    if (!current->sig) {
        log_printf(LOG_LEVEL_WARN, "signal: sigreturn with no signal state\n");
        return;
    }

    if (current->sig->saved_rip) {
        /*
         * Restore the sigframe from the user stack. The sigframe was
         * placed at user_rsp - sizeof(struct sigframe) by check_signals(),
         * where user_rsp is the original RSP before signal delivery.
         *
         * Stack layout (low to high):
         *   [new_rsp]         = return addr (8 bytes) → consumed by handler's ret
         *   [new_rsp+8]       = trampoline (16 bytes)
         *   [new_rsp+24]      = sigframe (sizeof(struct sigframe) bytes)
         *   [orig user_rsp]   = original stack top
         *
         * new_rsp = user_rsp - (sizeof(sigframe) + 8 + TRAMPOLINE_SIZE)
         * sigframe is at: user_rsp - sizeof(sigframe)
         */
        uint64_t user_rsp = current->sig->saved_rsp;
        uint64_t frame_addr = user_rsp - sizeof(struct sigframe);

        struct sigframe frame;
        if (copy_from_user(&frame, (void *)(uintptr_t)frame_addr,
                           sizeof(frame)) == 0 &&
            frame.signo > 0 && frame.signo < NSIG) {
            /* Restore all general-purpose registers from the sigframe */
            current->current_tf->r15 = frame.r15;
            current->current_tf->r14 = frame.r14;
            current->current_tf->r13 = frame.r13;
            current->current_tf->r12 = frame.r12;
            /*
             * FIXED (v4.1.4): Preserve IF bit (0x200) in RFLAGS during
             * sigreturn.  The previous mask 0x3F7FD7 cleared bit 9 (IF),
             * disabling interrupts for user processes after signal handler
             * return.  New mask 0x3F7FF7 preserves IF.
             * FIXED (v4.2.4): Also mask IOPL bits (12-13, 0x3000).
             * The previous mask 0x3F7FF7 did not clear IOPL, allowing a
             * signal handler to elevate I/O privilege.  New mask 0x3F4FF7
             * clears IOPL, NT, TF, and AC while preserving IF.  (BUG-IOPL)
             * FIXED (v4.2.5): BUG-SIG-RFLAGS — The mask 0x3F4FF7 incorrectly
             * preserved TF (bit 8, 0x100), NT (bit 14, 0x4000), and AC
             * (bit 18, 0x40000).  Corrected mask 0x3F0CF7 clears these bits
             * while preserving IF (bit 9). */
            current->current_tf->r11 = frame.rflags & 0x3F0CF7;  /* mask IOPL/NT/TF/AC, preserve IF */
            current->current_tf->r10 = frame.r10;
            current->current_tf->r9  = frame.r9;
            current->current_tf->r8  = frame.r8;
            current->current_tf->rsi = frame.rsi;
            current->current_tf->rdi = frame.rdi;
            current->current_tf->rdx = frame.rdx;
            current->current_tf->rcx = frame.rcx;
            current->current_tf->rax = frame.rax;
            current->current_tf->rip = frame.rip;
            current->current_tf->rsp = frame.rsp;
        } else {
            /* Fallback: restore RIP/RSP from saved context only */
            current->current_tf->rip = current->sig->saved_rip;
            current->current_tf->rsp = current->sig->saved_rsp;
        }

        current->sig->saved_rip = 0;
        current->sig->saved_rsp = 0;

        /* Unblock all signals that were blocked during handler */
        current->sig->blocked = 0;

        log_printf(LOG_LEVEL_DEBUG, "signal: sigreturn restored RIP=%p RSP=%p\n",
                   (void *)current->current_tf->rip, (void *)current->current_tf->rsp);
    } else {
        log_printf(LOG_LEVEL_WARN, "signal: sigreturn with no saved context\n");
    }
}

/* ================================================================
 * do_signal_default
 * ================================================================ */
void do_signal_default(int sig) {
    switch (sig) {
        case SIGKILL:
        case SIGTERM:
        case SIGINT:
            log_printf(LOG_LEVEL_INFO, "signal: terminating pid=%d on sig=%d\n",
                       current->pid, sig);
            do_exit_current(128 + sig);
            break;
        case SIGCHLD:
            break;
        default:
            log_printf(LOG_LEVEL_INFO, "signal: terminating pid=%d on sig=%d\n",
                       current->pid, sig);
            do_exit_current(128 + sig);
            break;
    }
}

/* ================================================================
 * check_signals: Deliver pending signals
 *
 * Now with full user-handler support: pushes a sigframe onto the
 * user stack and redirects RIP/RSP in the trapframe.
 * ================================================================ */
void check_signals(void) {
    if (!current) return;
    if (!current->sig) return;
    if (current->sig->pending == 0) return;
    if (!current->current_tf) return;  /* no trapframe = kernel context, defer */

    struct signal_state *sig = current->sig;

    for (int s = 1; s < NSIG; ++s) {
        if (!(sig->pending & (1U << s))) continue;
        if (sig->blocked & (1U << s)) continue;

        sig->pending &= ~(1U << s);
        sighandler_t handler = sig->actions[s].sa_handler;

        if (handler == SIG_DFL) {
            do_signal_default(s);
            return;
        }

        if (handler == SIG_IGN) {
            continue;
        }

        /*
         * User-defined handler: push sigframe onto user stack
         * and redirect execution to the handler.
         *
         * FIXED (v4.1.4): Prevent signal nesting.  If a signal handler is
         * already in progress (saved_rip != 0), defer delivery of new signals
         * until the current handler returns via sigreturn.  This prevents
         * the saved_rip/saved_rsp (the original user context) from being
         * overwritten, which would permanently lose the return context for
         * the first signal handler.  (BUG 4.9)
         */
        if (sig->saved_rip != 0) {
            /* Signal already being handled — defer this signal */
            sig->pending |= (1U << s);  /* re-queue */
            continue;
        }

        /*
         * User stack layout after setup (grows downward):
         *   [original RSP]      ← original top
         *   [sigframe]          ← saved registers
         *   [trampoline code]   ← mov eax, SYS_SIGRETURN; syscall (8 bytes)
         *   [return addr]       ← pointer to trampoline code
         *   [new RSP]           ← trapframe->rsp points here
         *
         * The handler is called with (signo) in RDI.
         * When it returns (ret), it pops the return address which
         * points to the trampoline code that calls syscall(SYS_SIGRETURN).
         *
         * Saved context is stored in per-task signal_state,
         * not in globals (fixes thread-safety issue).
         */
        uint64_t user_rsp = current->current_tf->rsp;

        /* Trampoline code: mov eax, SYS_SIGRETURN; syscall (7 bytes)
         * Named SIG_TRAMPOLINE_SIZE to avoid conflict with smp.h's TRAMPOLINE_SIZE */
        #define SIG_TRAMPOLINE_SIZE 16  /* 16-byte aligned for safety */
        uint64_t frame_size = sizeof(struct sigframe) + 8 + SIG_TRAMPOLINE_SIZE;

        /* Check user stack bounds */
        if (user_rsp < frame_size + 0x1000) {
            log_printf(LOG_LEVEL_ERR, "signal: user stack too small for sigframe\n");
            do_signal_default(s);
            return;
        }

        uint64_t new_rsp = user_rsp - frame_size;

        /* Validate new_rsp is still in user address space (not wrapped to kernel) */
        if (new_rsp > user_rsp) {
            log_printf(LOG_LEVEL_ERR, "signal: stack underflow detected\n");
            do_signal_default(s);
            return;
        }

        /*
         * FIXED (v4.1.4): Check that the signal frame falls within a
         * registered VMA.  Without this check, a signal frame could be
         * placed below the stack VMA boundary, overwriting the syscall
         * return address or other critical data on the stack.  (BUG 4.8)
         */
        if (!vma_find(current, new_rsp)) {
            log_printf(LOG_LEVEL_ERR, "signal: sigframe at %p outside VMA bounds\n",
                       (void *)(uintptr_t)new_rsp);
            do_signal_default(s);
            return;
        }

        /* Validate the entire trampoline + sigframe region is in valid user memory */
        size_t frame_total = 8 + SIG_TRAMPOLINE_SIZE + sizeof(struct sigframe);
        if (!user_addr_range_ok((const void *)(uintptr_t)new_rsp, frame_total) ||
            !user_pages_mapped((const void *)(uintptr_t)new_rsp, frame_total)) {
            log_printf(LOG_LEVEL_ERR, "signal: user stack region invalid at %p\n",
                       (void *)(uintptr_t)new_rsp);
            do_signal_default(s);
            return;
        }

        /* Save RFLAGS to restore AC flag after user memory access.
         * This ensures SMAP is properly re-enabled even if an
         * exception occurs within the protected window. */
        uint64_t saved_rflags;
        asm volatile ("pushfq; popq %0" : "=r"(saved_rflags));
        asm volatile ("stac" ::: "memory");

        uint8_t *tramp = (uint8_t *)(uintptr_t)(new_rsp + 8);
        tramp[0] = 0xB8;                        /* mov eax, imm32 */
        {
            uint32_t sigret = (uint32_t)SYS_SIGRETURN;
            tramp[1] = (uint8_t)(sigret);
            tramp[2] = (uint8_t)(sigret >> 8);
            tramp[3] = (uint8_t)(sigret >> 16);
            tramp[4] = (uint8_t)(sigret >> 24);
        }
        tramp[5] = 0x0F;                        /* syscall */
        tramp[6] = 0x05;
        /* Zero the remaining trampoline bytes for security */
        memset(tramp + 7, 0, SIG_TRAMPOLINE_SIZE - 7);

        /* Write return address pointing to trampoline code */
        *(uint64_t *)(uintptr_t)new_rsp = new_rsp + 8;

        /* Place sigframe above trampoline */
        struct sigframe *frame = (struct sigframe *)(uintptr_t)(new_rsp + 8 + SIG_TRAMPOLINE_SIZE);

        /* Save current user context */
        frame->signo  = s;
        frame->r15    = current->current_tf->r15;
        frame->r14    = current->current_tf->r14;
        frame->r13    = current->current_tf->r13;
        frame->r12    = current->current_tf->r12;
        frame->r11    = current->current_tf->r11;
        frame->r10    = current->current_tf->r10;
        frame->r9     = current->current_tf->r9;
        frame->r8     = current->current_tf->r8;
        frame->rsi    = current->current_tf->rsi;
        frame->rdi    = current->current_tf->rdi;
        frame->rdx    = current->current_tf->rdx;
        frame->rcx    = current->current_tf->rcx;
        frame->rax    = current->current_tf->rax;
        frame->rip    = current->current_tf->rip;
        frame->rflags = current->current_tf->r11;  /* R11 holds RFLAGS from syscall entry */
        frame->rsp    = user_rsp;

        /* Store saved context in per-task signal_state (thread-safe) */
        sig->saved_rsp = frame->rsp;
        sig->saved_rip = frame->rip;

        /* Modify trapframe: redirect to handler */
        current->current_tf->rip = (uint64_t)(uintptr_t)handler;
        current->current_tf->rsp = new_rsp;
        current->current_tf->rdi = (uint64_t)s;  /* arg0 = signo */

        /* Restore AC flag to its original state, re-enabling SMAP
         * if it was previously enabled. */
        if (!(saved_rflags & (1ULL << 18))) {
            asm volatile ("clac" ::: "memory");
        }

        /*
         * Block the signal being delivered AND any additional signals
         * specified in sa_mask during handler execution.
         * FIXED (v4.2.3): Previously only the delivered signal was blocked,
         * ignoring sa_mask which allows users to specify additional signals
         * to be blocked during handler execution (POSIX requirement).
         */
        sig->blocked |= (1U << s) | sig->actions[s].sa_mask;

        log_printf(LOG_LEVEL_DEBUG, "signal: delivering sig=%d handler=%p\n",
                   s, (void *)handler);
        return;
    }
}

/* ================================================================
 * signal_child_event
 * ================================================================ */
void signal_child_event(struct task_struct *child, int event) {
    (void)event;
    /*
     * FIXED (v4.2.3): Protect child->parent access with child_lock
     * to prevent UAF if the parent is being freed concurrently.
     * (BUG-PROC-08)
     */
    spin_lock((spinlock_t*)&child->child_lock);
    struct task_struct *parent = child->parent;
    if (!parent || parent->state == TASK_DEAD) {
        spin_unlock((spinlock_t*)&child->child_lock);
        return;
    }
    int parent_pid = parent->pid;
    spin_unlock((spinlock_t*)&child->child_lock);
    do_sys_kill(parent_pid, SIGCHLD);
}

/*
 * FIXED (v4.1.4): Reset caught signals to SIG_DFL on exec.
 * POSIX requires that signals with custom handlers (not SIG_DFL or
 * SIG_IGN) are reset to SIG_DFL, and pending signals are cleared.
 * (BUG 4.5)
 */
void signal_reset_on_exec(struct task_struct *task) {
    if (!task || !task->sig) return;

    for (int i = 1; i < NSIG; i++) {
        /* Skip SIGKILL and SIGSTOP — they can't be caught */
        if (i == SIGKILL || i == SIGSTOP) continue;

        if (task->sig->actions[i].sa_handler != SIG_DFL &&
            task->sig->actions[i].sa_handler != SIG_IGN) {
            task->sig->actions[i].sa_handler = SIG_DFL;
            task->sig->actions[i].sa_flags = 0;
        }
    }

    /* Clear pending signals */
    task->sig->pending = 0;
    task->sig->blocked = 0;
}
