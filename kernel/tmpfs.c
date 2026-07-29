/*
 * tmpfs.c - Memory-backed temporary filesystem
 *
 * FIXED (v4.3.3): TMPFS-001 — Implement tmpfs filesystem.
 * Previously /tmp was mounted as RamFS instead of tmpfs.
 * tmpfs is a memory-backed filesystem that supports all VFS operations
 * without persistence.  It uses the slab allocator for metadata and
 * alloc_pages() for file data.
 */

#include "tmpfs.h"
#include "fs.h"
#include "vfs.h"
#include "mem.h"
#include "smp.h"
#include "include/log.h"
#include <string.h>
#include <stdint.h>

/* ================================================================
 * tmpfs inode private data
 * ================================================================ */
struct tmpfs_inode {
    uint32_t ino;
    uint32_t mode;
    uint32_t uid, gid;
    uint64_t atime, mtime, ctime;
    uint32_t nlink;  /* FIXED (v4.3.3): TMPFS-001 — proper nlink tracking */
    void    *data;    /* file data (kmalloc) */
    struct tmpfs_inode *next;     /* next sibling in parent's children list */
    struct tmpfs_inode *children; /* first child (for directories) */
};

/* ================================================================
 * Static state
 * ================================================================ */
static struct tmpfs_inode *tmpfs_root = NULL;
static spinlock_t tmpfs_lock = {0};
static uint32_t tmpfs_next_ino = 1;

/* Forward declarations */
static struct file_ops tmpfs_file_ops;
static struct file_ops tmpfs_dir_ops;

/* ================================================================
 * File operations
 * ================================================================ */

static int tmpfs_open(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp; return 0;
}

static ssize_t tmpfs_read(struct file *filp, void *buf, size_t count,
                          off_t *offset) {
    struct tmpfs_inode *n = (struct tmpfs_inode *)filp->inode->priv;
    if (!n || !buf) return -1;

    spin_lock(&tmpfs_lock);
    if (*offset >= (off_t)n->inode.size) {
        spin_unlock(&tmpfs_lock);
        return 0;
    }
    if (!n->data) {
        spin_unlock(&tmpfs_lock);
        return 0;
    }

    size_t toread = count;
    if ((size_t)(*offset) + toread > n->inode.size)
        toread = n->inode.size - (size_t)(*offset);
    memcpy(buf, (char *)n->data + (*offset), toread);
    *offset += (off_t)toread;
    spin_unlock(&tmpfs_lock);
    return (ssize_t)toread;
}

static ssize_t tmpfs_write(struct file *filp, const void *buf, size_t count,
                           off_t *offset) {
    struct tmpfs_inode *n = (struct tmpfs_inode *)filp->inode->priv;
    if (!n || !buf || !offset) return -1;
    if (*offset < 0) return -1;
    if (count == 0) return 0;

    if ((size_t)(*offset) > SIZE_MAX - count) return -1;
    size_t new_size = (size_t)(*offset) + count;

    spin_lock(&tmpfs_lock);

    if (new_size > n->inode.size) {
        void *new_data = kmalloc(new_size);
        if (!new_data) { spin_unlock(&tmpfs_lock); return -1; }
        if (n->data && n->inode.size > 0)
            memcpy(new_data, n->data, n->inode.size);
        if (n->data) kfree(n->data);
        n->data = new_data;
        n->inode.size = new_size;
    }

    memcpy((char *)n->data + (*offset), buf, count);
    *offset += (off_t)count;
    n->mtime = 0;  /* FIXED (v4.3.3): TMPFS-001 — update mtime on write */

    spin_unlock(&tmpfs_lock);
    return (ssize_t)count;
}

static int tmpfs_close(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp; return 0;
}

/* ================================================================
 * Directory operations
 * ================================================================ */

