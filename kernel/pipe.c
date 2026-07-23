/*
 * pipe.c - Anonymous pipe (NEW — Phase 1)
 *
 * A pipe is a pair of file descriptors (read end, write end) backed
 * by a ring buffer in kernel memory. Data written to the write end
 * can be read from the read end in FIFO order.
 *
 * Design:
 *   - Ring buffer: PIPE_BUF_SIZE bytes, circular.
 *   - Blocking read/write: if buffer is empty/full, blocks the task
 *     (TASK_BLOCKED) and yields. Woken by the other end.
 *   - Close detection: read returns 0 when write end is closed and
 *     buffer is empty (EOF).
 *   - Multi-reader/multi-writer: blocked tasks are queued in a linked
 *     list (not a single pointer), so all blocked readers/writers are
 *     woken when data/space becomes available.  (BUG-009 / 2.6)
 *
 * VFS integration: pipefs is a simple filesystem that creates
 * inode pairs for each pipe. The inode's priv points to the
 * pipe_ring structure.
 */
#include "fs.h"
#include "vfs.h"
#include "sched.h"
#include "include/log.h"
#include "include/userspace.h"
#include "include/errno.h"
#include "mem.h"
#include <string.h>
#include <stdint.h>

/* Forward declarations for file_ops tables (referenced before definition) */
static struct file_ops pipe_read_ops;
static struct file_ops pipe_write_ops;

#define PIPE_BUF_SIZE 4096

/* Simple spinlock for pipe SMP safety */
static inline void pipe_spin_lock(volatile uint32_t *lock) {
    while (1) {
        uint32_t old = 0;
        uint32_t new = 1;
        asm volatile (
            "lock cmpxchgl %2, %1"
            : "=a"(old), "+m"(*lock)
            : "r"(new), "0"(old)
            : "memory"
        );
        if (old == 0) break;
        asm volatile ("pause" ::: "memory");
    }
}

static inline void pipe_spin_unlock(volatile uint32_t *lock) {
    asm volatile ("movl $0, %0" : "=m"(*lock) : : "memory");
}

/*
 * FIXED (v4.1.4): Blocked-task queue node.
 * Replaces single blocked_reader/writer pointers with a linked list
 * so that multiple readers/writers can be blocked simultaneously.
 * Nodes are allocated on the stack of the blocking function — they
 * remain valid while the task is blocked (stack is not freed).
 * (BUG-009 / 2.6)
 */
struct pipe_blocked_node {
    struct task_struct *task;
    struct pipe_blocked_node *next;
};

/* Pipe ring buffer */
struct pipe_ring {
    char     buf[PIPE_BUF_SIZE];
    uint32_t head;        /* read position */
    uint32_t tail;        /* write position */
    uint32_t count;       /* bytes in buffer */
    int      read_open;   /* read end still open? */
    int      write_open;  /* write end still open? */
    volatile uint32_t lock;  /* spinlock for SMP safety */
    struct pipe_blocked_node *blocked_readers;  /* linked list of blocked readers */
    struct pipe_blocked_node *blocked_writers;  /* linked list of blocked writers */
    struct inode *read_inode;   /* FIXED (v4.2.0): back-pointer to read inode */
    struct inode *write_inode;  /* FIXED (v4.2.0): back-pointer to write inode */
};

/* ================================================================
 * Pipe file operations
 * ================================================================ */

static int pipe_open(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp;
    return 0;
}

