/*
 * FIXED (v4.1.8): Removed <stdio.h> and standard printf().
 * Freestanding environments do not have the standard C library.
 * Now uses the kernel's built-in printf() available via libc.h.
 * (BUG C-18 / H-5)
 */
#include <stddef.h>

/* Declare kernel printf (provided by libc/kernel) */
extern int printf(const char *fmt, ...);

int main(void) {
    printf("Hello from userspace!\n");
    return 0;
}