static int tmpfs_lookup(struct inode *dir, struct dentry *dentry) {
    struct tmpfs_inode *head = (struct tmpfs_inode *)dir->priv;
    spin_lock(&tmpfs_lock);
    struct tmpfs_inode *n = head->children;

    while (n) {
        if (n->inode.name && strcmp(n->inode.name, dentry->name) == 0) {
            dentry->inode = &n->inode;
            n->inode.dentry = dentry;
            spin_unlock(&tmpfs_lock);
            return 0;
        }
        n = n->next;
    }
    spin_unlock(&tmpfs_lock);
    return -1;
}

static int tmpfs_create_file(struct inode *dir, const char *name, int flags) {
    (void)flags;
    struct tmpfs_inode *head = (struct tmpfs_inode *)dir->priv;
    if (!head || !head->inode.is_dir || !name) return -1;

    spin_lock(&tmpfs_lock);

    struct tmpfs_inode *existing = head->children;
    while (existing) {
        if (existing->inode.name && strcmp(existing->inode.name, name) == 0) {
            spin_unlock(&tmpfs_lock);
            return -1;
        }
        existing = existing->next;
    }

    struct tmpfs_inode *n = (struct tmpfs_inode *)kmalloc(sizeof(*n));
    if (!n) { spin_unlock(&tmpfs_lock); return -1; }
    memset(n, 0, sizeof(*n));

    n->inode.name = (const char *)kmalloc(strlen(name) + 1);
    if (!n->inode.name) { kfree(n); spin_unlock(&tmpfs_lock); return -1; }
    strcpy((char *)n->inode.name, name);

    n->ino = tmpfs_next_ino++;
    n->mode = 0644;
    n->uid = 0;
    n->gid = 0;
    n->nlink = 1;
    n->inode.size = 0;
    n->data = NULL;
    n->inode.ops = &tmpfs_file_ops;
    n->inode.is_dir = 0;
    n->inode.priv = n;
    n->inode.mode = (int)n->mode;

    n->next = head->children;
    head->children = n;

    spin_unlock(&tmpfs_lock);
    return 0;
}

static int tmpfs_mkdir(struct inode *dir, const char *name) {
    struct tmpfs_inode *head = (struct tmpfs_inode *)dir->priv;
    if (!head || !head->inode.is_dir || !name) return -1;

    spin_lock(&tmpfs_lock);

    struct tmpfs_inode *existing = head->children;
    while (existing) {
        if (existing->inode.name && strcmp(existing->inode.name, name) == 0) {
            spin_unlock(&tmpfs_lock);
            return -1;
        }
        existing = existing->next;
    }

    struct tmpfs_inode *n = (struct tmpfs_inode *)kmalloc(sizeof(*n));
    if (!n) { spin_unlock(&tmpfs_lock); return -1; }
    memset(n, 0, sizeof(*n));

    n->inode.name = (const char *)kmalloc(strlen(name) + 1);
    if (!n->inode.name) { kfree(n); spin_unlock(&tmpfs_lock); return -1; }
    strcpy((char *)n->inode.name, name);

    n->ino = tmpfs_next_ino++;
    n->mode = 0755;
    n->uid = 0;
    n->gid = 0;
    n->nlink = 2;  /* FIXED (v4.3.3): TMPFS-001 — "." and ".." */
    n->inode.size = 0;
    n->data = NULL;
    n->inode.ops = &tmpfs_dir_ops;
    n->inode.is_dir = 1;
    n->inode.priv = n;
    n->inode.mode = (int)n->mode;
    n->children = NULL;

    /* FIXED (v4.3.3): TMPFS-001 — increment parent nlink for ".." */
    head->nlink++;

    n->next = head->children;
    head->children = n;

    spin_unlock(&tmpfs_lock);
    return 0;
}

