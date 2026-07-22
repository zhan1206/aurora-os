/*
 * dhcp.c - DHCP Client (RFC 2131)
 *
 * FIXED (v4.2.2): Converted to async state machine.  The old dhcp_run()
 * used blocking retry loops in dhcp_handle_offer() and dhcp_handle_ack()
 * which called net_poll()/udp_recvfrom() synchronously, blocking the
 * entire kernel during boot.  Now dhcp_start() initiates the state
 * machine and dhcp_poll() advances it from net_poll() without blocking.
 */

#include "net.h"
#include "log.h"
#include "string.h"
#include "../mem.h"
#include "../perf.h"
#include <stdint.h>

/* Byte order conversion (local, same as net.c) */
static inline uint16_t ntohs(uint16_t n) {
    return ((n & 0xFF) << 8) | ((n & 0xFF00) >> 8);
}

static inline uint16_t htons(uint16_t n) {
    return ntohs(n);
}

static inline uint32_t ntohl(uint32_t n) {
    return ((n & 0xFF) << 24) | ((n & 0xFF00) << 8) |
           ((n & 0xFF0000) >> 8) | ((n & 0xFF000000) >> 24);
}

static inline uint32_t htonl(uint32_t n) {
    return ntohl(n);
}

/*
 * FIXED (v4.2.2): Async DHCP state machine states.
 * The old code used a single synchronous dhcp_run() that blocked
 * the kernel.  Now dhcp_poll() advances through these states
 * without blocking, called periodically from net_poll().
 */
enum dhcp_state {
    DHCP_IDLE,           /* Not started */
    DHCP_DISCOVER_SENT,  /* DISCOVER sent, awaiting OFFER */
    DHCP_WAIT_OFFER,     /* Actively waiting for OFFER */
    DHCP_REQUEST_SENT,   /* REQUEST sent, awaiting ACK */
    DHCP_WAIT_ACK,       /* Actively waiting for ACK */
    DHCP_BOUND,          /* Configured with IP */
    DHCP_ERROR           /* Failed, will retry after delay */
};

/* DHCP client state */
static uint32_t dhcp_xid = 0;
static uint8_t  dhcp_offered_ip[4];
static uint8_t  dhcp_server_ip[4];
static int      dhcp_initialized = 0;

/*
 * FIXED (v4.2.2): Async state machine tracking.
 */
static enum dhcp_state dhcp_state = DHCP_IDLE;
static int  dhcp_retry_count = 0;       /* Number of retries in current state */
static int  dhcp_timeout_ticks = 0;     /* Poll ticks elapsed in current state */

/*
 * FIXED (v4.2.2): Async DHCP timeout / retry configuration.
 * Timeouts are measured in poll ticks (each call to dhcp_poll() = 1 tick).
 * Assuming net_poll() is called roughly every ~10ms, 500 ticks ≈ 5 seconds.
 * The timeout doubles on each retry (exponential backoff per RFC 2131).
 */
#define DHCP_POLL_TIMEOUT_BASE  500
#define DHCP_MAX_RETRIES        3
#define DHCP_ERROR_RETRY_DELAY  2000   /* Wait ~20s before retrying after error */

/*
 * FIXED (v4.1.9): DHCP lease renewal support.
 * Tracks lease expiry time and automatically renews before expiry.
 * Without renewal, the IP becomes invalid after the lease period
 * (typically 24 hours).  (H-25: DHCP lease 24h no renewal)
 */
static uint64_t dhcp_lease_expiry = 0;   /* Tick-based expiry timestamp (perf.uptime_ticks) */
static uint32_t dhcp_lease_seconds = 0;  /* lease duration in seconds */
static int      dhcp_configured = 0;      /* 1 when DHCP ACK received */

/*
 * FIXED (v4.2.3): Use system tick counter (perf.uptime_ticks) instead
 * of raw TSC for lease expiry.  The TSC frequency varies across CPUs
 * (1-5 GHz), making the hardcoded 1 GHz estimate inaccurate by up to
 * 5x.  System ticks are calibrated to 100 Hz and provide a reliable
 * time source.  (BUG-NET-02)
 */
