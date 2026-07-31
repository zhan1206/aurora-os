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

/* FIXED (v4.3.8): MAGIC-001 — Named constants to replace magic numbers. */
#define PAGE_SIZE_4K    4096
#define FS_BLOCK_SIZE   1024
#define MAX_PATH_LEN    256
#define MAX_CMD_LEN     128
#define MAX_ARGS        64

#endif /* KERNEL_TYPES_H */