static ssize_t pipe_read(struct file *filp, void *buf, size_t count,
                         off_t *offset) {
    (void)offset;
    if (!filp || !filp->inode) return -1;
    struct pipe_ring *ring = (struct pipe_ring *)filp->inode->priv;
    if (!ring) return -1;

    /*
     * FIXED (v4.1.4): Stack-allocated node for the blocked-readers list.
     * Replaces single blocked_reader pointer.  Multiple readers can now
     * block simultaneously; all are woken when data arrives.  (BUG-009)
     */
    struct pipe_blocked_node node;
    node.task = current;
    node.next = NULL;

    /* Block until data available, write end closed, or signal.
     * Use spinlock for SMP safety: protect ring buffer state checks
     * and modifications from concurrent access by pipe_write/pipe_close. */
    for (;;) {
        pipe_spin_lock(&ring->lock);

        if (ring->count > 0) break;  /* data available */
        if (!ring->write_open) {
            pipe_spin_unlock(&ring->lock);
            return 0;  /* EOF */
        }
        /*
         * FIXED (v4.1.8): Set state to TASK_BLOCKED BEFORE unlocking.
         * Previously, the state was set AFTER unlock, creating a race:
         * the writer could wake us (set state=TASK_READY) between our
         * unlock and state=TASK_BLOCKED, then we overwrite it with
         * TASK_BLOCKED and call schedule() — permanent hang.
         * (BUG C-1 / P0-4)
         */
        current->state = TASK_BLOCKED;
        /* Add to blocked readers linked list (head insertion) */
        node.next = ring->blocked_readers;
        ring->blocked_readers = &node;
        pipe_spin_unlock(&ring->lock);

        if (!current) return -1;
        if (current->sig && current->sig->pending) {
            /* Remove ourselves from the blocked list on signal */
            pipe_spin_lock(&ring->lock);
            struct pipe_blocked_node **prev = &ring->blocked_readers;
            while (*prev) {
                if (*prev == &node) {
                    *prev = node.next;
                    break;
                }
                prev = &(*prev)->next;
            }
            pipe_spin_unlock(&ring->lock);
            current->state = TASK_READY;
            current->t_errno = EINTR; return -1;
        }
        schedule();
        /* After wakeup, re-acquire lock and re-check */
    }

    /* Read from ring buffer (lock still held) */
    size_t toread = count;
    if (toread > ring->count) toread = ring->count;

    /*
     * Copy from ring buffer. First copy from head to end of buffer,
     * then wrap around and copy from start if needed.
     */
    size_t first_chunk = PIPE_BUF_SIZE - ring->head;
    if (first_chunk > toread) first_chunk = toread;

    /*
     * FIXED (v4.2.5): BUG-PIPE-SMAP
     * Wrap user-space access with stac()/clac() to allow SMAP-
     * protected kernels to copy data to the user buffer.  Without
     * this, memcpy to a user-space address would cause a page fault
     * when SMAP is enabled.
     */
    stac();
    memcpy((char *)buf, ring->buf + ring->head, first_chunk);
    if (toread > first_chunk) {
        memcpy((char *)buf + first_chunk, ring->buf, toread - first_chunk);
    }
    clac();
    ring->head = (ring->head + toread) % PIPE_BUF_SIZE;
    ring->count -= (uint32_t)toread;

    /* Wake ALL blocked writers now that we've freed some space */
    struct pipe_blocked_node *wn = ring->blocked_writers;
    ring->blocked_writers = NULL;
    while (wn) {
        if (wn->task->state == TASK_BLOCKED) {
            wn->task->state = TASK_READY;
        }
        wn = wn->next;
    }

    pipe_spin_unlock(&ring->lock);
    return (ssize_t)toread;
}

