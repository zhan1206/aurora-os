/*
 * explain.h - Kernel decision explainability interface
 *
 * Provides human-readable explanations for kernel decisions.
 * Used by the shell for error messages and by the kernel
 * for logging decision rationale.
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

/* Record an exit event */
void explain_exit(int pid, int code, int signal);

/* Record a signal delivery event */
void explain_signal(int pid, int sig, const char *action);

/* Record an OOM kill event */
void explain_oom_kill(int victim_pid, const char *reason);

/* Dump all recorded events to the given emitter */
void explain_dump(void (*emit)(const char *line));

/* Convert errno to human-readable string */
const char *explain_errno(int err);

#endif /* EXPLAIN_H */