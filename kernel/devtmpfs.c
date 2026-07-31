/*
 * devtmpfs.c - /dev virtual filesystem implementation
 *
 * Inspired by CoolPotOS's devtmpfs. Provides a simple device
 * filesystem that auto-creates device nodes in /dev.
 *
 * Each device file is backed by a function that handles read/write
 * operations. Uses the existing VFS infrastructure.
 */

#include "devtmpfs.h"
#include "fs.h"
#include "vfs.h"
#include "mem.h"
#include "console.h"
#include "include/log.h"
#include "include/errno.h"
#include <string.h>
#include <stdint.h>

/* ================================================================
 * Device entry types
 * ================================================================ */
#define DEV_TYPE_NULL    0
#define DEV_TYPE_ZERO    1
#define DEV_TYPE_CONSOLE 2
#define DEV_TYPE_TTY     3
#define DEV_TYPE_RANDOM  4
#define DEV_TYPE_URANDOM 5
#define DEV_TYPE_STDIN   6   /* POSIX (v4.2.6) */
#define DEV_TYPE_STDOUT  7   /* POSIX (v4.2.6) */
#define DEV_TYPE_STDERR  8   /* POSIX (v4.2.6) */
#define DEV_TYPE_USB_KBD  9   /* FIXED (v4.3.2): USB-001 */
#define DEV_TYPE_USB_MOUSE 10 /* FIXED (v4.3.2): USB-001 */

struct dev_entry {
    const char *name;
    int         type;
};

/* FIXED (v4.2.7): BUG-DEVTMPFS-CACHE - Simple inode cache to avoid
 * allocating a new inode on every lookup.  Without this, every open()
 * or stat() on a device node leaks kmalloc'd inodes and inode_data. */
#define DEV_CACHE_SIZE 16
static struct inode *dev_inode_cache[DEV_CACHE_SIZE];

/* Simple hash function for device name */
static int dev_cache_hash(const char *name) {
    unsigned int h = 0;
    while (*name) {
        h = (h * 31) + (unsigned char)(*name++);
    }
    return (int)(h % DEV_CACHE_SIZE);
}

/* ================================================================
 * Device inode private data
 * ================================================================ */
/* FIXED (v4.3.8): USB-002 — Add subdirectory support for /dev/usb/.
 * The 'subdir' field identifies the USB subdirectory so that lookups
 * under /dev/usb/ can find kbd0/mouse0 device nodes. */
struct devtmpfs_inode_data {
    int type;  /* DEV_TYPE_* */
    int subdir; /* 0=normal device, 1=/dev/usb subdirectory */
};

/* ================================================================
 * Device table
 * ================================================================ */
static struct dev_entry dev_entries[] = {
    { "null",    DEV_TYPE_NULL    },
    { "zero",    DEV_TYPE_ZERO    },
    { "console", DEV_TYPE_CONSOLE },
    { "tty",     DEV_TYPE_TTY     },
    { "random",  DEV_TYPE_RANDOM  },
    { "urandom", DEV_TYPE_URANDOM },
    /* POSIX (v4.2.6): stdio symbolic devices */
    { "stdin",   DEV_TYPE_STDIN   },
    { "stdout",  DEV_TYPE_STDOUT  },
    { "stderr",  DEV_TYPE_STDERR  },
    /* FIXED (v4.3.2): USB-001 — USB HID device nodes */
    { "kbd0",   DEV_TYPE_USB_KBD   },
    { "mouse0", DEV_TYPE_USB_MOUSE },
    { NULL,      0                },  /* sentinel */
};

/* ================================================================
 * Forward declarations
 * ================================================================ */
static struct file_ops devtmpfs_file_ops;
static struct file_ops devtmpfs_dir_ops;
static int devtmpfs_lookup(struct inode *dir, struct dentry *dentry);

/* ================================================================
 * Device read/write handlers
 * ================================================================ */

/*
 * dev_null_read: Always returns EOF (0 bytes).
 */