#define DHCP_TICKS_PER_SEC  100  /* System tick rate is 100 Hz */

/* ================================================================
 * dhcp_init
 * ================================================================ */
/*
 * FIXED (v4.1.7): Removed auto-run of dhcp_run() from dhcp_init().
 * Previously, dhcp_init() would call dhcp_run() internally if any
 * network interface was available, AND net_init() would also call
 * dhcp_run() explicitly.  This caused duplicate DHCP DISCOVER
 * packets to be sent on every boot.  (BUG N5)
 *
 * dhcp_init() now only initializes the DHCP client state.
 * The caller (net_init()) is responsible for calling dhcp_start()
 * to start the DHCP state machine.
 *
 * FIXED (v4.2.2): dhcp_init() now also resets the async state machine
 * to DHCP_IDLE so that dhcp_start() can be called safely.
 */
int dhcp_init(void) {
    /*
     * FIXED (v4.1.8): Use TSC-based random XID instead of fixed 0x12345678.
     * A fixed XID makes DHCP transactions easily forgeable.
     * (M-23: DHCP XID fixed, easy to forge)
     */
    uint32_t tsc_low, tsc_high;
    asm volatile ("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    dhcp_xid = (tsc_low ^ 0x12345678) & 0xFFFFFFFF;
    if (dhcp_xid == 0) dhcp_xid = 0x12345678;
    memset(dhcp_offered_ip, 0, 4);
    memset(dhcp_server_ip, 0, 4);
    dhcp_initialized = 1;

    /* FIXED (v4.2.2): Reset async state machine */
    dhcp_state = DHCP_IDLE;
    dhcp_retry_count = 0;
    dhcp_timeout_ticks = 0;

    log_printf(LOG_LEVEL_INFO, "dhcp: client initialized (xid=0x%08x)\n", dhcp_xid);
    return 0;
}

/* ================================================================
 * Build DHCP option TLV
 * Returns number of bytes written
 * ================================================================ */
static int dhcp_build_option(uint8_t *buf, uint8_t type, uint8_t len,
                              const void *value) {
    buf[0] = type;
    buf[1] = len;
    if (len > 0 && value) {
        memcpy(buf + 2, value, len);
    }
    return 2 + (int)len;
}

/* ================================================================
 * dhcp_send_discover - Send DHCP DISCOVER broadcast
 *
 * FIXED (v4.2.2): Renamed from dhcp_discover() to dhcp_send_discover().
 * This is now called from the async state machine in dhcp_poll().
 * ================================================================ */
static int dhcp_send_discover(void) {
    struct net_if *iface = net_get_interface(0);
    if (!iface) {
        log_printf(LOG_LEVEL_ERR, "dhcp: no network interface\n");
        return -1;
    }

    int pkt_len = (int)sizeof(struct dhcp_hdr) + 64;
    uint8_t *pkt = (uint8_t *)kmalloc((size_t)pkt_len);
    if (!pkt) return -1;

    memset(pkt, 0, (size_t)pkt_len);

    struct dhcp_hdr *dhcp = (struct dhcp_hdr *)pkt;
    dhcp->op = 1;           /* BOOTREQUEST */
    dhcp->htype = 1;        /* Ethernet */
    dhcp->hlen = 6;
    dhcp->hops = 0;
    dhcp->xid = htonl(dhcp_xid);
    dhcp->secs = 0;
    dhcp->flags = htons(0x8000);  /* Broadcast */
    memcpy(dhcp->chaddr, iface->mac, 6);
    dhcp->magic = htonl(DHCP_MAGIC_COOKIE);

    /* Build options */
    uint8_t *opt = pkt + sizeof(struct dhcp_hdr);
    int opt_len = 0;

    /* Option 53: DHCP Message Type = DISCOVER */
    uint8_t msg_type = DHCP_DISCOVER;
    opt_len += dhcp_build_option(opt + opt_len, DHCP_OPT_MSG_TYPE,
                                  1, &msg_type);

    /* Option 55: Parameter Request List */
    uint8_t params[] = { DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS };
    opt_len += dhcp_build_option(opt + opt_len, 55,
                                  (uint8_t)sizeof(params), params);

    /* Option 255: END */
    opt[opt_len++] = DHCP_OPT_END;

    /* Broadcast to 255.255.255.255:67 */
    uint8_t broadcast[4] = { 255, 255, 255, 255 };

    int ret = udp_send(DHCP_CLIENT_PORT, broadcast, DHCP_SERVER_PORT,
                       pkt, (uint16_t)(sizeof(struct dhcp_hdr) + opt_len));
    kfree(pkt);

    if (ret < 0) {
        log_printf(LOG_LEVEL_ERR, "dhcp: failed to send DISCOVER\n");
        return -1;
    }

    log_printf(LOG_LEVEL_INFO, "dhcp: DISCOVER sent (xid=0x%x)\n", dhcp_xid);
    return 0;
}

/* ================================================================
 * Find DHCP option in packet
 * ================================================================ */
static int dhcp_find_option(const uint8_t *pkt, int pkt_len,
                             uint8_t opt_type, uint8_t *out, int out_max) {
    int opt_off = (int)sizeof(struct dhcp_hdr);
    while (opt_off < pkt_len) {
        uint8_t type = pkt[opt_off];
        if (type == DHCP_OPT_END) break;
        if (type == 0) { opt_off++; continue; }  /* padding */
        if (opt_off + 1 >= pkt_len) break;
        uint8_t len = pkt[opt_off + 1];
        if (opt_off + 2 + len > pkt_len) break;
        if (type == opt_type) {
            int copy = (int)len;
            if (copy > out_max) copy = out_max;
            memcpy(out, pkt + opt_off + 2, (size_t)copy);
            return copy;
        }
        opt_off += 2 + len;
    }
    return -1;
}

/* ================================================================
 * dhcp_send_request - Send DHCP REQUEST
 *
 * FIXED (v4.2.2): Renamed from dhcp_request() to dhcp_send_request().
 * This is now called from the async state machine in dhcp_poll().
 * ================================================================ */
static int dhcp_send_request(void) {
    struct net_if *iface = net_get_interface(0);
    if (!iface) return -1;

    int pkt_len = (int)sizeof(struct dhcp_hdr) + 128;
    uint8_t *pkt = (uint8_t *)kmalloc((size_t)pkt_len);
    if (!pkt) return -1;

    memset(pkt, 0, (size_t)pkt_len);

    struct dhcp_hdr *dhcp = (struct dhcp_hdr *)pkt;
    dhcp->op = 1;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->hops = 0;
    dhcp->xid = htonl(dhcp_xid);
    dhcp->secs = 0;
    dhcp->flags = htons(0x8000);
    memcpy(dhcp->chaddr, iface->mac, 6);
    dhcp->magic = htonl(DHCP_MAGIC_COOKIE);

    uint8_t *opt = pkt + sizeof(struct dhcp_hdr);
    int opt_len = 0;

    /* Option 53: DHCP Message Type = REQUEST */
    uint8_t msg_type = DHCP_REQUEST;
    opt_len += dhcp_build_option(opt + opt_len, DHCP_OPT_MSG_TYPE,
                                  1, &msg_type);

    /* Option 50: Requested IP Address */
    opt_len += dhcp_build_option(opt + opt_len, DHCP_OPT_REQ_IP,
                                  4, dhcp_offered_ip);

    /* Option 54: DHCP Server Identifier */
    opt_len += dhcp_build_option(opt + opt_len, DHCP_OPT_SERVER_ID,
                                  4, dhcp_server_ip);

    /* Option 55: Parameter Request List */
    uint8_t params[] = { DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS };
    opt_len += dhcp_build_option(opt + opt_len, 55,
                                  (uint8_t)sizeof(params), params);

    /* Option 255: END */
    opt[opt_len++] = DHCP_OPT_END;

    uint8_t broadcast[4] = { 255, 255, 255, 255 };

    int ret = udp_send(DHCP_CLIENT_PORT, broadcast, DHCP_SERVER_PORT,
                       pkt, (uint16_t)(sizeof(struct dhcp_hdr) + opt_len));
    kfree(pkt);

    if (ret < 0) {
        log_printf(LOG_LEVEL_ERR, "dhcp: failed to send REQUEST\n");
        return -1;
    }

    log_printf(LOG_LEVEL_INFO, "dhcp: REQUEST sent\n");
    return 0;
}

/* ================================================================
 * dhcp_configure_interface - Apply DHCP ACK to network interface
 *
 * FIXED (v4.2.2): Extracted from old dhcp_handle_ack() into a
 * standalone helper.  Called from the async state machine when
 * an ACK is received in DHCP_WAIT_ACK state.
 * ================================================================ */
static void dhcp_configure_interface(const uint8_t *buf, int len) {
    struct dhcp_hdr *dhcp = (struct dhcp_hdr *)buf;
    struct net_if *iface = net_get_interface(0);
    if (!iface) return;

    /* Set IP address */
    memcpy(iface->ip, dhcp->yiaddr, 4);

    /* Set subnet mask */
    uint8_t netmask[4];
    if (dhcp_find_option(buf, len, DHCP_OPT_SUBNET_MASK,
                          netmask, 4) == 4) {
        memcpy(iface->netmask, netmask, 4);
    }

    /* Set gateway */
    uint8_t gateway[4];
    if (dhcp_find_option(buf, len, DHCP_OPT_ROUTER,
                          gateway, 4) == 4) {
        memcpy(iface->gateway, gateway, 4);
    }

    /* Set DNS server */
    uint8_t dns[4];
    if (dhcp_find_option(buf, len, DHCP_OPT_DNS, dns, 4) == 4) {
        memcpy(iface->dns_server, dns, 4);
    }

    /*
     * FIXED (v4.1.9): Extract lease time from DHCP ACK (option 51).
     * The lease time determines when the IP address expires and
     * needs renewal.  Default to 86400 seconds (24 hours) if not
     * specified.  (H-25: DHCP lease 24h no renewal)
     */
    {
        uint8_t lease_buf[4];
        if (dhcp_find_option(buf, len, DHCP_OPT_LEASE_TIME,
                              lease_buf, 4) == 4) {
            dhcp_lease_seconds = ((uint32_t)lease_buf[0] << 24) |
                                 ((uint32_t)lease_buf[1] << 16) |
                                 ((uint32_t)lease_buf[2] << 8)  |
                                 (uint32_t)lease_buf[3];
        } else {
            dhcp_lease_seconds = 86400;  /* Default: 24 hours */
        }

        /* Set expiry: current ticks + lease time in ticks.
         * Renew at 50% of lease time (T1) per RFC 2131. */
        uint64_t renew_ticks = (uint64_t)dhcp_lease_seconds *
                               (uint64_t)DHCP_TICKS_PER_SEC / 2;
        dhcp_lease_expiry = perf.uptime_ticks + renew_ticks;
        dhcp_configured = 1;

        log_printf(LOG_LEVEL_INFO,
                   "dhcp: lease %u seconds, renew in %u seconds\n",
                   dhcp_lease_seconds, dhcp_lease_seconds / 2);
    }

    log_printf(LOG_LEVEL_INFO,
               "dhcp: configured ip=%d.%d.%d.%d "
               "mask=%d.%d.%d.%d gw=%d.%d.%d.%d dns=%d.%d.%d.%d\n",
               iface->ip[0], iface->ip[1], iface->ip[2], iface->ip[3],
               iface->netmask[0], iface->netmask[1],
               iface->netmask[2], iface->netmask[3],
               iface->gateway[0], iface->gateway[1],
               iface->gateway[2], iface->gateway[3],
               iface->dns_server[0], iface->dns_server[1],
               iface->dns_server[2], iface->dns_server[3]);
}

/* ================================================================
 * dhcp_get_timeout - Calculate current state timeout in poll ticks
 *
 * FIXED (v4.2.2): Exponential backoff timeout.  Each retry doubles
 * the timeout period, starting from DHCP_POLL_TIMEOUT_BASE.
 * ================================================================ */
static int dhcp_get_timeout(void) {
    int timeout = DHCP_POLL_TIMEOUT_BASE;
    if (dhcp_retry_count > 0) {
        timeout = DHCP_POLL_TIMEOUT_BASE * (1 << dhcp_retry_count);
    }
    /* Cap at a reasonable maximum (~64 seconds) */
    if (timeout > 6400) timeout = 6400;
    return timeout;
}

/* ================================================================
 * dhcp_start - Initiate async DHCP discovery
 *
 * FIXED (v4.2.2): Replaces the old synchronous dhcp_run().  This
 * sends the initial DISCOVER and transitions to DHCP_DISCOVER_SENT.
 * The dhcp_poll() function (called from net_poll()) advances the
 * state machine from there without blocking.
 *
 * Call this from net_init() after dhcp_init().
 * ================================================================ */
void dhcp_start(void) {
    if (!dhcp_initialized) {
        dhcp_init();
    }

    if (dhcp_state != DHCP_IDLE && dhcp_state != DHCP_ERROR) {
        log_printf(LOG_LEVEL_DEBUG, "dhcp: already running (state=%d)\n",
                   dhcp_state);
        return;
    }

    log_printf(LOG_LEVEL_INFO, "dhcp: starting async discovery\n");

    if (dhcp_send_discover() < 0) {
        dhcp_state = DHCP_ERROR;
        dhcp_retry_count = 0;
        dhcp_timeout_ticks = 0;
        return;
    }

    dhcp_state = DHCP_DISCOVER_SENT;
    dhcp_retry_count = 0;
    dhcp_timeout_ticks = 0;
}

/* ================================================================
 * dhcp_try_receive_offer - Non-blocking check for DHCP OFFER
 *
 * FIXED (v4.2.2): Replaces the old blocking dhcp_handle_offer().
 * Checks udp_recvfrom() once without looping.  If an OFFER is
 * found, stores the offered IP and server IP, returns 0.
 * Returns -1 if no OFFER is available yet.
 * ================================================================ */
static int dhcp_try_receive_offer(void) {
    uint8_t buf[600];
    uint8_t src_ip[4];
    uint16_t src_port;

    int len = udp_recvfrom(DHCP_CLIENT_PORT, buf, (int)sizeof(buf),
                            src_ip, &src_port);
    if (len <= 0) return -1;
    if (len < (int)sizeof(struct dhcp_hdr)) return -1;

    struct dhcp_hdr *dhcp = (struct dhcp_hdr *)buf;
    if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE) return -1;
    if (ntohl(dhcp->xid) != dhcp_xid) return -1;
    if (ntohs(src_port) != DHCP_SERVER_PORT) return -1;

    /* Check DHCP message type */
    uint8_t msg_type = 0;
    if (dhcp_find_option(buf, len, DHCP_OPT_MSG_TYPE,
                          &msg_type, 1) != 1) return -1;
    if (msg_type != DHCP_OFFER) return -1;

    /* Extract offered IP */
    memcpy(dhcp_offered_ip, dhcp->yiaddr, 4);

    /* Extract server IP */
    if (dhcp_find_option(buf, len, DHCP_OPT_SERVER_ID,
                          dhcp_server_ip, 4) <= 0) {
        memcpy(dhcp_server_ip, src_ip, 4);
    }

    log_printf(LOG_LEVEL_INFO,
               "dhcp: OFFER received, ip=%d.%d.%d.%d server=%d.%d.%d.%d\n",
               dhcp_offered_ip[0], dhcp_offered_ip[1],
               dhcp_offered_ip[2], dhcp_offered_ip[3],
               dhcp_server_ip[0], dhcp_server_ip[1],
               dhcp_server_ip[2], dhcp_server_ip[3]);
    return 0;
}

