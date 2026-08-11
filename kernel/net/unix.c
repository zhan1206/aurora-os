/*
 * unix.c - AF_UNIX (Unix domain sockets) implementation       /* AF_UNIX (v4.2.6) */
 *
 * Provides local IPC through filesystem-path-addressed sockets.
 * SOCK_STREAM uses a ring buffer (pipe-like) for bidirectional
 * byte streams.  SOCK_DGRAM uses a message queue for datagrams.
 *
 * Synchronisation: spinlock_t on each socket; a global lock
 * protects the bound-socket registry.
 */
#include "unix.h"
#include "../sched.h"
#include "../mem.h"
#include "../vfs.h"
#include "../include/log.h"
#include "../include/errno.h"
#include <string.h>
#include <stdint.h>

/* ================================================================
 * Global bound-socket registry                                  /* AF_UNIX (v4.2.6) */
 * ================================================================ */
#define UNIX_MAX_SOCKETS  64

static struct unix_sock *unix_bound_list = NULL;
static spinlock_t unix_global_lock;

/* ================================================================
 * Helpers                                                       /* AF_UNIX (v4.2.6) */
 * ================================================================ */

/* Wake all tasks in a blocked-node linked list */
static void unix_wake_all(struct unix_blocked_node **list) {
    struct unix_blocked_node *n = *list;
    *list = NULL;
    while (n) {
        if (n->task && n->task->state == TASK_BLOCKED) {
            n->task->state = TASK_READY;
        }
        n = n->next;
    }
}

/* Find a bound socket by path (caller must hold unix_global_lock) */
static struct unix_sock *unix_find_bound(const char *path) {
    struct unix_sock *sk = unix_bound_list;
    while (sk) {
        if (sk->path[0] && strcmp(sk->path, path) == 0) {
            return sk;
        }
        sk = sk->next;
    }
    return NULL;
}

/* ================================================================
 * unix_init                                                    /* AF_UNIX (v4.2.6) */
 * ================================================================ */
void unix_init(void) {
    spin_init(&unix_global_lock);
    unix_bound_list = NULL;
    log_printf(LOG_LEVEL_INFO, "unix: AF_UNIX domain socket subsystem initialised\n");
}

/* ================================================================
 * unix_socket_create                                           /* AF_UNIX (v4.2.6) */
 * ================================================================ */
struct unix_sock *unix_socket_create(int type) {
    struct unix_sock *sk = (struct unix_sock *)kmalloc(sizeof(*sk));
    if (!sk) return NULL;

    memset(sk, 0, sizeof(*sk));
    sk->state     = UNIX_CLOSED;
    sk->type      = type;
    sk->read_open  = 1;
    sk->write_open = 1;
    sk->refcount   = 1;  /* FIXED (v4.3.3): UNIX-001 — atomic refcount */
    sk->magic      = UNIX_SOCK_MAGIC;  /* FIXED (v4.3.9): BOOT-03 — type confusion guard */
    spin_init(&sk->lock);

    if (type == SOCK_STREAM) {
        sk->buf_size = UNIX_BUF_SIZE;
        sk->buf = (char *)kmalloc(sk->buf_size);
        if (!sk->buf) {
            kfree(sk);
            return NULL;
        }
        memset(sk->buf, 0, sk->buf_size);
    }

    return sk;
}

/* ================================================================
 * unix_bind                                                    /* AF_UNIX (v4.2.6) */
 * ================================================================ */
