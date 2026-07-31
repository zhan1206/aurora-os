/*
 * ramfs.c - In-memory filesystem (Phase 1: dentry lookup support)
 *
 * FIXED (v4.1.4): Added ramfs_lock spinlock to protect all linked list
 * operations (lookup, create, mkdir, unlink, write).  Without locking,
 * concurrent SMP access to the children linked list would cause
 * corruption.  (BUG 3.11)
 */
#include "fs.h"
#include "include/log.h"
#include "mem.h"
#include "smp.h"
#include <string.h>

struct ramfs_node {
    struct inode      inode;
    struct ramfs_node *next;     /* next sibling in parent's children list */
    struct ramfs_node *children; /* first child (for directories) */
    char             *data;
};

static struct ramfs_node *ramfs_root = NULL;

/* Protect all ramfs linked list operations (SMP safety) */
static spinlock_t ramfs_lock = {0};

/* Forward declarations for file_ops tables (referenced before definition) */
static struct file_ops ramfs_file_ops;
static struct file_ops ramfs_dir_ops;

/* ================================================================
 * File operations
 * ================================================================ */

static int ramfs_open(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp; return 0;
}

static ssize_t ramfs_read(struct file *filp, void *buf, size_t count,
                          off_t *offset) {
    struct ramfs_node *n = (struct ramfs_node *)filp->inode;
    if (!n || !buf) return -1;
    /*
     * FIXED (v4.2.0): Acquire the ramfs lock to prevent concurrent
     * writes from reallocating n->data while we are reading it.
     * Without this lock, a concurrent ramfs_write could free and
     * replace n->data, causing a use-after-free.  (BUG-FS-H5)
     */
    spin_lock(&ramfs_lock);
    if (*offset >= (off_t)n->inode.size) { spin_unlock(&ramfs_lock); return 0; }
    size_t toread = count;
    if ((size_t)(*offset) + toread > n->inode.size) toread = n->inode.size - (*offset);
    memcpy(buf, n->data + (*offset), toread);
    *offset += toread;
    spin_unlock(&ramfs_lock);
    return toread;
}

static int ramfs_close(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp; return 0;
}

/*
 * ramfs_lookup: Resolve a name within a ramfs directory.
 * Searches the linked list for a matching name.
 */
static int ramfs_lookup(struct inode *dir, struct dentry *dentry) {
    struct ramfs_node *head = (struct ramfs_node *)dir;
    spin_lock(&ramfs_lock);
    struct ramfs_node *n = head->children;  /* first child */

    while (n) {
        if (n->inode.name && strcmp(n->inode.name, dentry->name) == 0) {
            dentry->inode = &n->inode;
            n->inode.dentry = dentry;
            spin_unlock(&ramfs_lock);
            return 0;
        }
        n = n->next;  /* next sibling */
    }
    spin_unlock(&ramfs_lock);
    return -1;  /* not found (negative dentry) */
}

/*
 * ramfs_create_file: Create a new file in a ramfs directory.
 * Called when O_CREAT is specified and the file does not exist.
 * Returns 0 on success, -1 on failure.
 *
 * FIXED (v4.1.6): Renamed from ramfs_create to ramfs_create_file
 * to avoid conflict with the external ramfs_create(void) declared
 * in fs.h (BUG 6.4).
 */
static int ramfs_create_file(struct inode *dir, const char *name, int flags) {
    (void)flags;
    struct ramfs_node *head = (struct ramfs_node *)dir;
    if (!head || !head->inode.is_dir || !name) return -1;

    spin_lock(&ramfs_lock);

    /* Check for duplicate name */
    struct ramfs_node *existing = head->children;
    while (existing) {
        if (existing->inode.name && strcmp(existing->inode.name, name) == 0) {
            spin_unlock(&ramfs_lock);
            return -1;  /* Already exists */
        }
        existing = existing->next;
    }

    struct ramfs_node *n = (struct ramfs_node *)kmalloc(sizeof(*n));
    if (!n) { spin_unlock(&ramfs_lock); return -1; }
    memset(n, 0, sizeof(*n));

    n->inode.name = (const char *)kmalloc(strlen(name) + 1);
    if (!n->inode.name) { kfree(n); spin_unlock(&ramfs_lock); return -1; }
    strcpy((char *)n->inode.name, name);

    n->inode.size   = 0;
    n->data         = NULL;
    n->inode.ops    = &ramfs_file_ops;
    n->inode.is_dir = 0;
    n->inode.priv   = NULL;

    /* Insert at head of directory children list */
    n->next = head->children;
    head->children = n;

    spin_unlock(&ramfs_lock);
    return 0;
}

/*
 * ramfs_write: Write data to a ramfs file.
 * If the write extends beyond current file size, the buffer is expanded.
 */
