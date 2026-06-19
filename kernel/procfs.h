/*
 * procfs.h - /proc filesystem interface for AuroraOS
 *
 * Provides a simple proc filesystem that exposes kernel information
 * through virtual files. Each file is backed by a function that
 * generates its content on read.
 */
#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>
#include <stddef.h>

/* A procfs entry: a virtual file with a name and a content generator */
struct procfs_entry {
    const char *name;
    int (*read_func)(char *buf, size_t size);
    int is_dir;  /* 1 if this entry is a directory */
};

/* Initialize the procfs subsystem and return the super block */
struct super_block *procfs_create(void);

/*
 * procfs_init: Create procfs and mount it at /proc.
 * Must be called after vfs_mount_root().
 */
void procfs_init(void);

#endif /* PROCFS_H */