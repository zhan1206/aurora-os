/*
 * vfs_safe_copy.h - VFS-SMAP (v4.2.6)
 *
 * Safe user/kernel buffer copy mechanism for the VFS layer.
 *
 * Problem: vfs_read()/vfs_write() pass user-space pointers through to
 * filesystem operations. Some filesystems (ramfs, ext2, fat32, devfs)
 * may directly memcpy to/from user pointers without SMAP protection.
 *
 * Solution: Instead of fixing each filesystem individually, this
 * generic safe buffer layer provides kernel-side staging buffers.
 * When vfs_read/vfs_write detect a user-space pointer, the data is
 * first copied to/from a kernel vfs_iobuf (with stac/clac), and the
 * filesystem operation receives a safe kernel pointer.
 *
 * This is transparent to filesystem implementations — they don't
 * need any changes.
 */
#ifndef VFS_SAFE_COPY_H
#define VFS_SAFE_COPY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mem.h"
#include "pagetable.h"
#include "include/userspace.h"

/* Default kernel buffer size for VFS safe I/O (one page) */
#define VFS_IOBUF_SIZE  4096

/*
 * vfs_iobuf: A kernel-side staging buffer for safe user-space I/O.
 *
 * Allocated from kernel heap (kmalloc). The filesystem reads/writes
 * into ->data, which is always a valid kernel pointer. The VFS layer
 * handles the stac/clac-protected copy between ->data and the user
 * buffer.
 */
struct vfs_iobuf {
    void   *data;   /* kernel heap buffer */
    size_t  size;   /* allocated size (typically VFS_IOBUF_SIZE) */
};

/*
 * vfs_iobuf_init: Allocate a kernel buffer for safe I/O.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static inline int vfs_iobuf_init(struct vfs_iobuf *buf, size_t size) {
    if (!buf || size == 0) return -1;
    buf->data = kmalloc(size);
    if (!buf->data) {
        buf->size = 0;
        return -1;
    }
    buf->size = size;
    return 0;
}

/*
 * vfs_iobuf_free: Free the kernel buffer.
 */
static inline void vfs_iobuf_free(struct vfs_iobuf *buf) {
    if (buf && buf->data) {
        kfree(buf->data);
        buf->data = NULL;
        buf->size = 0;
    }
}

/*
 * vfs_copy_from_user_safe: Copy from user space into the kernel iobuf.
 *
 * Uses stac/clac to safely access user memory.
 * Returns 0 on success, -1 if the user range is invalid or unmapped.
 */
static inline int vfs_copy_from_user_safe(struct vfs_iobuf *buf,
                                           const void *src, size_t len) {
    if (!buf || !buf->data || !src || len == 0) return -1;
    if (len > buf->size) return -1;
    if (!user_addr_range_ok(src, len)) return -1;
    if (!user_pages_mapped(src, len)) return -1;
    stac();
    memcpy(buf->data, src, len);
    clac();
    return 0;
}

/*
 * vfs_copy_to_user_safe: Copy from the kernel iobuf to user space.
 *
 * Uses stac/clac to safely access user memory.
 * Returns 0 on success, -1 if the user range is invalid or unmapped.
 */
static inline int vfs_copy_to_user_safe(void *dst, struct vfs_iobuf *buf,
                                         size_t len) {
    if (!buf || !buf->data || !dst || len == 0) return -1;
    if (len > buf->size) return -1;
    if (!user_addr_range_ok(dst, len)) return -1;
    if (!user_pages_mapped(dst, len)) return -1;
    stac();
    memcpy(dst, buf->data, len);
    clac();
    return 0;
}

#endif /* VFS_SAFE_COPY_H */