int unix_bind(struct unix_sock *sk, const struct sockaddr_un *addr) {
    if (!sk || !addr) return -EINVAL;
    if (addr->sun_family != AF_UNIX) return -EAFNOSUPPORT;

    /* Validate path length */
    size_t path_len = 0;
    while (path_len < UNIX_PATH_MAX && addr->sun_path[path_len] != '\0') {
        path_len++;
    }
    if (path_len == 0 || path_len >= UNIX_PATH_MAX) {
        return -EINVAL;
    }

    /* Acquire global lock first to avoid AB-BA with unix_sendto */
    spin_lock(&unix_global_lock);
    spin_lock(&sk->lock);

    if (sk->state != UNIX_CLOSED) {
        spin_unlock(&sk->lock);
        spin_unlock(&unix_global_lock);
        return -EINVAL;
    }

    /* Check for duplicate path */
    if (unix_find_bound(addr->sun_path)) {
        spin_unlock(&sk->lock);
        spin_unlock(&unix_global_lock);
        return -EADDRINUSE;
    }

    /* Copy path and add to global registry */
    memcpy(sk->path, addr->sun_path, path_len);
    sk->path[path_len] = '\0';

    /* Try to create a VFS inode for the socket path */
    sk->vnode = vfs_lookup(sk->path);
    if (!sk->vnode) {
        /* Path doesn't exist in VFS; that's okay for AF_UNIX.
         * The socket is identified by the string path in our registry. */
    }

    /* Add to bound list */
    sk->next = unix_bound_list;
    unix_bound_list = sk;

    spin_unlock(&sk->lock);
    spin_unlock(&unix_global_lock);

    log_printf(LOG_LEVEL_DEBUG, "unix: bound socket to '%s'\n", sk->path);
    return 0;
}

/* ================================================================
 * unix_listen                                                    /* AF_UNIX (v4.2.6) */
 * ================================================================ */
int unix_listen(struct unix_sock *sk, int backlog) {
    if (!sk) return -EINVAL;
    if (sk->type != SOCK_STREAM) return -EOPNOTSUPP;

    spin_lock(&sk->lock);
    if (sk->state != UNIX_CLOSED || sk->path[0] == '\0') {
        /* Must be bound first */
        spin_unlock(&sk->lock);
        return -EINVAL;
    }

    if (backlog < 0) backlog = 0;
    if (backlog > UNIX_BACKLOG_MAX) backlog = UNIX_BACKLOG_MAX;

    sk->backlog = backlog;
    sk->state = UNIX_LISTENING;
    sk->accept_queue = NULL;
    sk->accept_queue_len = 0;

    spin_unlock(&sk->lock);
    return 0;
}

/* ================================================================
 * unix_accept                                                    /* AF_UNIX (v4.2.6) */
 *
 * FIXED (v4.2.7): BUG-UNIX-BACKLOG — The backlog limit is enforced by
 * unix_connect() before adding a connection to the accept queue.
 * unix_accept() does not need to check the backlog because it only
 * dequeues from the already-bounded queue.  The queue length is
 * guaranteed to be <= backlog by unix_connect().
 * ================================================================ */
struct unix_sock *unix_accept(struct unix_sock *sk) {
    if (!sk) return NULL;
    if (sk->type != SOCK_STREAM) return NULL;

    spin_lock(&sk->lock);

    if (sk->state != UNIX_LISTENING) {
        spin_unlock(&sk->lock);
        return NULL;
    }

    /* Block until a connection is pending or signal */
    struct unix_blocked_node node;
    node.task = current;
    node.next = NULL;

    for (;;) {
        if (sk->accept_queue_len > 0) break;

        /* Block: add to blocked_readers and wait */
        current->state = TASK_BLOCKED;
        node.next = sk->blocked_readers;
        sk->blocked_readers = &node;
        spin_unlock(&sk->lock);

        if (current && current->sig && current->sig->pending) {
            /* Remove from blocked list on signal */
            spin_lock(&sk->lock);
            struct unix_blocked_node **prev = &sk->blocked_readers;
            while (*prev) {
                if (*prev == &node) {
                    *prev = node.next;
                    break;
                }
                prev = &(*prev)->next;
            }
            spin_unlock(&sk->lock);
            current->state = TASK_READY;
            current->t_errno = EINTR;
            return NULL;
        }
        schedule();

        spin_lock(&sk->lock);
        if (sk->state != UNIX_LISTENING) {
            spin_unlock(&sk->lock);
            return NULL;
        }
    }