static ssize_t dev_null_read(struct file *filp, void *buf, size_t count,
                             off_t *offset) {
    (void)filp; (void)buf; (void)count; (void)offset;
    return 0;  /* EOF */
}

/*
 * dev_null_write: Discards all data (returns count as if written).
 */
static ssize_t dev_null_write(struct file *filp, const void *buf, size_t count,
                              off_t *offset) {
    (void)filp; (void)buf; (void)offset;
    return (ssize_t)count;  /* Silently discard */
}

/*
 * dev_zero_read: Returns zero-filled buffer.
 */
static ssize_t dev_zero_read(struct file *filp, void *buf, size_t count,
                             off_t *offset) {
    (void)filp; (void)offset;
    if (!buf || count == 0) return 0;
    memset(buf, 0, count);
    return (ssize_t)count;
}

/*
 * dev_zero_write: Discards data (like /dev/null).
 */
static ssize_t dev_zero_write(struct file *filp, const void *buf, size_t count,
                              off_t *offset) {
    (void)filp; (void)buf; (void)offset;
    return (ssize_t)count;
}

/*
 * dev_console_read: Read from console input (blocking line read).
 */
static ssize_t dev_console_read(struct file *filp, void *buf, size_t count,
                                off_t *offset) {
    (void)filp; (void)offset;
    if (!buf || count == 0) return 0;

    /* Block until a line is available from the console */
    int len = console_getline((char *)buf, count);
    if (len <= 0) return 0;
    return (ssize_t)len;
}

/*
 * dev_console_write: Write to the console.
 */
static ssize_t dev_console_write(struct file *filp, const void *buf, size_t count,
                                 off_t *offset) {
    (void)filp; (void)offset;
    if (!buf || count == 0) return 0;

    const char *cbuf = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        console_putc(cbuf[i]);
    }
    return (ssize_t)count;
}

/*
 * rdrand32: Get a 32-bit random value using RDRAND instruction.
 * Returns 1 on success, 0 if RDRAND is not available or failed.
 *
 * FIXED (v4.1.4): Added retry limit to prevent infinite loop on CPUs
 * without RDRAND support.  RDRAND can fail transiently when the hardware
 * RNG is exhausted, but on CPUs without RDRAND, CF is always 0 and the
 * jnc instruction loops forever.  Now retries up to 10 times.  (BUG-006 / 2.4)
 */
static int rdrand32(uint32_t *val) {
    uint8_t ok_byte = 0;
    int retries = 0;
    asm volatile (
        "1:\n\t"
        "rdrand %0\n\t"
        "setc %1\n\t"
        "testb %1, %1\n\t"
        "jnz 2f\n\t"
        "incl %2\n\t"
        "cmpl $10, %2\n\t"
        "jl 1b\n\t"
        "2:\n\t"
        : "=r"(*val), "=qm"(ok_byte), "+r"(retries)
        :
        : "cc"
    );
    return ok_byte;
}

/*
 * dev_random_read: Read random bytes using RDRAND.
 * Blocks until enough random bytes are available (for /dev/random).
 */
static ssize_t dev_random_read(struct file *filp, void *buf, size_t count,
                                off_t *offset, int blocking) {
    (void)filp; (void)offset;
    if (!buf || count == 0) return 0;

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = count;

    while (remaining > 0) {
        uint32_t rand_val;
        if (!rdrand32(&rand_val)) {
            if (blocking) {
                /* For /dev/random, retry on failure */
                continue;
            }
            /* For /dev/urandom, stop on failure */
            break;
        }
        size_t copy = (remaining < 4) ? remaining : 4;
        memcpy(dst, &rand_val, copy);
        dst += copy;
        remaining -= copy;
    }

    return (ssize_t)(count - remaining);
}

/*
 * dev_random_write: Discard data (like /dev/null).
 */
static ssize_t dev_random_write(struct file *filp, const void *buf, size_t count,
                                off_t *offset) {
    (void)filp; (void)buf; (void)offset;
    return (ssize_t)count;
}