/* ================================================================
 * dhcp_try_receive_ack - Non-blocking check for DHCP ACK/NAK
 *
 * FIXED (v4.2.2): Replaces the old blocking dhcp_handle_ack().
 * Checks udp_recvfrom() once without looping.  Returns 0 for ACK,
 * 1 for NAK, -1 if no packet available.
 * ================================================================ */
static int dhcp_try_receive_ack(uint8_t *buf_out, int buf_size, int *len_out) {
    uint8_t src_ip[4];
    uint16_t src_port;

    int len = udp_recvfrom(DHCP_CLIENT_PORT, buf_out, buf_size,
                            src_ip, &src_port);
    if (len <= 0) return -1;
    if (len < (int)sizeof(struct dhcp_hdr)) return -1;

    struct dhcp_hdr *dhcp = (struct dhcp_hdr *)buf_out;
    if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE) return -1;
    if (ntohl(dhcp->xid) != dhcp_xid) return -1;
    if (ntohs(src_port) != DHCP_SERVER_PORT) return -1;

    uint8_t msg_type = 0;
    if (dhcp_find_option(buf_out, len, DHCP_OPT_MSG_TYPE,
                          &msg_type, 1) != 1) return -1;

    if (msg_type == DHCP_NAK) {
        log_printf(LOG_LEVEL_ERR, "dhcp: NAK received\n");
        return 1;
    }

    if (msg_type != DHCP_ACK) return -1;
    *len_out = len;
    return 0;
}