    /* Dequeue first pending connection */
    struct unix_sock *new_sk = sk->accept_queue;
    sk->accept_queue = new_sk->next;
    sk->accept_queue_len--;
    new_sk->next = NULL;

    spin_unlock(&sk->lock);
    return new_sk;
}

/* ================================================================
 * unix_connect                                                   /* AF_UNIX (v4.2.6) */
 * ================================================================ */
int unix_connect(struct unix_sock *sk, const struct sockaddr_un *addr) {
    if (!sk || !addr) return -EINVAL;
    if (addr->sun_family != AF_UNIX) return -EAFNOSUPPORT;
    if (sk->type != SOCK_STREAM) return -EOPNOTSUPP;

    spin_lock(&sk->lock);
    if (sk->state != UNIX_CLOSED) {
        spin_unlock(&sk->lock);
        return -EISCONN;
    }
    spin_unlock(&sk->lock);

    spin_lock(&unix_global_lock);

    /* Look up the listening socket by path */
    struct unix_sock *listener = unix_find_bound(addr->sun_path);
    if (!listener) {
        spin_unlock(&unix_global_lock);
        return -ECONNREFUSED;
    }

    spin_lock(&listener->lock);

    if (listener->state != UNIX_LISTENING) {
        spin_unlock(&listener->lock);
        spin_unlock(&unix_global_lock);
        return -ECONNREFUSED;
    }

    if (listener->accept_queue_len >= listener->backlog) {
        spin_unlock(&listener->lock);
        spin_unlock(&unix_global_lock);
        return -ECONNREFUSED;
    }

    /* Create a new socket for the server side of this connection */
    struct unix_sock *server_sk = unix_socket_create(SOCK_STREAM);
    if (!server_sk) {
        spin_unlock(&listener->lock);
        spin_unlock(&unix_global_lock);
        return -ENOMEM;
    }

    /* Set up the connection pair */
    spin_lock(&sk->lock);

    sk->peer = server_sk;
    server_sk->peer = sk;
    sk->state = UNIX_CONNECTED;
    server_sk->state = UNIX_CONNECTED;

    /* Copy the path from listener for getsockname */
    memcpy(server_sk->path, listener->path, UNIX_PATH_MAX);

    /* Add server socket to the listener's accept queue */
    server_sk->next = listener->accept_queue;
    listener->accept_queue = server_sk;
    listener->accept_queue_len++;

    /* Wake up any blocked accept() callers */
    unix_wake_all(&listener->blocked_readers);

    spin_unlock(&sk->lock);
    spin_unlock(&listener->lock);
    spin_unlock(&unix_global_lock);

    log_printf(LOG_LEVEL_DEBUG, "unix: connected to '%s'\n", listener->path);
    return 0;
}

/* ================================================================
 * unix_send (SOCK_STREAM)                                       /* AF_UNIX (v4.2.6) */
 * ================================================================ */