/* FIXED (v4.3.2): USB-001 — USB HID device handlers.
 * These are stubs that will be connected to the actual USB HID driver
 * once the USB subsystem is fully initialized. */
static ssize_t dev_usb_kbd_read(struct file *filp, void *buf, size_t count,
                                off_t *offset) {
    (void)filp; (void)buf; (void)count; (void)offset;
    return 0;  /* No data available yet — USB HID driver not connected */
}

static ssize_t dev_usb_kbd_write(struct file *filp, const void *buf, size_t count,
                                 off_t *offset) {
    (void)filp; (void)buf; (void)offset;
    return (ssize_t)count;  /* Discard writes (keyboard is input-only) */
}

static ssize_t dev_usb_mouse_read(struct file *filp, void *buf, size_t count,
                                  off_t *offset) {
    (void)filp; (void)buf; (void)count; (void)offset;
    return 0;  /* No data available yet — USB HID driver not connected */
}

static ssize_t dev_usb_mouse_write(struct file *filp, const void *buf, size_t count,
                                   off_t *offset) {
    (void)filp; (void)buf; (void)offset;
    return (ssize_t)count;  /* Discard writes (mouse is input-only) */
}

/* ================================================================
 * Devtmpfs file operations
 * ================================================================ */

static int devtmpfs_open(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp;
    return 0;
}

static ssize_t devtmpfs_read(struct file *filp, void *buf, size_t count,
                             off_t *offset) {
    if (!filp || !filp->inode || !buf) return -EINVAL;

    struct devtmpfs_inode_data *data =
        (struct devtmpfs_inode_data *)filp->inode->priv;
    if (!data) return -EINVAL;

    switch (data->type) {
        case DEV_TYPE_NULL:    return dev_null_read(filp, buf, count, offset);
        case DEV_TYPE_ZERO:    return dev_zero_read(filp, buf, count, offset);
        case DEV_TYPE_CONSOLE: return dev_console_read(filp, buf, count, offset);
        case DEV_TYPE_TTY:     return dev_console_read(filp, buf, count, offset);
        case DEV_TYPE_RANDOM:  return dev_random_read(filp, buf, count, offset, 1);
        case DEV_TYPE_URANDOM: return dev_random_read(filp, buf, count, offset, 0);
        /* POSIX (v4.2.6): stdin maps to fd 0 (console input) */
        case DEV_TYPE_STDIN:   return dev_console_read(filp, buf, count, offset);
        /* POSIX (v4.2.6): stdout/stderr return EOF on read (output-only) */
        case DEV_TYPE_STDOUT:
        case DEV_TYPE_STDERR:  return 0;  /* EOF on read */
        /* FIXED (v4.3.2): USB-001 — USB HID device nodes */
        case DEV_TYPE_USB_KBD:   return dev_usb_kbd_read(filp, buf, count, offset);
        case DEV_TYPE_USB_MOUSE: return dev_usb_mouse_read(filp, buf, count, offset);
        default:               return -ENXIO;
    }
}

static ssize_t devtmpfs_write(struct file *filp, const void *buf, size_t count,
                              off_t *offset) {
    if (!filp || !filp->inode || !buf) return -EINVAL;

    struct devtmpfs_inode_data *data =
        (struct devtmpfs_inode_data *)filp->inode->priv;
    if (!data) return -EINVAL;

    switch (data->type) {
        case DEV_TYPE_NULL:    return dev_null_write(filp, buf, count, offset);
        case DEV_TYPE_ZERO:    return dev_zero_write(filp, buf, count, offset);
        case DEV_TYPE_CONSOLE: return dev_console_write(filp, buf, count, offset);
        case DEV_TYPE_TTY:     return dev_console_write(filp, buf, count, offset);
        case DEV_TYPE_RANDOM:  return dev_random_write(filp, buf, count, offset);
        case DEV_TYPE_URANDOM: return dev_random_write(filp, buf, count, offset);
        /* POSIX (v4.2.6): stdin discards writes (input-only) */
        case DEV_TYPE_STDIN:   return dev_null_write(filp, buf, count, offset);
        /* POSIX (v4.2.6): stdout/stderr write to console */
        case DEV_TYPE_STDOUT:
        case DEV_TYPE_STDERR:  return dev_console_write(filp, buf, count, offset);
        /* FIXED (v4.3.2): USB-001 — USB HID device nodes */
        case DEV_TYPE_USB_KBD:   return dev_usb_kbd_write(filp, buf, count, offset);
        case DEV_TYPE_USB_MOUSE: return dev_usb_mouse_write(filp, buf, count, offset);
        default:               return -ENXIO;
    }
}

