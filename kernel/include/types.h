/*
 * types.h - Kernel-specific POSIX-compatible type definitions
 * Required for -ffreestanding builds where libc headers are unavailable.
 */
#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

/* Signed size type (typically long on 64-bit) */
typedef long ssize_t;

/* File offset type (64-bit) */
typedef long long off_t;

#endif /* KERNEL_TYPES_H */
