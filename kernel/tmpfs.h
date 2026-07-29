/*
 * tmpfs.h - Memory-backed temporary filesystem for AuroraOS
 *
 * FIXED (v4.3.3): TMPFS-001 — Implement tmpfs filesystem.
 * Previously /tmp was mounted as RamFS instead of tmpfs.
 * tmpfs is a memory-backed filesystem that supports all VFS operations
 * without persistence.  It uses the slab allocator for metadata and
 * alloc_pages() for file data.
 */
#ifndef TMPFS_H
#define TMPFS_H

struct super_block *tmpfs_create(void);
void tmpfs_init(void);

#endif /* TMPFS_H */