/*
 * signal.h - POSIX signal framework (Phase 1)
 */
#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>
#include <stddef.h>

/* Forward declaration for struct task_struct (defined in sched.h) */
struct task_struct;

/* Signal numbers */
#define SIGHUP    1
#define SIGINT    2
#define SIGSEGV   11
#define SIGKILL   9
#define SIGTERM   15
#define SIGCHLD   17
#define NSIG      32

/* Signal actions */
#define SIG_DFL  ((void *)0)
#define SIG_IGN  ((void *)1)
#define SIG_ERR  ((void *)-1)

/* sigaction flags */
#define SA_NOCLDSTOP  1
#define SA_RESTART    2

typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    uint64_t     sa_flags;
    uint64_t     sa_mask;
};

struct sigframe {
    uint64_t ret_addr;
    uint64_t signo;      /* uint64_t for 8-byte alignment of subsequent fields */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rdx, rcx, rax;
    uint64_t rip, rflags, rsp;
};

struct signal_state {
    struct sigaction actions[NSIG];
    uint32_t         pending;
    uint32_t         blocked;
    uint64_t         saved_rsp;  /* for sigreturn: original user RSP */
    uint64_t         saved_rip;  /* for sigreturn: original user RIP */
};

/* Signal API — implementation in signal.c */

/* Send a signal to a process */
int do_sys_kill(int pid, int sig);

/* Set signal handler */
int do_sys_sigaction(int signo, const struct sigaction *act,
                      struct sigaction *oldact);

/* Return from signal handler */
void do_sys_sigreturn(void);

/* Check and deliver pending signals at safe points */
void check_signals(void);

/* Execute default action for a signal */
void do_signal_default(int sig);

/* Notify parent about child state change */
void signal_child_event(struct task_struct *child, int event);

/* Allocate a new signal_state (lazy allocation) */
struct signal_state *signal_alloc(void);

#endif /* SIGNAL_H */