int unix_send(struct unix_sock *sk, const void *data, int len) {
    if (!sk || !data || len <= 0) return -EINVAL;
    if (sk->type != SOCK_STREAM) return -EOPNOTSUPP;

    spin_lock(&sk->lock);
    if (sk->state != UNIX_CONNECTED || !sk->peer) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }

    struct unix_sock *peer = sk->peer;

    /* Release our lock before acquiring peer's to avoid deadlock.
     * We hold a reference to peer (via the connection), so it won't
     * disappear. */
    spin_unlock(&sk->lock);

    /* Block until there is space in the peer's ring buffer */
    struct unix_blocked_node node;
    node.task = current;
    node.next = NULL;

    int total = 0;
    const char *src = (const char *)data;

    while (total < len) {
        spin_lock(&peer->lock);

        /* Check peer is still open for reading */
        if (!peer->read_open || peer->state != UNIX_CONNECTED) {
            spin_unlock(&peer->lock);
            return -EPIPE;
        }

        for (;;) {
            if (peer->count < peer->buf_size) break; /* space available */

            /* Block: add to peer's blocked_writers */
            current->state = TASK_BLOCKED;
            node.next = peer->blocked_writers;
            peer->blocked_writers = &node;
            spin_unlock(&peer->lock);

            if (current && current->sig && current->sig->pending) {
                spin_lock(&peer->lock);
                struct unix_blocked_node **prev = &peer->blocked_writers;
                while (*prev) {
                    if (*prev == &node) {
                        *prev = node.next;
                        break;
                    }
                    prev = &(*prev)->next;
                }
                spin_unlock(&peer->lock);
                current->state = TASK_READY;
                current->t_errno = EINTR;
                return (total > 0) ? total : -1;
            }
            schedule();
            spin_lock(&peer->lock);

            if (!peer->read_open || peer->state != UNIX_CONNECTED) {
                spin_unlock(&peer->lock);
                return -EPIPE;
            }
        }

        /* Write to peer's ring buffer */
        uint32_t space = peer->buf_size - peer->count;
        uint32_t towrite = (uint32_t)(len - total);
        if (towrite > space) towrite = space;

        uint32_t first_chunk = peer->buf_size - peer->tail;
        if (first_chunk > towrite) first_chunk = towrite;

        memcpy(peer->buf + peer->tail, src + total, first_chunk);
        if (towrite > first_chunk) {
            memcpy(peer->buf, src + total + first_chunk,
                   towrite - first_chunk);
        }
        peer->tail = (peer->tail + towrite) % peer->buf_size;
        peer->count += towrite;

        /* Wake blocked readers on the peer */
        unix_wake_all(&peer->blocked_readers);

        spin_unlock(&peer->lock);
        total += (int)towrite;
    }

    return total;
}

/* ================================================================
 * unix_recv (SOCK_STREAM)                                       /* AF_UNIX (v4.2.6) */
 * ================================================================ */
int unix_recv(struct unix_sock *sk, void *buf, int max_len) {
    if (!sk || !buf || max_len <= 0) return -EINVAL;
    if (sk->type != SOCK_STREAM) return -EOPNOTSUPP;

    spin_lock(&sk->lock);
    if (sk->state != UNIX_CONNECTED) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }

    /* Block until data is available, write end closed, or signal */
    struct unix_blocked_node node;
    node.task = current;
    node.next = NULL;

    for (;;) {
        if (sk->count > 0) break;
        if (!sk->write_open || !sk->peer || sk->peer->state != UNIX_CONNECTED) {
            spin_unlock(&sk->lock);
            return 0; /* EOF */
        }

        current->state = TASK_BLOCKED;
        node.next = sk->blocked_readers;
        sk->blocked_readers = &node;
        spin_unlock(&sk->lock);

        if (current && current->sig && current->sig->pending) {
            spin_lock(&sk->lock);
            struct unix_blocked_node **prev = &sk->blocked_readers;
            while (*prev) {
                if (*prev == &node) {
                    *prev = node.next;
                    break;
                }
                prev = &(*prev)->next;
            }
            spin_unlock(&sk->lock);
            current->state = TASK_READY;
            current->t_errno = EINTR;
            return -1;
        }
        schedule();
        spin_lock(&sk->lock);
        if (sk->state != UNIX_CONNECTED) {
            spin_unlock(&sk->lock);
            return -ENOTCONN;
        }
    }

    /* Read from own ring buffer */
    uint32_t toread = (uint32_t)max_len;
    if (toread > sk->count) toread = sk->count;

    uint32_t first_chunk = sk->buf_size - sk->head;
    if (first_chunk > toread) first_chunk = toread;

    memcpy(buf, sk->buf + sk->head, first_chunk);
    if (toread > first_chunk) {
        memcpy((char *)buf + first_chunk, sk->buf, toread - first_chunk);
    }
    sk->head = (sk->head + toread) % sk->buf_size;
    sk->count -= toread;

    /* Wake blocked writers (room in buffer) */
    unix_wake_all(&sk->blocked_writers);

    spin_unlock(&sk->lock);
    return (int)toread;
}