static int devtmpfs_close(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp;
    return 0;
}

/* ================================================================
 * File operations tables
 * ================================================================ */

static struct file_ops devtmpfs_file_ops = {
    .open   = devtmpfs_open,
    .read   = devtmpfs_read,
    .write  = devtmpfs_write,
    .close  = devtmpfs_close,
    .lookup = NULL,
};

static struct file_ops devtmpfs_dir_ops = {
    .open   = devtmpfs_open,
    .read   = NULL,
    .write  = NULL,
    .close  = devtmpfs_close,
    .lookup = devtmpfs_lookup,
};

/* ================================================================
 * lookup: Resolve a name within the devtmpfs root directory
 * ================================================================ */

static int devtmpfs_lookup(struct inode *dir, struct dentry *dentry) {
    if (!dir || !dentry || !dentry->name) return -EINVAL;

    /* FIXED (v4.3.8): USB-002 — Check if the parent is a devtmpfs subdirectory.
     * If dir->priv->subdir is set, we are inside /dev/usb/ and should
     * look up the device name (kbd0, mouse0) in the device table. */
    struct devtmpfs_inode_data *dir_data =
        (struct devtmpfs_inode_data *)dir->priv;
    int is_subdir = (dir_data && dir_data->subdir);

    /* FIXED (v4.2.7): BUG-DEVTMPFS-CACHE - Check the inode cache
     * before allocating a new one.  Without this, every lookup would
     * kmalloc a new inode and leak the previous one. */
    int hash = dev_cache_hash(dentry->name);
    if (dev_inode_cache[hash] && dev_inode_cache[hash]->name &&
        strcmp(dev_inode_cache[hash]->name, dentry->name) == 0) {
        dev_inode_cache[hash]->dentry = dentry;
        dentry->inode = dev_inode_cache[hash];
        return 0;
    }

    /* FIXED (v4.3.8): USB-002 — Handle /dev/usb directory lookup.
     * When looking up "usb" in the root, create a directory inode. */
    if (!is_subdir && strcmp(dentry->name, "usb") == 0) {
        struct inode *inode = (struct inode *)kmalloc(sizeof(*inode));
        if (!inode) return -ENOMEM;
        memset(inode, 0, sizeof(*inode));

        struct devtmpfs_inode_data *data =
            (struct devtmpfs_inode_data *)kmalloc(sizeof(*data));
        if (!data) { kfree(inode); return -ENOMEM; }
        memset(data, 0, sizeof(*data));

        data->type = -1;
        data->subdir = 1;  /* Mark as /dev/usb subdirectory */

        inode->name = "usb";
        inode->priv = data;
        inode->is_dir = 1;
        inode->ops = &devtmpfs_dir_ops;
        inode->dentry = dentry;
        dentry->inode = inode;

        dev_inode_cache[hash] = inode;
        return 0;
    }

    /* Search the device entry table */
    for (int i = 0; dev_entries[i].name != NULL; i++) {
        struct dev_entry *e = &dev_entries[i];
        if (strcmp(e->name, dentry->name) == 0) {
            /* FIXED (v4.3.8): USB-002 — Only match USB devices
             * (kbd0, mouse0) when looking up inside the /dev/usb/
             * subdirectory.  Non-USB devices are only visible at
             * the /dev/ root level. */
            if (is_subdir) {
                if (e->type != DEV_TYPE_USB_KBD &&
                    e->type != DEV_TYPE_USB_MOUSE) {
                    continue;  /* Skip non-USB devices in /dev/usb/ */
                }
            } else {
                if (e->type == DEV_TYPE_USB_KBD ||
                    e->type == DEV_TYPE_USB_MOUSE) {
                    continue;  /* Skip USB devices at /dev/ root */
                }
            }

            /* Create an inode for this device */
            struct inode *inode = (struct inode *)kmalloc(sizeof(*inode));
            if (!inode) return -ENOMEM;
            memset(inode, 0, sizeof(*inode));

            struct devtmpfs_inode_data *data =
                (struct devtmpfs_inode_data *)kmalloc(sizeof(*data));
            if (!data) { kfree(inode); return -ENOMEM; }
            memset(data, 0, sizeof(*data));

            data->type = e->type;
            data->subdir = 0;

            inode->name = e->name;
            inode->priv = data;
            inode->is_dir = 0;
            inode->ops = &devtmpfs_file_ops;
            inode->dentry = dentry;
            dentry->inode = inode;

            /* FIXED (v4.2.7): BUG-DEVTMPFS-CACHE - Cache the inode
             * for future lookups to avoid repeated allocations. */
            dev_inode_cache[hash] = inode;
            return 0;
        }
    }

    return -ENOENT;  /* Not found */
}

