/*
 * sysfs.c - Kernel object information filesystem
 *
 * Provides a virtual filesystem mounted at /sys that exposes kernel
 * information to userspace. Each entry is backed by a callback function
 * that generates content on read.
 *
 * Layout:
 *   /sys/kernel/version  - kernel version string
 *   /sys/kernel/ostype   - OS type name
 */

#include "fs.h"
#include "vfs.h"
#include "include/version.h"
#include "include/log.h"
#include "mem.h"
#include <string.h>

/* Forward declarations for file_ops tables (referenced before definition) */
static struct file_ops sysfs_file_ops;
static struct file_ops sysfs_dir_ops;

/* ================================================================
 * sysfs entry types
 * ================================================================ */
typedef int (*sysfs_read_fn)(char *buf, size_t size);

struct sysfs_entry {
    const char    *name;
    int            is_dir;
    sysfs_read_fn  read_fn;
    struct sysfs_entry *children;  /* linked list */
    struct sysfs_entry *next;
    struct inode   *cached_inode;  /* cached inode to avoid re-allocation */
};

/* ================================================================
 * Inode private data for sysfs
 * ================================================================ */
struct sysfs_inode_data {
    struct sysfs_entry *entry;
};

/* ================================================================
 * File operations
 * ================================================================ */

static int sysfs_read(struct file *filp, void *buf, size_t count, off_t *offset) {
    struct sysfs_inode_data *d = (struct sysfs_inode_data *)filp->inode->priv;
    if (!d || !d->entry || !d->entry->read_fn) return -1;

    /* Use a stack buffer for small sysfs entries */
    char tmp[512];
    int len = d->entry->read_fn(tmp, sizeof(tmp));
    if (len < 0) return -1;

    if (*offset >= (off_t)len) return 0;
    size_t toread = count;
    if ((size_t)(*offset) + toread > (size_t)len)
        toread = (size_t)len - (size_t)(*offset);
    {
        /* FIXED (v4.2.7): BUG-SYSFS-SMAP - stac/clac must always be
         * paired.  Previously stac() was unconditional but clac() was
         * conditional on the saved RFLAGS.AC bit, which could invert
         * the AC bit state if interrupts or context switches occurred
         * between the pushfq and the condition check.  Now both are
         * unconditional since sysfs_read always copies to user space. */
        asm volatile ("stac" ::: "memory");
        memcpy(buf, tmp + (*offset), toread);
        asm volatile ("clac" ::: "memory");
    }
    *offset += (off_t)toread;
    return (ssize_t)toread;
}

static ssize_t sysfs_write(struct file *filp, const void *buf, size_t count,
                           off_t *offset) {
    (void)filp; (void)buf; (void)count; (void)offset;
    return -1;  /* sysfs is read-only */
}

static int sysfs_open(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp; return 0;
}

static int sysfs_close(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp; return 0;
}

static int sysfs_lookup(struct inode *dir, struct dentry *dentry) {
    struct sysfs_inode_data *d = (struct sysfs_inode_data *)dir->priv;
    if (!d || !d->entry) return -1;

    struct sysfs_entry *child = d->entry->children;
    while (child) {
        if (child->name && strcmp(child->name, dentry->name) == 0) {
            /* Return cached inode if available to avoid memory leak */
            if (child->cached_inode) {
                child->cached_inode->dentry = dentry;
                dentry->inode = child->cached_inode;
                return 0;
            }

            /* Create inode for this entry on the fly */
            struct inode *inode = (struct inode *)kmalloc(sizeof(*inode));
            if (!inode) return -1;
            memset(inode, 0, sizeof(*inode));

            struct sysfs_inode_data *cd =
                (struct sysfs_inode_data *)kmalloc(sizeof(*cd));
            if (!cd) { kfree(inode); return -1; }
            memset(cd, 0, sizeof(*cd));

            cd->entry = child;

            inode->name   = child->name;
            inode->priv   = cd;
            inode->is_dir = child->is_dir;
            inode->ops    = child->is_dir ? &sysfs_dir_ops : &sysfs_file_ops;
            inode->dentry = dentry;

            child->cached_inode = inode;
            dentry->inode = inode;
            return 0;
        }
        child = child->next;
    }
    return -1;
}

static struct file_ops sysfs_file_ops = {
    .open   = sysfs_open,
    .read   = sysfs_read,
    .write  = sysfs_write,
    .close  = sysfs_close,
    .lookup = NULL,
};

static struct file_ops sysfs_dir_ops = {
    .open   = sysfs_open,
    .read   = NULL,
    .write  = NULL,
    .close  = sysfs_close,
    .lookup = sysfs_lookup,
};

/* ================================================================
 * Callback functions for sysfs entries
 * ================================================================ */

static int read_version(char *buf, size_t size) {
    (void)size;
    const char *s = AURORAOS_VERSION;
    size_t len = 0;
    for (const char *p = s; *p; p++) len++;
    if (len >= size) len = size - 1;
    memcpy(buf, s, len);
    buf[len] = '\n';
    return (int)(len + 1);
}

/* FIXED (v4.2.7): BUG-OSTYPE-OVERFLOW — When size==0, the old code
 * computed len = size - 1 = (size_t)-1, causing a huge memcpy that
 * smashed the kernel stack.  Now we cap len at size and handle the
 * zero-size case gracefully. */
