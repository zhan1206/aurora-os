/*
 * vfs.h - Virtual File System interface
 */
#ifndef VFS_H
#define VFS_H

#include "fs.h"

void vfs_init(void);
int vfs_mount_root(struct super_block *sb);

struct inode *vfs_lookup(const char *path);
struct file *vfs_open(const char *path, int flags);
ssize_t vfs_read(struct file *filp, void *buf, size_t count);
ssize_t vfs_write(struct file *filp, const void *buf, size_t count);
int vfs_close(struct file *filp);
int vfs_file_dup(struct file *filp);
struct super_block *vfs_get_root_sb(void);

#endif