static ssize_t pipe_write(struct file *filp, const void *buf, size_t count,
                          off_t *offset) {
    (void)offset;
    if (!filp || !filp->inode) return -1;
    struct pipe_ring *ring = (struct pipe_ring *)filp->inode->priv;
    if (!ring) return -1;

    pipe_spin_lock(&ring->lock);
    if (!ring->read_open) {
        pipe_spin_unlock(&ring->lock);
        return -1;  /* Reader closed → SIGPIPE */
    }
    pipe_spin_unlock(&ring->lock);

    /*
     * FIXED (v4.1.4): Stack-allocated node for the blocked-writers list.
     * Replaces single blocked_writer pointer.  Multiple writers can now
     * block simultaneously; all are woken when space becomes available.
     * (BUG-009 / 2.6)
     */
    struct pipe_blocked_node node;
    node.task = current;
    node.next = NULL;

    size_t total = 0;
    const char *src = (const char *)buf;

    while (total < count) {
        /* Block if buffer is full (with SMP-safe spinlock) */
        for (;;) {
            pipe_spin_lock(&ring->lock);
            if (ring->count < PIPE_BUF_SIZE) break;  /* space available */
            if (!ring->read_open) {
                pipe_spin_unlock(&ring->lock);
                current->t_errno = EPIPE; return -1;
            }
            /*
             * FIXED (v4.1.8): Same race condition as reader side.
             * Set state to TASK_BLOCKED BEFORE unlocking to prevent
             * the reader from waking us between unlock and state set.
             * (BUG C-1 / P0-4)
             */
            current->state = TASK_BLOCKED;
            /* Add to blocked writers linked list (head insertion) */
            node.next = ring->blocked_writers;
            ring->blocked_writers = &node;
            pipe_spin_unlock(&ring->lock);

            if (!current) return -1;
            if (current->sig && current->sig->pending) {
                /* Remove ourselves from the blocked list on signal */
                pipe_spin_lock(&ring->lock);
                struct pipe_blocked_node **prev = &ring->blocked_writers;
                while (*prev) {
                    if (*prev == &node) {
                        *prev = node.next;
                        break;
                    }
                    prev = &(*prev)->next;
                }
                pipe_spin_unlock(&ring->lock);
                current->state = TASK_READY;
                current->t_errno = EINTR; return -1;
            }
            schedule();
        }

        /* lock still held: write to ring buffer */
        size_t space = PIPE_BUF_SIZE - ring->count;
        size_t towrite = count - total;
        if (towrite > space) towrite = space;

        /*
         * Copy to ring buffer. First copy from tail to end of buffer,
         * then wrap around and copy from start if needed.
         */
        size_t first_chunk = PIPE_BUF_SIZE - ring->tail;
        if (first_chunk > towrite) first_chunk = towrite;

        /*
         * FIXED (v4.2.5): BUG-PIPE-SMAP
         * Wrap user-space access with stac()/clac() to allow SMAP-
         * protected kernels to read from the user buffer.  Without
         * this, memcpy from a user-space address would cause a page
         * fault when SMAP is enabled.
         */
        stac();
        memcpy(ring->buf + ring->tail, src + total, first_chunk);
        if (towrite > first_chunk) {
            memcpy(ring->buf, src + total + first_chunk, towrite - first_chunk);
        }
        clac();
        ring->tail = (ring->tail + towrite) % PIPE_BUF_SIZE;
        ring->count += (uint32_t)towrite;

        /* Wake ALL blocked readers now that we've written some data */
        struct pipe_blocked_node *rn = ring->blocked_readers;
        ring->blocked_readers = NULL;
        while (rn) {
            if (rn->task->state == TASK_BLOCKED) {
                rn->task->state = TASK_READY;
            }
            rn = rn->next;
        }

        pipe_spin_unlock(&ring->lock);
        total += towrite;
    }

    return (ssize_t)total;
}

static int pipe_close(struct inode *inode, struct file *filp) {
    if (!inode) return 0;
    struct pipe_ring *ring = (struct pipe_ring *)inode->priv;
    if (!ring) return 0;

    /* Spinlock for SMP safety: prevent concurrent close from both ends */
    pipe_spin_lock(&ring->lock);

    /* Determine read/write end by comparing the inode's ops table */
    if (inode->ops == &pipe_read_ops) {
        ring->read_open = 0;
    } else {
        ring->write_open = 0;
    }

    /* If both ends closed, free the ring.
     * FIXED (v4.2.0): Set both inode->priv to NULL to prevent
     * caller from accessing freed memory via the other inode.
     * (BUG-FS-H1 / Top 10 #5) */
    if (!ring->read_open && !ring->write_open) {
        if (ring->read_inode)  ring->read_inode->priv  = NULL;
        if (ring->write_inode) ring->write_inode->priv = NULL;
        inode->priv = NULL;
        pipe_spin_unlock(&ring->lock);
        kfree(ring);
    } else {
        pipe_spin_unlock(&ring->lock);
    }

    (void)filp;
    return 0;
}

static struct file_ops pipe_read_ops = {
    .open  = pipe_open,
    .read  = pipe_read,
    .write = NULL,       /* read end doesn't support write */
    .close = pipe_close,
};