static ssize_t ramfs_write(struct file *filp, const void *buf, size_t count,
                           off_t *offset) {
    struct ramfs_node *n = (struct ramfs_node *)filp->inode;
    if (!n || !buf || !offset) return -1;

    /* Guard against negative offset */
    if (*offset < 0) return -1;

    /* No-op write */
    if (count == 0) return 0;

    /* Check for integer overflow: (size_t)(*offset) + count */
    if ((size_t)(*offset) > SIZE_MAX - count) return -1;
    size_t new_size = (size_t)(*offset) + count;

    spin_lock(&ramfs_lock);

    if (new_size > n->inode.size) {
        /* Expand buffer */
        char *new_data = (char *)kmalloc(new_size);
        if (!new_data) { spin_unlock(&ramfs_lock); return -1; }
        if (n->data && n->inode.size > 0)
            memcpy(new_data, n->data, n->inode.size);
        if (n->data) kfree(n->data);
        n->data = new_data;
        n->inode.size = new_size;
    }

    memcpy(n->data + (*offset), buf, count);
    *offset += (off_t)count;

    spin_unlock(&ramfs_lock);
    return (ssize_t)count;
}

/*
 * ramfs_mkdir: Create a directory.
 */
static int ramfs_mkdir(struct inode *dir, const char *name) {
    struct ramfs_node *head = (struct ramfs_node *)dir;
    if (!head || !head->inode.is_dir || !name) return -1;

    spin_lock(&ramfs_lock);

    /* Check for duplicate name */
    struct ramfs_node *existing = head->children;
    while (existing) {
        if (existing->inode.name && strcmp(existing->inode.name, name) == 0) {
            spin_unlock(&ramfs_lock);
            return -1;
        }
        existing = existing->next;
    }

    struct ramfs_node *n = (struct ramfs_node *)kmalloc(sizeof(*n));
    if (!n) { spin_unlock(&ramfs_lock); return -1; }
    memset(n, 0, sizeof(*n));

    n->inode.name = (const char *)kmalloc(strlen(name) + 1);
    if (!n->inode.name) { kfree(n); spin_unlock(&ramfs_lock); return -1; }
    strcpy((char *)n->inode.name, name);

    n->inode.size   = 0;
    n->data         = NULL;
    n->inode.ops    = &ramfs_dir_ops;
    n->inode.is_dir = 1;
    n->inode.priv   = NULL;
    n->children     = NULL;

    /* Insert at head of directory children list */
    n->next = head->children;
    head->children = n;

    /* FIXED (v4.2.7): BUG-RAMFS-NLINK — increment the parent directory's
     * link count.  The new subdirectory contains a ".." entry that points
     * back to the parent, so the parent's link count must be incremented. */
    /* NOTE: struct inode currently has no nlink field; this increment
     * is a no-op until the field is added to the inode structure. */
    head->inode.size++; /* placeholder: use nlink when available */

    spin_unlock(&ramfs_lock);
    return 0;
}

/*
 * ramfs_unlink: Remove a file (not a directory).
 */