/* ================================================================
 * unix_sendto (SOCK_DGRAM)                                      /* AF_UNIX (v4.2.6) */
 * ================================================================ */
int unix_sendto(struct unix_sock *sk, const void *data, int len,
                const struct sockaddr_un *addr) {
    if (!sk || !data || len <= 0) return -EINVAL;
    if (sk->type != SOCK_DGRAM) return -EOPNOTSUPP;
    if (!addr || addr->sun_family != AF_UNIX) return -EINVAL;

    spin_lock(&unix_global_lock);

    /* Look up the destination socket by path */
    struct unix_sock *dest = unix_find_bound(addr->sun_path);
    if (!dest || dest->type != SOCK_DGRAM) {
        spin_unlock(&unix_global_lock);
        return -ECONNREFUSED;
    }

    spin_lock(&dest->lock);

    if (!dest->read_open) {
        spin_unlock(&dest->lock);
        spin_unlock(&unix_global_lock);
        return -ECONNREFUSED;
    }

    /* Allocate a datagram node */
    struct unix_dgram *dg = (struct unix_dgram *)kmalloc(sizeof(*dg));
    if (!dg) {
        spin_unlock(&dest->lock);
        spin_unlock(&unix_global_lock);
        return -ENOMEM;
    }

    dg->data = (char *)kmalloc((size_t)len);
    if (!dg->data) {
        kfree(dg);
        spin_unlock(&dest->lock);
        spin_unlock(&unix_global_lock);
        return -ENOMEM;
    }

    memcpy(dg->data, data, (size_t)len);
    dg->len = len;
    dg->next = NULL;

    /* Copy sender address if the socket is bound */
    if (sk->path[0] != '\0') {
        dg->addr.sun_family = AF_UNIX;
        memcpy(dg->addr.sun_path, sk->path, UNIX_PATH_MAX);
        dg->addr_valid = 1;
    } else {
        dg->addr_valid = 0;
    }

    /* Enqueue into destination's message queue */
    if (dest->dgram_tail) {
        dest->dgram_tail->next = dg;
        dest->dgram_tail = dg;
    } else {
        dest->dgram_head = dg;
        dest->dgram_tail = dg;
    }
    dest->dgram_count++;

    /* Wake blocked readers */
    unix_wake_all(&dest->blocked_readers);

    spin_unlock(&dest->lock);
    spin_unlock(&unix_global_lock);

    return len;
}

/* ================================================================
 * unix_recvfrom (SOCK_DGRAM)                                    /* AF_UNIX (v4.2.6) */
 * ================================================================ */
int unix_recvfrom(struct unix_sock *sk, void *buf, int max_len,
                  struct sockaddr_un *addr, int *addrlen) {
    if (!sk || !buf || max_len <= 0) return -EINVAL;
    if (sk->type != SOCK_DGRAM) return -EOPNOTSUPP;

    spin_lock(&sk->lock);

    /* Block until a datagram is available or signal */
    struct unix_blocked_node node;
    node.task = current;
    node.next = NULL;

    for (;;) {
        if (sk->dgram_count > 0) break;
        if (!sk->read_open) {
            spin_unlock(&sk->lock);
            return 0;
        }

        current->state = TASK_BLOCKED;
        node.next = sk->blocked_readers;
        sk->blocked_readers = &node;
        spin_unlock(&sk->lock);

        if (current && current->sig && current->sig->pending) {
            spin_lock(&sk->lock);
            struct unix_blocked_node **prev = &sk->blocked_readers;
            while (*prev) {
                if (*prev == &node) {
                    *prev = node.next;
                    break;
                }
                prev = &(*prev)->next;
            }
            spin_unlock(&sk->lock);
            current->state = TASK_READY;
            current->t_errno = EINTR;
            return -1;
        }
        schedule();
        spin_lock(&sk->lock);
    }

    /* Dequeue the first datagram */
    struct unix_dgram *dg = sk->dgram_head;
    sk->dgram_head = dg->next;
    if (!sk->dgram_head) sk->dgram_tail = NULL;
    sk->dgram_count--;

    int copy_len = dg->len;
    if (copy_len > max_len) copy_len = max_len;
    memcpy(buf, dg->data, (size_t)copy_len);

    /* Fill in source address if requested */
    if (addr && addrlen && dg->addr_valid) {
        int alen = (int)sizeof(struct sockaddr_un);
        if (*addrlen < alen) alen = *addrlen;
        memcpy(addr, &dg->addr, (size_t)alen);
        *addrlen = (int)sizeof(struct sockaddr_un);
    } else if (addrlen) {
        *addrlen = 0;
    }

    spin_unlock(&sk->lock);

    kfree(dg->data);
    kfree(dg);
    return copy_len;
}

