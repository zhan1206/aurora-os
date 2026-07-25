/*
 * user_access.h - Unified user-space access macros
 * /* USER_ACCESS (v4.2.7) */
 *
 * Centralizes the scattered stac()/clac() calls in the kernel into
 * unified, safe macros with built-in address validation.  Replaces
 * ad-hoc SMAP toggling with a single, auditable API.
 *
 * All kernel code that touches user-space pointers should use these
 * macros instead of directly calling stac()/clac().
 */
#ifndef USER_ACCESS_H
#define USER_ACCESS_H

#include "mem.h"

/*
 * user_access_begin / user_access_end: Temporarily allow the kernel to
 * access user-space memory.  Must be paired — every begin() must have
 * a matching end() on every code path.
 */
#define user_access_begin()  stac()
#define user_access_end()    clac()

/*
 * copy_from_user_safe: Copy data from user space into a kernel buffer.
 * Validates the user address range before copying.
 *
 * @dst:  kernel buffer (must be valid kernel memory).
 * @src:  user-space pointer (validated).
 * @len:  number of bytes to copy.
 * Returns 0 on success, -1 if the user range is invalid.
 */
static inline int copy_from_user_safe(void *dst, const void __user *src, size_t len) {
    if (!user_addr_range_ok(src, len)) return -1;
    user_access_begin();
    memcpy(dst, src, len);
    user_access_end();
    return 0;
}

/*
 * copy_to_user_safe: Copy data from a kernel buffer to user space.
 * Validates the user address range before copying.
 *
 * @dst:  user-space pointer (validated).
 * @src:  kernel buffer.
 * @len:  number of bytes to copy.
 * Returns 0 on success, -1 if the user range is invalid.
 */
static inline int copy_to_user_safe(void __user *dst, const void *src, size_t len) {
    if (!user_addr_range_ok(dst, len)) return -1;
    user_access_begin();
    memcpy(dst, src, len);
    user_access_end();
    return 0;
}

#endif /* USER_ACCESS_H */