/* ================================================================
 * devtmpfs_create: Create the devtmpfs filesystem super block
 * ================================================================ */

struct super_block *devtmpfs_create(void) {
    /* Create the root inode for devtmpfs */
    struct inode *root_inode = (struct inode *)kmalloc(sizeof(*root_inode));
    if (!root_inode) return NULL;
    memset(root_inode, 0, sizeof(*root_inode));

    struct devtmpfs_inode_data *root_data =
        (struct devtmpfs_inode_data *)kmalloc(sizeof(*root_data));
    if (!root_data) { kfree(root_inode); return NULL; }
    memset(root_data, 0, sizeof(*root_data));

    root_data->type = -1;  /* Root directory, not a device */

    root_inode->name = "";
    root_inode->priv = root_data;
    root_inode->is_dir = 1;
    root_inode->ops = &devtmpfs_dir_ops;

    /* Create the super block */
    struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
    if (!sb) { kfree(root_data); kfree(root_inode); return NULL; }
    memset(sb, 0, sizeof(*sb));

    sb->fs_name = "devtmpfs";
    sb->root = root_inode;
    sb->sb_data = NULL;

    return sb;
}

/* ================================================================
 * FIXED (v4.3.2): USB-001 — Create /dev/usb device nodes.
 * FIXED (v4.3.8): USB-002 — Device nodes are now at /dev/usb/kbd0
 * and /dev/usb/mouse0 (subdirectory), not flat at /dev/usb_kbd0.
 * The /dev/usb/ directory is created and the lookup function handles
 * the subdirectory lookup transparently.
 * ================================================================ */
void devtmpfs_create_usb_nodes(void) {
    /* Create /dev/usb directory */
    vfs_mkdir("/dev/usb");

    log_printf(LOG_LEVEL_INFO, "devtmpfs: /dev/usb/kbd0 and /dev/usb/mouse0 device nodes created\n");
}

/* ================================================================
 * devtmpfs_init: Create devtmpfs and mount it at /dev
 * ================================================================ */

void devtmpfs_init(void) {
    struct super_block *dev_sb = devtmpfs_create();
    if (!dev_sb) {
        log_printf(LOG_LEVEL_ERR, "devtmpfs: failed to create super block\n");
        return;
    }

    if (vfs_mount("/dev", dev_sb) < 0) {
        log_printf(LOG_LEVEL_ERR, "devtmpfs: failed to mount at /dev\n");
        kfree(dev_sb);
        return;
    }

    log_printf(LOG_LEVEL_INFO, "devtmpfs: mounted at /dev\n");

    /* FIXED (v4.3.2): USB-001 — Create USB device nodes after mount */
    devtmpfs_create_usb_nodes();
}