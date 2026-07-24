/*
 * unix.h - AF_UNIX (Unix domain sockets) definitions        /* AF_UNIX (v4.2.6) */
 *
 * Implements local IPC via filesystem-path-addressed sockets.
 * Supports SOCK_STREAM (ring-buffer, pipe-like) and SOCK_DGRAM
 * (message queue).  Synchronisation via kernel spinlocks.
 */
#ifndef UNIX_H
#define UNIX_H

#include <stdint.h>
#include <stddef.h>
#include "../smp.h"
#include "../fs.h"

/* ================================================================
 * Address family & constants                                   /* AF_UNIX (v4.2.6) */
 * ================================================================ */
#define AF_UNIX            1
#define AF_LOCAL           1

#define UNIX_PATH_MAX      108
#define UNIX_BACKLOG_MAX   128
#define UNIX_BUF_SIZE      65536   /* 64 KiB default ring buffer */

/* Socket types (re-declared here to avoid circular include) */
#ifndef SOCK_STREAM
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#endif

/* ================================================================
 * Unix socket states                                          /* AF_UNIX (v4.2.6) */
 * ================================================================ */
enum unix_state {
    UNIX_CLOSED,
    UNIX_LISTENING,
    UNIX_CONNECTED,
    UNIX_CONNECTING,
};

/* ================================================================
 * Unix domain socket address                                  /* AF_UNIX (v4.2.6) */
 * ================================================================ */
struct sockaddr_un {
    uint16_t sun_family;               /* AF_UNIX */
    char     sun_path[UNIX_PATH_MAX];  /* filesystem path */
};

/* ================================================================
 * Forward declarations                                        /* AF_UNIX (v4.2.6) */
 * ================================================================ */
struct unix_sock;
struct task_struct;

/* Blocked-task queue node (stack-allocated, same pattern as pipe.c) */
struct unix_blocked_node {
    struct task_struct       *task;
    struct unix_blocked_node *next;
};

/* Datagram message node (for SOCK_DGRAM message queue) */
struct unix_dgram {
    char                 *data;
    int                   len;
    struct sockaddr_un    addr;
    int                   addr_valid;
    struct unix_dgram    *next;
};

/* ================================================================
 * Unix domain socket control block                             /* AF_UNIX (v4.2.6) */
 * ================================================================ */
struct unix_sock {
    int             state;             /* UNIX_CLOSED / LISTENING / CONNECTED / CONNECTING */
    int             type;              /* SOCK_STREAM or SOCK_DGRAM */
    char            path[UNIX_PATH_MAX]; /* bound filesystem path */
    struct inode   *vnode;             /* VFS inode for the socket pseudo-file */

    /* ---- Ring buffer (SOCK_STREAM) ---- */
    char           *buf;               /* kmalloc'd buffer */
    uint32_t        head;              /* read position */
    uint32_t        tail;              /* write position */
    uint32_t        count;             /* bytes currently in buffer */
    uint32_t        buf_size;          /* total buffer capacity */

    /* ---- Accept queue (SOCK_STREAM listening sockets) ---- */
    struct unix_sock *accept_queue;    /* linked list of pending connections */
    int              accept_queue_len;
    int              backlog;

    /* ---- Connection (SOCK_STREAM connected sockets) ---- */
    struct unix_sock *peer;            /* the other end of the connection */

    /* ---- Linked-list node for accept queue ---- */
    struct unix_sock *next;

    /* ---- Datagram message queue (SOCK_DGRAM) ---- */
    struct unix_dgram *dgram_head;
    struct unix_dgram *dgram_tail;
    int                dgram_count;

    /* ---- Blocked task lists (SOCK_STREAM) ---- */
    struct unix_blocked_node *blocked_readers;
    struct unix_blocked_node *blocked_writers;

    /* ---- Lifecycle ---- */
    spinlock_t       lock;
    int              read_open;        /* 1 = read side still open */
    int              write_open;       /* 1 = write side still open */
    int              refcount;         /* reference count for safe cleanup */
};

/* ================================================================
 * Public API                                                   /* AF_UNIX (v4.2.6) */
 * ================================================================ */
void unix_init(void);

struct unix_sock *unix_socket_create(int type);
int  unix_bind(struct unix_sock *sk, const struct sockaddr_un *addr);
int  unix_listen(struct unix_sock *sk, int backlog);
struct unix_sock *unix_accept(struct unix_sock *sk);
int  unix_connect(struct unix_sock *sk, const struct sockaddr_un *addr);
int  unix_send(struct unix_sock *sk, const void *data, int len);
int  unix_recv(struct unix_sock *sk, void *buf, int max_len);
int  unix_sendto(struct unix_sock *sk, const void *data, int len,
                 const struct sockaddr_un *addr);
int  unix_recvfrom(struct unix_sock *sk, void *buf, int max_len,
                   struct sockaddr_un *addr, int *addrlen);
void unix_close(struct unix_sock *sk);
int  unix_poll(struct unix_sock *sk, int events);
int  unix_getsockname(struct unix_sock *sk, struct sockaddr_un *addr,
                      int *addrlen);

#endif /* UNIX_H */