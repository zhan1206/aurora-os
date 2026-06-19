#ifndef TRAPFRAME_H
#define TRAPFRAME_H

#include <stdint.h>

/* Layout matches pushes in arch/x86_64/syscall.S */
struct trapframe {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rdx;
    uint64_t rcx; /* contains user RIP for SYSCALL */
    uint64_t rax; /* syscall number on entry, return value on exit */
    uint64_t rip; /* user RIP (for signal delivery) */
    uint64_t rsp; /* user RSP (for signal delivery) */
};

/* Global trapframe pointer for signal delivery (set by syscall_trap) */
extern struct trapframe *current_tf;

#endif