/* ================================================================
 * unix_socket_flush_queues — Free all queued data           /* FIXED (v4.3.3): UNIX-001 */
 * ================================================================ */
static void unix_socket_flush_queues(struct unix_sock *sk) {
    if (!sk) return;

    /* Free ring buffer */
    if (sk->buf) {
        kfree(sk->buf);
        sk->buf = NULL;
    }

    /* Free any remaining datagrams */
    while (sk->dgram_head) {
        struct unix_dgram *dg = sk->dgram_head;
        sk->dgram_head = dg->next;
        if (dg->data) kfree(dg->data);
        kfree(dg);
    }
}

/* ================================================================
 * unix_sock_get — Atomic refcount increment                 /* FIXED (v4.3.3): UNIX-001 */
 * ================================================================ */
static inline void unix_sock_get(struct unix_sock *sk) {
    if (!sk) return;
    __sync_fetch_and_add(&sk->refcount, 1);
}

/* ================================================================
 * unix_sock_put — Atomic refcount decrement & free when zero /* FIXED (v4.3.3): UNIX-001 */
 * ================================================================ */
static void unix_sock_put(struct unix_sock *sk) {
    if (!sk) return;
    if (__sync_sub_and_fetch(&sk->refcount, 1) == 0) {
        /* Last reference — free the socket */
        if (sk->peer) {
            sk->peer->peer = NULL;
            unix_sock_put(sk->peer);
        }
        unix_socket_flush_queues(sk);
        kfree(sk);  /* FIXED (v4.3.3): UNIX-001 — safe cleanup */
    }
}

/* ================================================================
 * unix_socket_close — Close by fd with atomic refcount       /* FIXED (v4.3.3): UNIX-001 */
 * ================================================================ */
static int unix_socket_close(int fd) {
    struct unix_sock *sock = fd_to_unix_sock(fd);
    if (!sock) return -EBADF;

    spin_lock(&sock->lock);
    sock->state = UNIX_CLOSED;
    spin_unlock(&sock->lock);

    /* Notify peer */
    if (sock->peer) {
        sock->peer->peer = NULL;
    }

    /* Drop the fd reference */
    unix_sock_put(sock);
    return 0;
}

/* ================================================================
 * unix_socket_fork — Child inherits reference on fork       /* FIXED (v4.3.3): UNIX-001 */
 * ================================================================ */
int unix_socket_fork(struct unix_sock *sock) {
    if (!sock) return -EINVAL;
    unix_sock_get(sock);  /* Child inherits reference */
    return 0;
}

/* ================================================================
 * unix_close                                                    /* AF_UNIX (v4.2.6) */
 * ================================================================ */