/* ================================================================
 * dhcp_poll - Periodic DHCP state machine tick
 *
 * FIXED (v4.2.2): Completely rewritten as an async state machine.
 * Called from net_poll() in the main loop.  Each call is non-blocking
 * and advances the DHCP state machine by one tick.
 *
 * States:
 *   DHCP_IDLE           - Nothing to do
 *   DHCP_DISCOVER_SENT  - Transition to DHCP_WAIT_OFFER
 *   DHCP_WAIT_OFFER     - Check for OFFER, handle timeout/retry
 *   DHCP_REQUEST_SENT   - Transition to DHCP_WAIT_ACK
 *   DHCP_WAIT_ACK       - Check for ACK/NAK, handle timeout/retry
 *   DHCP_BOUND          - Check lease expiry, trigger renewal
 *   DHCP_ERROR          - Wait then retry from start
 *
 * Also handles the existing v4.1.9 lease renewal check when in
 * DHCP_BOUND state.
 * ================================================================ */
void dhcp_poll(void) {
    /*
     * FIXED (v4.1.9): Periodic DHCP lease renewal check.
     * Moved inside the DHCP_BOUND state handler for clarity.
     * The old standalone check is now part of the state machine.
     */
    if (dhcp_state == DHCP_BOUND) {
        if (dhcp_configured && dhcp_lease_expiry != 0) {
            if (perf.uptime_ticks >= dhcp_lease_expiry) {
                log_printf(LOG_LEVEL_INFO, "dhcp: lease renewal time reached, "
                           "restarting discovery\n");
                dhcp_lease_expiry = 0;
                dhcp_configured = 0;
                /* Fall through to restart DHCP from IDLE */
                dhcp_state = DHCP_IDLE;
                dhcp_retry_count = 0;
                dhcp_timeout_ticks = 0;
            }
        }
    }

    switch (dhcp_state) {
    case DHCP_IDLE:
        /* Nothing to do */
        break;

    /*
     * FIXED (v4.2.2): DHCP_DISCOVER_SENT - DISCOVER was just sent.
     * Transition to DHCP_WAIT_OFFER to begin receiving.
     */
    case DHCP_DISCOVER_SENT:
        dhcp_state = DHCP_WAIT_OFFER;
        dhcp_timeout_ticks = 0;
        break;

    /*
     * FIXED (v4.2.2): DHCP_WAIT_OFFER - Non-blocking wait for OFFER.
     * If OFFER received, send REQUEST and transition to DHCP_REQUEST_SENT.
     * If timeout, retry DISCOVER (up to DHCP_MAX_RETRIES).
     */
    case DHCP_WAIT_OFFER: {
        int timeout = dhcp_get_timeout();

        if (dhcp_try_receive_offer() == 0) {
            /* OFFER received - send REQUEST */
            if (dhcp_send_request() < 0) {
                dhcp_state = DHCP_ERROR;
                dhcp_retry_count = 0;
                dhcp_timeout_ticks = 0;
                break;
            }
            dhcp_state = DHCP_REQUEST_SENT;
            dhcp_retry_count = 0;
            dhcp_timeout_ticks = 0;
            break;
        }

        dhcp_timeout_ticks++;
        if (dhcp_timeout_ticks >= timeout) {
            if (dhcp_retry_count < DHCP_MAX_RETRIES) {
                dhcp_retry_count++;
                log_printf(LOG_LEVEL_INFO,
                           "dhcp: OFFER timeout, retry %d/%d\n",
                           dhcp_retry_count, DHCP_MAX_RETRIES);
                if (dhcp_send_discover() < 0) {
                    dhcp_state = DHCP_ERROR;
                    dhcp_retry_count = 0;
                    dhcp_timeout_ticks = 0;
                    break;
                }
                dhcp_state = DHCP_DISCOVER_SENT;
                dhcp_timeout_ticks = 0;
            } else {
                log_printf(LOG_LEVEL_ERR, "dhcp: no OFFER after %d retries\n",
                           DHCP_MAX_RETRIES);
                dhcp_state = DHCP_ERROR;
                dhcp_retry_count = 0;
                dhcp_timeout_ticks = 0;
            }
        }
        break;
    }

    /*
     * FIXED (v4.2.2): DHCP_REQUEST_SENT - REQUEST was just sent.
     * Transition to DHCP_WAIT_ACK to begin receiving.
     */
    case DHCP_REQUEST_SENT:
        dhcp_state = DHCP_WAIT_ACK;
        dhcp_timeout_ticks = 0;
        break;

    /*
     * FIXED (v4.2.2): DHCP_WAIT_ACK - Non-blocking wait for ACK/NAK.
     * If ACK received, configure interface and transition to DHCP_BOUND.
     * If NAK received, transition to DHCP_ERROR.
     * If timeout, retry REQUEST (up to DHCP_MAX_RETRIES).
     */
    case DHCP_WAIT_ACK: {
        int timeout = dhcp_get_timeout();
        uint8_t buf[600];
        int ack_len = 0;

        int result = dhcp_try_receive_ack(buf, (int)sizeof(buf), &ack_len);
        if (result == 0) {
            /* ACK received */
            dhcp_configure_interface(buf, ack_len);
            dhcp_state = DHCP_BOUND;
            dhcp_retry_count = 0;
            dhcp_timeout_ticks = 0;
            log_printf(LOG_LEVEL_INFO, "dhcp: configuration complete\n");
            break;
        } else if (result == 1) {
            /* NAK received */
            dhcp_state = DHCP_ERROR;
            dhcp_retry_count = 0;
            dhcp_timeout_ticks = 0;
            break;
        }

        dhcp_timeout_ticks++;
        if (dhcp_timeout_ticks >= timeout) {
            if (dhcp_retry_count < DHCP_MAX_RETRIES) {
                dhcp_retry_count++;
                log_printf(LOG_LEVEL_INFO,
                           "dhcp: ACK timeout, retry %d/%d\n",
                           dhcp_retry_count, DHCP_MAX_RETRIES);
                if (dhcp_send_request() < 0) {
                    dhcp_state = DHCP_ERROR;
                    dhcp_retry_count = 0;
                    dhcp_timeout_ticks = 0;
                    break;
                }
                dhcp_state = DHCP_REQUEST_SENT;
                dhcp_timeout_ticks = 0;
            } else {
                log_printf(LOG_LEVEL_ERR, "dhcp: no ACK after %d retries\n",
                           DHCP_MAX_RETRIES);
                dhcp_state = DHCP_ERROR;
                dhcp_retry_count = 0;
                dhcp_timeout_ticks = 0;
            }
        }
        break;
    }

    case DHCP_BOUND:
        /* Lease renewal is checked at the top of dhcp_poll() */
        break;

    /*
     * FIXED (v4.2.2): DHCP_ERROR - Wait for a delay, then retry
     * from the beginning.
     */
    case DHCP_ERROR:
        dhcp_timeout_ticks++;
        if (dhcp_timeout_ticks >= DHCP_ERROR_RETRY_DELAY) {
            log_printf(LOG_LEVEL_INFO, "dhcp: retrying after error\n");
            /* Reset state and start fresh */
            dhcp_state = DHCP_IDLE;
            dhcp_retry_count = 0;
            dhcp_timeout_ticks = 0;
            dhcp_configured = 0;
            dhcp_lease_expiry = 0;
            dhcp_start();
        }
        break;
    }
}