static struct file_ops pipe_write_ops = {
    .open  = pipe_open,
    .read  = NULL,       /* write end doesn't support read */
    .write = pipe_write,
    .close = pipe_close,
};

/* ================================================================
 * sys_pipe: Create a pipe, return two fds
 *
 * @fds: user-space array of 2 ints → fds[0]=read, fds[1]=write.
 * Returns 0 on success, -1 on error.
 * ================================================================ */

int sys_pipe(int *fds) {
    if (!fds) return -1;
    if (!user_addr_range_ok(fds, 2 * sizeof(int))) return -1;

    /* Allocate ring buffer */
    struct pipe_ring *ring = (struct pipe_ring *)kmalloc(sizeof(*ring));
    if (!ring) return -1;
    memset(ring, 0, sizeof(*ring));
    ring->read_open  = 1;
    ring->write_open = 1;

    /* Create read-end inode */
    struct inode *rinode = (struct inode *)kmalloc(sizeof(*rinode));
    if (!rinode) { kfree(ring); return -1; }
    memset(rinode, 0, sizeof(*rinode));
    rinode->name = "r";
    rinode->ops  = &pipe_read_ops;
    rinode->priv = ring;
    ring->read_inode = rinode;  /* FIXED (v4.2.0): back-pointer for cleanup */

    /* Create write-end inode */
    struct inode *winode = (struct inode *)kmalloc(sizeof(*winode));
    if (!winode) { kfree(rinode); kfree(ring); return -1; }
    memset(winode, 0, sizeof(*winode));
    winode->name = "w";
    winode->ops  = &pipe_write_ops;
    winode->priv = ring;
    ring->write_inode = winode;  /* FIXED (v4.2.0): back-pointer for cleanup */

    /* Create file objects */
    struct file *rfilp = (struct file *)kmalloc(sizeof(*rfilp));
    struct file *wfilp = (struct file *)kmalloc(sizeof(*wfilp));
    if (!rfilp || !wfilp) {
        if (rfilp) kfree(rfilp);
        if (wfilp) kfree(wfilp);
        kfree(winode); kfree(rinode); kfree(ring);
        return -1;
    }
    memset(rfilp, 0, sizeof(*rfilp));
    memset(wfilp, 0, sizeof(*wfilp));
    rfilp->inode = rinode;
    wfilp->inode = winode;

    /* Allocate fds */
    int rfd = fd_alloc(current, rfilp);
    int wfd = fd_alloc(current, wfilp);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) fd_close(current, rfd);
        else if (rfilp) { kfree(rfilp); rfilp = NULL; }
        if (wfd >= 0) fd_close(current, wfd);
        else if (wfilp) { kfree(wfilp); wfilp = NULL; }
        /*
         * FIXED (v4.2.0): Only free ring if pipe_close didn't already
         * free it (i.e., both ends weren't closed).  Check inode->priv
         * to determine if the ring was already freed by pipe_close.
         * Also clear inode->priv to prevent dangling pointers.
         * (BUG-FS-H1 / Top 10 #5)
         */
        if (rinode->priv) kfree(rinode->priv);
        kfree(rinode); kfree(winode);
        return -1;
    }

    /* Write fds to user space */
    int kfds[2] = { rfd, wfd };
    if (copy_to_user(fds, kfds, sizeof(kfds)) != 0) {
        /*
         * FIXED (v4.2.0): fd_close may free the ring buffer via
         * pipe_close when both ends are closed.  After fd_close,
         * the ring, rinode, and winode are still allocated but
         * ring may be freed.  We must free the remaining inodes
         * to prevent memory leaks.  (BUG-FS-H1 / Top 10 #5)
         */
        fd_close(current, rfd);
        fd_close(current, wfd);
        /* ring may have been freed by pipe_close; check before freeing */
        if (rinode->priv) kfree(rinode->priv);
        kfree(rinode); kfree(winode);
        return -1;
    }

    log_printf(LOG_LEVEL_DEBUG, "pipe: created fds [%d, %d]\n", rfd, wfd);
    return 0;
}