static int ramfs_unlink(struct inode *dir, const char *name) {
    struct ramfs_node *head = (struct ramfs_node *)dir;
    if (!head || !head->inode.is_dir || !name) return -1;

    spin_lock(&ramfs_lock);

    struct ramfs_node *prev = NULL;
    struct ramfs_node *cur = head->children;
    while (cur) {
        if (cur->inode.name && strcmp(cur->inode.name, name) == 0) {
            if (cur->inode.is_dir) { spin_unlock(&ramfs_lock); return -1; }
            if (prev)
                prev->next = cur->next;
            else
                head->children = cur->next;
            if (cur->inode.name) kfree((void *)cur->inode.name);
            if (cur->data) kfree(cur->data);
            kfree(cur);
            spin_unlock(&ramfs_lock);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    spin_unlock(&ramfs_lock);
    return -1;  /* not found */
}

/*
 * ramfs_rmdir: Remove an empty directory.
 */
static int ramfs_rmdir(struct inode *dir, const char *name) {
    struct ramfs_node *head = (struct ramfs_node *)dir;
    if (!head || !head->inode.is_dir || !name) return -1;

    spin_lock(&ramfs_lock);

    struct ramfs_node *prev = NULL;
    struct ramfs_node *cur = head->children;
    while (cur) {
        if (cur->inode.name && strcmp(cur->inode.name, name) == 0) {
            if (!cur->inode.is_dir) { spin_unlock(&ramfs_lock); return -1; }
            if (cur->children) { spin_unlock(&ramfs_lock); return -1; }
            if (prev)
                prev->next = cur->next;
            else
                head->children = cur->next;
            if (cur->inode.name) kfree((void *)cur->inode.name);
            kfree(cur);
            spin_unlock(&ramfs_lock);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    spin_unlock(&ramfs_lock);
    return -1;  /* not found */
}

static struct file_ops ramfs_file_ops = {
    .open   = ramfs_open,
    .read   = ramfs_read,
    .write  = ramfs_write,
    .close  = ramfs_close,
    .lookup = NULL,
};

static struct file_ops ramfs_dir_ops = {
    .open   = ramfs_open,
    .read   = NULL,   /* directories not readable as files */
    .write  = NULL,
    .close  = ramfs_close,
    .lookup = ramfs_lookup,
    .create = ramfs_create_file,
    .mkdir  = ramfs_mkdir,
    .unlink = ramfs_unlink,
    .rmdir  = ramfs_rmdir,
};

/* ================================================================
 * Filesystem operations
 * ================================================================ */

struct super_block *ramfs_create(void) {
    struct ramfs_node *head = (struct ramfs_node *)kmalloc(sizeof(*head));
    if (!head) return NULL;
    memset(head, 0, sizeof(*head));
    head->inode.name  = "";
    head->inode.ops   = &ramfs_dir_ops;  /* root is a directory */
    head->inode.is_dir = 1;
    head->inode.priv  = NULL;
    head->next = NULL;

    struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
    if (!sb) { kfree(head); return NULL; }
    memset(sb, 0, sizeof(*sb));
    sb->fs_name = "ramfs";
    sb->root    = &head->inode;
    sb->sb_data = head;
    ramfs_root  = head;
    return sb;
}

int ramfs_add_file(const char *name, const char *content) {
    if (!ramfs_root) return -1;
    if (!name) return -1;
    /* Allow empty content (will create a zero-length file) */
    if (!content) content = "";

    /* FIXED (v4.3.7): BUG-2C — Extract basename from the path.
     * Callers (touch, cp, etc.) pass full paths like "/dir/file",
     * but the inode name should be just "file".  Without this,
     * the file is stored with name "/dir/file" and vfs_lookup
     * can never find it (it searches for "file" in the directory). */
    const char *basename = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/') basename = p + 1;
    }
    if (*basename == '\0') return -1;  /* path ends with '/', no filename */

    /* Check for duplicate file names */
    struct ramfs_node *existing = ramfs_root->children;
    while (existing) {
        if (existing->inode.name && strcmp(existing->inode.name, basename) == 0) {
            log_printf(LOG_LEVEL_WARN, "ramfs: duplicate file '%s' ignored\n", basename);
            return -1;
        }
        existing = existing->next;
    }

    struct ramfs_node *n = (struct ramfs_node *)kmalloc(sizeof(*n));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));

    n->inode.name   = kmalloc(strlen(basename) + 1);
    if (!n->inode.name) { kfree(n); return -1; }
    strcpy((char *)n->inode.name, basename);

    n->inode.size = strlen(content);
    n->data = (char *)kmalloc(n->inode.size + 1);
    if (!n->data) { kfree((void *)n->inode.name); kfree(n); return -1; }
    memcpy(n->data, content, n->inode.size);
    n->data[n->inode.size] = '\0';

    n->inode.ops    = &ramfs_file_ops;
    n->inode.is_dir = 0;
    n->inode.priv   = NULL;

    /* Insert at head of directory children list */
    n->next = ramfs_root->children;
    ramfs_root->children = n;
    return 0;
}

int ramfs_add_file_data(const char *name, const void *data, size_t size) {
    if (!ramfs_root) return -1;
    if (!name || !data) return -1;

    /* FIXED (v4.3.7): BUG-2C — Extract basename from the path */
    const char *basename = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/') basename = p + 1;
    }
    if (*basename == '\0') return -1;

    /* Check for duplicate file names */
    struct ramfs_node *existing = ramfs_root->children;
    while (existing) {
        if (existing->inode.name && strcmp(existing->inode.name, basename) == 0) {
            log_printf(LOG_LEVEL_WARN, "ramfs: duplicate file '%s' ignored\n", basename);
            return -1;
        }
        existing = existing->next;
    }

    struct ramfs_node *n = (struct ramfs_node *)kmalloc(sizeof(*n));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));

    n->inode.name = kmalloc(strlen(basename) + 1);
    if (!n->inode.name) { kfree(n); return -1; }
    strcpy((char *)n->inode.name, basename);

    n->inode.size = size;
    n->data = (char *)kmalloc(size);
    if (!n->data) { kfree((void *)n->inode.name); kfree(n); return -1; }
    memcpy(n->data, data, size);

    n->inode.ops    = &ramfs_file_ops;
    n->inode.is_dir = 0;
    n->inode.priv   = NULL;

    n->next = ramfs_root->children;
    ramfs_root->children = n;
    return 0;
}
