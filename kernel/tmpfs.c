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
 *
 * FIXED (v4.3.8): TMPFS-002 — Use page cache (alloc_page) instead of
 * kmalloc for file data.  Each file's data is stored in an array of
 * page pointers, allowing sparse files, swap awareness, and mmap
 * compatibility with the kernel page cache.
 * ================================================================ */
struct tmpfs_inode {
    struct inode inode;  /* FIXED (v4.4.2): BUILD-11 — add inode member */
    uint32_t ino;
    uint32_t mode;
    uint32_t uid, gid;
    uint64_t atime, mtime, ctime;
    uint32_t nlink;  /* FIXED (v4.3.3): TMPFS-001 — proper nlink tracking */
    void    **pages;  /* FIXED (v4.3.8): TMPFS-002 — page array instead of kmalloc */
    uint32_t num_pages; /* number of allocated pages */
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

    /* FIXED (v4.3.8): TMPFS-002 — Read from page cache instead of kmalloc buffer.
     * Compute page index and offset, copy from each page in the chain. */
    size_t toread = count;
    if ((size_t)(*offset) + toread > n->inode.size)
        toread = n->inode.size - (size_t)(*offset);

    size_t total = 0;
    char *dst = (char *)buf;
    off_t cur_off = *offset;

    while (total < toread) {
        uint32_t page_idx = (uint32_t)(cur_off / PAGE_SIZE);
        uint32_t page_off = (uint32_t)(cur_off % PAGE_SIZE);
        size_t chunk = toread - total;
        if (page_off + chunk > PAGE_SIZE) chunk = PAGE_SIZE - page_off;

        if (page_idx >= n->num_pages || !n->pages || !n->pages[page_idx]) {
            /* Sparse region: read zeros */
            memset(dst + total, 0, chunk);
        } else {
            memcpy(dst + total, (char *)n->pages[page_idx] + page_off, chunk);
        }
        total += chunk;
        cur_off += (off_t)chunk;
    }

    *offset += (off_t)total;
    spin_unlock(&tmpfs_lock);
    return (ssize_t)total;
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

    /* FIXED (v4.3.8): TMPFS-002 — Use alloc_page for page cache.
     * Allocate pages as needed for the write range.  Each page is
     * independently allocated and freed, supporting sparse files
     * and swap awareness. */
    uint32_t needed_pages = (uint32_t)((new_size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (needed_pages > n->num_pages) {
        /* Grow the page pointer array */
        void **new_pages = (void **)kmalloc(needed_pages * sizeof(void *));
        if (!new_pages) { spin_unlock(&tmpfs_lock); return -1; }
        if (n->pages) {
            memcpy(new_pages, n->pages, n->num_pages * sizeof(void *));
            kfree(n->pages);
        }
        /* Initialize new slots to NULL */
        for (uint32_t i = n->num_pages; i < needed_pages; i++) {
            new_pages[i] = NULL;
        }
        n->pages = new_pages;
        n->num_pages = needed_pages;
    }

    /* Allocate pages for the write range */
    uint32_t start_page = (uint32_t)((size_t)(*offset) / PAGE_SIZE);
    uint32_t end_page = (uint32_t)((new_size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (end_page > n->num_pages) end_page = n->num_pages;
    for (uint32_t pi = start_page; pi < end_page; pi++) {
        if (!n->pages[pi]) {
            n->pages[pi] = alloc_page();
            if (!n->pages[pi]) {
                spin_unlock(&tmpfs_lock);
                return -1;
            }
        }
    }

    /* Write data page by page */
    size_t total = 0;
    const char *src = (const char *)buf;
    off_t cur_off = *offset;

    while (total < count) {
        uint32_t page_idx = (uint32_t)(cur_off / PAGE_SIZE);
        uint32_t page_off = (uint32_t)(cur_off % PAGE_SIZE);
        size_t chunk = count - total;
        if (page_off + chunk > PAGE_SIZE) chunk = PAGE_SIZE - page_off;

        if (page_idx < n->num_pages && n->pages[page_idx]) {
            memcpy((char *)n->pages[page_idx] + page_off, src + total, chunk);
        }
        total += chunk;
        cur_off += (off_t)chunk;
    }

    if (new_size > n->inode.size)
        n->inode.size = new_size;
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
    n->pages = NULL;      /* FIXED (v4.3.8): TMPFS-002 — page cache */
    n->num_pages = 0;     /* FIXED (v4.3.8): TMPFS-002 */
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
    n->pages = NULL;      /* FIXED (v4.3.8): TMPFS-002 */
    n->num_pages = 0;     /* FIXED (v4.3.8): TMPFS-002 */
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
            /* FIXED (v4.3.8): TMPFS-002 — Free all allocated pages */
            if (cur->pages) {
                for (uint32_t i = 0; i < cur->num_pages; i++) {
                    if (cur->pages[i]) free_page(cur->pages[i]);
                }
                kfree(cur->pages);
            }
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
            /* FIXED (v4.3.8): TMPFS-002 — Free pages if any */
            if (cur->pages) {
                for (uint32_t i = 0; i < cur->num_pages; i++) {
                    if (cur->pages[i]) free_page(cur->pages[i]);
                }
                kfree(cur->pages);
            }
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