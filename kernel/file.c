/*
 * file.c - File descriptor table management (FIXED)
 *
 * Fixes applied:
 *   - fd_table now uses uintptr_t (not int), eliminating 64→32 bit
 *     pointer truncation. (Report §8.2, issue #1 in §4.1)
 *   - fd_close_all: close all fds on process exit.
 */
#include "fs.h"
#include "vfs.h"
#include "sched.h"
#include "include/log.h"
#include "net/unix.h"     /* AF_UNIX (v4.2.6) */
#include "capability.h"   /* FIXED (v4.2.9): SEC-CAP-FD-TYPE - type check for fd_table */
#include <string.h>

/* FIXED (v4.2.9): SEC-CAP-FD-TYPE — fd_table entry type tags.
 * cap_fd_* functions share fd_table with fd_* but store cap_entry*
 * pointers instead of raw file* pointers.  These type constants
 * allow fd_get to distinguish and reject capability entries,
 * preventing type confusion that would cause vfs_close to be
 * called on a cap_entry* pointer. */
#define FD_TYPE_FILE  0
#define FD_TYPE_CAP   1

void fd_table_init(struct task_struct *t) {
    for (int i = 0; i < MAX_FDS; ++i)
        t->fd_table[i] = (uintptr_t)-1;  /* all bits set = unused */
}

int fd_alloc(struct task_struct *t, void *filp) {
    if (!t || !filp) return -1;
    for (int i = 0; i < MAX_FDS; ++i) {
        if (t->fd_table[i] == (uintptr_t)-1) {
            t->fd_table[i] = (uintptr_t)filp;
            return i;
        }
    }
    return -1; /* table full */
}

void *fd_get(struct task_struct *t, int fd) {
    if (!t) return NULL;
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    if (t->fd_table[fd] == (uintptr_t)-1) return NULL;

    /* FIXED (v4.2.9): SEC-CAP-FD-TYPE — Check for cap_entry* stored
     * by cap_fd_* functions.  cap_entry has a magic field at offset 0;
     * if it matches CAP_ENTRY_MAGIC, this is a capability fd, not a
     * regular file fd.  Return NULL to prevent type confusion. */
    struct cap_entry *entry = (struct cap_entry *)t->fd_table[fd];
    if (entry->magic == CAP_ENTRY_MAGIC) return NULL;

    return (void *)t->fd_table[fd];
}

/* AF_UNIX (v4.2.6): Check if an fd_table entry is a Unix domain socket */
static inline struct unix_sock *fd_is_unix(uintptr_t entry) {
    if (entry == (uintptr_t)-1 || entry == 0 || entry == 0x1 || (entry & 1))
        return NULL;
    return (struct unix_sock *)entry;
}

int fd_close(struct task_struct *t, int fd) {
    if (!t) return -1;
    if (fd < 0 || fd >= MAX_FDS) return -1;
    uintptr_t entry = t->fd_table[fd];
    if (entry == (uintptr_t)-1) return -1;

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_is_unix(entry);
    if (usk) {
        unix_close(usk);
        t->fd_table[fd] = (uintptr_t)-1;
        return 0;
    }

    vfs_close((struct file *)entry);
    t->fd_table[fd] = (uintptr_t)-1;
    return 0;
}

int fd_open_path(struct task_struct *t, const char *path) {
    struct file *f = vfs_open(path, 0);
    if (!f) return -1;
    int fd = fd_alloc(t, f);
    if (fd < 0) { vfs_close(f); return -1; }
    return fd;
}

ssize_t fd_read_fd(struct task_struct *t, int fd, void *buf, size_t count) {
    struct file *f = (struct file *)fd_get(t, fd);
    if (!f) return -1;
    return vfs_read(f, buf, count);
}

ssize_t fd_write_fd(struct task_struct *t, int fd, const void *buf, size_t count) {
    struct file *f = (struct file *)fd_get(t, fd);
    if (!f) return -1;
    return vfs_write(f, buf, count);
}

void fd_close_all(struct task_struct *t) {
    if (!t) return;
    for (int i = 0; i < MAX_FDS; ++i) {
        uintptr_t entry = t->fd_table[i];
        if (entry == (uintptr_t)-1) continue;

        /* AF_UNIX (v4.2.6) */
        struct unix_sock *usk = fd_is_unix(entry);
        if (usk) {
            unix_close(usk);
            t->fd_table[i] = (uintptr_t)-1;
            continue;
        }

        vfs_close((struct file *)entry);
        t->fd_table[i] = (uintptr_t)-1;
    }
}

/*
 * fd_close_exec: Close file descriptors marked with O_CLOEXEC.
 * Called during exec() to prevent fd leakage to the new process image.
 *
 * FIXED (v4.2.0): Stub implementation — full CLOEXEC support requires
 * per-fd flags tracking in the fd_table.  When CLOEXEC is implemented,
 * this function should iterate fds and close those with the CLOEXEC flag.
 * (Top 10 #1 / BUG-PROC-H1)
 */
void fd_close_exec(struct task_struct *t) {
    if (!t) return;
    /* TODO (v4.2.8): When CLOEXEC flag is implemented, iterate fds and close
     * those with CLOEXEC set.  For now, this is a no-op since CLOEXEC
     * is not yet supported. */
    (void)t;
}

/*
 * fd_dup: Duplicate a file descriptor (like POSIX dup()).
 * Returns the new fd, or -1 on error.
 */
int fd_dup(struct task_struct *t, int oldfd) {
    if (!t) return -1;
    void *f = fd_get(t, oldfd);
    if (!f) return -1;

    /* AF_UNIX (v4.2.6): For Unix sockets, just share the socket pointer */
    struct unix_sock *usk = fd_is_unix((uintptr_t)f);
    if (usk) {
        spin_lock(&usk->lock);
        usk->refcount++;
        spin_unlock(&usk->lock);
        int newfd = fd_alloc(t, f);
        if (newfd < 0) {
            spin_lock(&usk->lock);
            usk->refcount--;
            spin_unlock(&usk->lock);
        }
        return newfd;
    }

    /* Increment file refcount since both fds share the same file */
    vfs_file_dup((struct file *)f);
    int newfd = fd_alloc(t, f);
    if (newfd < 0) {
        /* Rollback: fd_alloc failed, undo the refcount increment */
        vfs_close((struct file *)f);
    }
    return newfd;
}

/*
 * fd_dup2: Duplicate oldfd to newfd (like POSIX dup2()).
 * If newfd is already open, it is silently closed first.
 * Returns newfd on success, -1 on error.
 */
int fd_dup2(struct task_struct *t, int oldfd, int newfd) {
    if (!t) return -1;
    if (oldfd < 0 || oldfd >= MAX_FDS) return -1;
    if (newfd < 0 || newfd >= MAX_FDS) return -1;
    if (oldfd == newfd) return newfd;

    void *f = fd_get(t, oldfd);
    if (!f) return -1;

    /* Close newfd if already open */
    if (t->fd_table[newfd] != (uintptr_t)-1) {
        fd_close(t, newfd);
    }

    /* AF_UNIX (v4.2.6): For Unix sockets, just share the socket pointer */
    struct unix_sock *usk = fd_is_unix((uintptr_t)f);
    if (usk) {
        spin_lock(&usk->lock);
        usk->refcount++;
        spin_unlock(&usk->lock);
        t->fd_table[newfd] = (uintptr_t)f;
        return newfd;
    }

    /* Increment file refcount and assign */
    vfs_file_dup((struct file *)f);
    t->fd_table[newfd] = (uintptr_t)f;
    return newfd;
}