static int tmpfs_unlink(struct inode *dir, const char *name) {
    struct tmpfs_inode *head = (struct tmpfs_inode *)dir->priv;
    if (!head || !head->inode.is_dir || !name) return -1;

    spin_lock(&tmpfs_lock);

    struct tmpfs_inode *prev = NULL;
    struct tmpfs_inode *cur = head->children;
    while (cur) {
        if (cur->inode.name && strcmp(cur->inode.name, name) == 0) {
            if (cur->inode.is_dir) { spin_unlock(&tmpfs_lock); return -1; }
            if (prev)
                prev->next = cur->next;
            else
                head->children = cur->next;
            if (cur->inode.name) kfree((void *)cur->inode.name);
            if (cur->data) kfree(cur->data);
            kfree(cur);
            spin_unlock(&tmpfs_lock);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    spin_unlock(&tmpfs_lock);
    return -1;
}

static int tmpfs_rmdir(struct inode *dir, const char *name) {
    struct tmpfs_inode *head = (struct tmpfs_inode *)dir->priv;
    if (!head || !head->inode.is_dir || !name) return -1;

    spin_lock(&tmpfs_lock);

    struct tmpfs_inode *prev = NULL;
    struct tmpfs_inode *cur = head->children;
    while (cur) {
        if (cur->inode.name && strcmp(cur->inode.name, name) == 0) {
            if (!cur->inode.is_dir) { spin_unlock(&tmpfs_lock); return -1; }
            if (cur->children) { spin_unlock(&tmpfs_lock); return -1; }
            if (prev)
                prev->next = cur->next;
            else
                head->children = cur->next;
            /* FIXED (v4.3.3): TMPFS-001 — decrement parent nlink */
            head->nlink--;
            if (cur->inode.name) kfree((void *)cur->inode.name);
            kfree(cur);
            spin_unlock(&tmpfs_lock);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    spin_unlock(&tmpfs_lock);
    return -1;
}

/* ================================================================
 * File operations tables
 * ================================================================ */
static struct file_ops tmpfs_file_ops = {
    .open   = tmpfs_open,
    .read   = tmpfs_read,
    .write  = tmpfs_write,
    .close  = tmpfs_close,
    .lookup = NULL,
};

static struct file_ops tmpfs_dir_ops = {
    .open   = tmpfs_open,
    .read   = NULL,
    .write  = NULL,
    .close  = tmpfs_close,
    .lookup = tmpfs_lookup,
    .create = tmpfs_create_file,
    .mkdir  = tmpfs_mkdir,
    .unlink = tmpfs_unlink,
    .rmdir  = tmpfs_rmdir,
};

/* ================================================================
 * tmpfs_create: Create a new tmpfs super block
 * ================================================================ */
struct super_block *tmpfs_create(void) {
    struct tmpfs_inode *head = (struct tmpfs_inode *)kmalloc(sizeof(*head));
    if (!head) return NULL;
    memset(head, 0, sizeof(*head));

    head->ino = tmpfs_next_ino++;
    head->mode = 0755;
    head->uid = 0;
    head->gid = 0;
    head->nlink = 2;  /* FIXED (v4.3.3): TMPFS-001 — root directory nlink */
    head->inode.name  = "";
    head->inode.ops   = &tmpfs_dir_ops;
    head->inode.is_dir = 1;
    head->inode.priv  = head;
    head->inode.mode  = (int)head->mode;
    head->next = NULL;
    head->children = NULL;

    struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
    if (!sb) { kfree(head); return NULL; }
    memset(sb, 0, sizeof(*sb));
    sb->fs_name = "tmpfs";
    sb->root    = &head->inode;
    sb->sb_data = head;
    tmpfs_root  = head;

    return sb;
}

/* ================================================================
 * tmpfs_init: Create tmpfs and mount it at /tmp
 * ================================================================ */
void tmpfs_init(void) {
    struct super_block *tmpfs_sb = tmpfs_create();
    if (!tmpfs_sb) {
        log_printf(LOG_LEVEL_ERR, "tmpfs: failed to create super block\n");
        return;
    }

    if (vfs_mount("/tmp", tmpfs_sb) < 0) {
        log_printf(LOG_LEVEL_ERR, "tmpfs: failed to mount at /tmp\n");
        kfree(tmpfs_sb);
        return;
    }

    log_printf(LOG_LEVEL_INFO, "tmpfs: mounted at /tmp\n");
}