static int read_ostype(char *buf, size_t size) {
    const char *s = "AuroraOS\n";
    size_t len = 0;
    for (const char *p = s; *p; p++) len++;
    if (len > size) len = size;
    if (len > 0) memcpy(buf, s, len);
    return (int)len;
}

/* POSIX (v4.2.6): Hostname reader */
static int read_hostname(char *buf, size_t size) {
    (void)size;
    const char *s = "aurora\n";
    size_t len = 0;
    for (const char *p = s; *p; p++) len++;
    if (len >= size) len = size - 1;
    memcpy(buf, s, len);
    return (int)len;
}

/* POSIX (v4.2.6): CPU online reader */
static int read_cpu_online(char *buf, size_t size) {
    (void)size;
    const char *s = "0\n";
    size_t len = 0;
    for (const char *p = s; *p; p++) len++;
    if (len >= size) len = size - 1;
    memcpy(buf, s, len);
    return (int)len;
}

/* POSIX (v4.2.6): CPU possible reader */
static int read_cpu_possible(char *buf, size_t size) {
    (void)size;
    const char *s = "0-3\n";
    size_t len = 0;
    for (const char *p = s; *p; p++) len++;
    if (len >= size) len = size - 1;
    memcpy(buf, s, len);
    return (int)len;
}

/* ================================================================
 * sysfs entry tree definition
 * ================================================================ */

/* POSIX (v4.2.6): CPU devices children */
static struct sysfs_entry cpu_children[] = {
    { "online",   0, read_cpu_online,   NULL, NULL },
    { "possible", 0, read_cpu_possible, NULL, NULL },
    { NULL, 0, NULL, NULL, NULL }
};

static struct sysfs_entry kernel_children[] = {
    { "version",  0, read_version,  NULL, NULL },
    { "ostype",   0, read_ostype,   NULL, NULL },
    { "hostname", 0, read_hostname, NULL, NULL },  /* POSIX (v4.2.6) */
    { NULL, 0, NULL, NULL, NULL }
};

static struct sysfs_entry devices_children[] = {
    { "cpu", 1, NULL, cpu_children, NULL },  /* POSIX (v4.2.6) */
    { NULL, 0, NULL, NULL, NULL }
};

static struct sysfs_entry root_children[] = {
    { "kernel",  1, NULL, kernel_children,   NULL },
    { "devices", 1, NULL, devices_children,  NULL },  /* POSIX (v4.2.6) */
    { NULL, 0, NULL, NULL, NULL }
};

static struct sysfs_entry sysfs_root_entry = {
    "", 1, NULL, root_children, NULL
};

/* Link the children lists */
static void sysfs_link_children(void) {
    for (int j = 0; root_children[j].name; j++) {
        if (j + 1 < (int)(sizeof(root_children) / sizeof(root_children[0])) &&
            root_children[j + 1].name)
            root_children[j].next = &root_children[j + 1];
    }
    for (int j = 0; kernel_children[j].name; j++) {
        if (j + 1 < (int)(sizeof(kernel_children) / sizeof(kernel_children[0])) &&
            kernel_children[j + 1].name)
            kernel_children[j].next = &kernel_children[j + 1];
    }
    /* POSIX (v4.2.6): Link new children arrays */
    for (int j = 0; devices_children[j].name; j++) {
        if (j + 1 < (int)(sizeof(devices_children) / sizeof(devices_children[0])) &&
            devices_children[j + 1].name)
            devices_children[j].next = &devices_children[j + 1];
    }
    for (int j = 0; cpu_children[j].name; j++) {
        if (j + 1 < (int)(sizeof(cpu_children) / sizeof(cpu_children[0])) &&
            cpu_children[j + 1].name)
            cpu_children[j].next = &cpu_children[j + 1];
    }
}

/* ================================================================
 * sysfs_create: Create the sysfs filesystem super block
 * ================================================================ */

struct super_block *sysfs_create(void) {
    sysfs_link_children();

    struct inode *root_inode = (struct inode *)kmalloc(sizeof(*root_inode));
    if (!root_inode) return NULL;
    memset(root_inode, 0, sizeof(*root_inode));

    struct sysfs_inode_data *root_data =
        (struct sysfs_inode_data *)kmalloc(sizeof(*root_data));
    if (!root_data) { kfree(root_inode); return NULL; }
    memset(root_data, 0, sizeof(*root_data));

    root_data->entry = &sysfs_root_entry;

    root_inode->name   = "";
    root_inode->priv   = root_data;
    root_inode->is_dir = 1;
    root_inode->ops    = &sysfs_dir_ops;

    struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
    if (!sb) { kfree(root_data); kfree(root_inode); return NULL; }
    memset(sb, 0, sizeof(*sb));

    sb->fs_name = "sysfs";
    sb->root    = root_inode;
    sb->sb_data = NULL;

    return sb;
}

/* ================================================================
 * sysfs_init: Create sysfs and mount it at /sys
 * ================================================================ */

void sysfs_init(void) {
    struct super_block *sys_sb = sysfs_create();
    if (!sys_sb) {
        log_printf(LOG_LEVEL_ERR, "sysfs: failed to create super block\n");
        return;
    }

    if (vfs_mount("/sys", sys_sb) < 0) {
        log_printf(LOG_LEVEL_ERR, "sysfs: failed to mount at /sys\n");
        return;
    }

    log_printf(LOG_LEVEL_INFO, "sysfs: mounted at /sys\n");
}