void unix_close(struct unix_sock *sk) {
    if (!sk) return;

    /* Acquire locks in address order to avoid AB-BA deadlock */
    struct unix_sock *peer = NULL;

    spin_lock(&sk->lock);
    peer = sk->peer;
    if (peer) {
        /* Lock ordering: lower address first */
        if ((uintptr_t)sk < (uintptr_t)peer) {
            spin_lock(&peer->lock);
        } else {
            spin_unlock(&sk->lock);
            spin_lock(&peer->lock);
            spin_lock(&sk->lock);
        }
    }

    /* Mark both ends closed */
    sk->read_open = 0;
    sk->write_open = 0;

    /* Wake all blocked readers and writers */
    unix_wake_all(&sk->blocked_readers);
    unix_wake_all(&sk->blocked_writers);

    /* Wake peer's blocked readers/writers */
    if (peer) {
        peer->read_open = 0;
        peer->write_open = 0;
        unix_wake_all(&peer->blocked_readers);
        unix_wake_all(&peer->blocked_writers);
        /* FIXED (v4.3.3): UNIX-001 — Clear the peer's
         * back-reference.  The peer's own state is NOT set to
         * CLOSED here — the peer manages its own lifecycle.
         * Release locks before calling unix_sock_put to avoid
         * use-after-free on the lock object. */
        peer->peer = NULL;
        sk->peer = NULL;
        spin_unlock(&peer->lock);
    }

    sk->state = UNIX_CLOSED;

    spin_unlock(&sk->lock);

    /* Remove from global bound list */
    spin_lock(&unix_global_lock);
    if (sk->path[0] != '\0') {
        struct unix_sock **prev = &unix_bound_list;
        while (*prev) {
            if (*prev == sk) {
                *prev = sk->next;
                break;
            }
            prev = &(*prev)->next;
        }
    }
    spin_unlock(&unix_global_lock);

    /* FIXED (v4.3.3): UNIX-001 — Drop references after all locks released.
     * unix_sock_put is atomic and may free the socket if refcount reaches 0. */
    if (peer) {
        unix_sock_put(peer);
    }
    unix_sock_put(sk);
}

/* ================================================================
 * unix_poll                                                     /* AF_UNIX (v4.2.6) */
 * ================================================================ */
int unix_poll(struct unix_sock *sk, int events) {
    if (!sk) return 0;
    int revents = 0;

    spin_lock(&sk->lock);

    if (sk->type == SOCK_STREAM) {
        if (events & 0x001) { /* POLLIN */
            if (sk->state == UNIX_LISTENING && sk->accept_queue_len > 0) {
                revents |= 0x001;
            } else if (sk->state == UNIX_CONNECTED && sk->count > 0) {
                revents |= 0x001;
            } else if (sk->state != UNIX_CONNECTED && sk->state != UNIX_LISTENING) {
                revents |= 0x001; /* HUP */
            }
        }
        if (events & 0x004) { /* POLLOUT */
            if (sk->state == UNIX_CONNECTED) {
                struct unix_sock *peer = sk->peer;
                if (peer) {
                    spin_lock(&peer->lock);
                    if (peer->count < peer->buf_size) {
                        revents |= 0x004;
                    }
                    spin_unlock(&peer->lock);
                }
            }
        }
        if (events & 0x010) { /* POLLHUP */
            if (sk->state != UNIX_CONNECTED) {
                revents |= 0x001; /* readable with EOF */
            }
        }
    } else {
        /* SOCK_DGRAM */
        if (events & 0x001) {
            if (sk->dgram_count > 0 || !sk->read_open) {
                revents |= 0x001;
            }
        }
        if (events & 0x004) {
            revents |= 0x004; /* DGRAM is always writable */
        }
    }

    spin_unlock(&sk->lock);
    return revents;
}

/* ================================================================
 * unix_getsockname                                              /* AF_UNIX (v4.2.6) */
 * ================================================================ */
int unix_getsockname(struct unix_sock *sk, struct sockaddr_un *addr,
                     int *addrlen) {
    if (!sk || !addr || !addrlen) return -EINVAL;

    spin_lock(&sk->lock);

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;

    if (sk->path[0] != '\0') {
        memcpy(addr->sun_path, sk->path, UNIX_PATH_MAX);
    }

    *addrlen = (int)sizeof(struct sockaddr_un);

    spin_unlock(&sk->lock);
    return 0;
}