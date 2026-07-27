/*
 * net.c - TCP/IP Network Protocol Stack Implementation
 *
 * Implements Ethernet, ARP, IPv4, ICMP, UDP, and TCP layers.
 * Integrates with the VirtIO netdev driver layer.
 */
#include "net.h"
#include "log.h"
#include "string.h"
#include "../netdev.h"
#include "../smp.h"
#include "../mem.h"
#include "unix.h"        /* AF_UNIX (v4.2.6) */
#include <stdint.h>

/* ================================================================
 * Byte Order Conversion
 * ================================================================ */
static inline uint16_t ntohs(uint16_t n) {
    return ((n & 0xFF) << 8) | ((n & 0xFF00) >> 8);
}

/* Forward declarations for protocol handlers */
static void icmp_handle_packet(struct net_device *netdev,
                               const uint8_t src_ip[4],
                               const uint8_t *data, int len);
static void udp_handle_packet(const uint8_t src_ip[4],
                              const uint8_t dst_ip[4],
                              const uint8_t *data, int len);
static void tcp_handle_packet(const uint8_t src_ip[4],
                              const uint8_t dst_ip[4],
                              const uint8_t *data, int len);

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

/* ================================================================
 * Checksum Calculation (RFC 1071)
 * ================================================================ */
static uint16_t checksum_calc(const void *data, int len) {
    uint32_t sum = 0;
    const uint16_t *ptr = (const uint16_t *)data;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    /* FIXED (v4.2.7): BUG-TCP-CSUM-ODD — The remaining odd byte must
     * be treated as the high byte of a 16-bit word with a zero low byte
     * (RFC 1071).  Previously it was treated as the low byte, yielding
     * incorrect checksums for odd-length data. */
    if (len > 0) {
        sum += ((uint16_t)*(const uint8_t *)ptr) << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

static uint16_t ip_checksum(const void *data, int len) {
    return checksum_calc(data, len);
}

static uint16_t tcp_udp_checksum(const uint8_t src_ip[4],
                                  const uint8_t dst_ip[4],
                                  uint8_t protocol,
                                  const void *data, int len) {
    /* Pseudo-header */
    struct {
        uint8_t  src_ip[4];
        uint8_t  dst_ip[4];
        uint8_t  zero;
        uint8_t  protocol;
        uint16_t length;
    } __attribute__((packed)) pseudo;

    memcpy(pseudo.src_ip, src_ip, 4);
    memcpy(pseudo.dst_ip, dst_ip, 4);
    pseudo.zero = 0;
    pseudo.protocol = protocol;
    pseudo.length = htons((uint16_t)len);

    uint32_t sum = 0;
    const uint16_t *ptr = (const uint16_t *)&pseudo;
    int i;
    for (i = 0; i < (int)sizeof(pseudo) / 2; i++) {
        sum += *ptr++;
    }

    ptr = (const uint16_t *)data;
    int remaining = len;
    while (remaining > 1) {
        sum += *ptr++;
        remaining -= 2;
    }
    if (remaining > 0) {
        /* FIXED (v4.2.7): BUG-TCP-CSUM-ODD — Same odd-byte padding
         * fix as in checksum_calc above. */
        sum += ((uint16_t)*(const uint8_t *)ptr) << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

/* ================================================================
 * Network Interfaces
 * ================================================================ */
#define MAX_NET_IF 8
static struct net_if net_ifs[MAX_NET_IF];
static int net_if_count = 0;

struct net_if *net_get_interface(int index) {
    if (index < 0 || index >= net_if_count) return NULL;
    return &net_ifs[index];
}

int net_get_interface_count(void) {
    return net_if_count;
}

static struct net_if *net_if_find_by_ip(const uint8_t ip[4]) {
    int i;
    for (i = 0; i < net_if_count; i++) {
        if (memcmp(net_ifs[i].ip, ip, 4) == 0) {
            return &net_ifs[i];
        }
    }
    /* If no exact match, return the first UP interface on the same subnet */
    for (i = 0; i < net_if_count; i++) {
        if ((net_ifs[i].flags & NETIF_FLAG_UP) && net_ifs[i].netdev) {
            return &net_ifs[i];
        }
    }
    return NULL;
}

/* ================================================================
 * Ethernet Layer
 * ================================================================ */
static int eth_send(struct net_device *netdev,
                    const uint8_t dst_mac[6],
                    uint16_t ethertype,
                    const void *data, int len) {
    if (!netdev || !data || len <= 0) return -1;

    int total = (int)sizeof(struct eth_hdr) + len;
    if (total > netdev->mtu + 14) return -1;

    uint8_t *frame = (uint8_t *)kmalloc((size_t)total);
    if (!frame) return -1;

    struct eth_hdr *eth = (struct eth_hdr *)frame;
    memcpy(eth->dst_mac, dst_mac, 6);
    memcpy(eth->src_mac, netdev->mac, 6);
    eth->ethertype = htons(ethertype);

    memcpy(frame + sizeof(struct eth_hdr), data, (size_t)len);

    int ret = netdev_send(netdev, frame, total);
    kfree(frame);
    return ret;
}

static int eth_recv(struct net_device *netdev, void *buf, int max_len) {
    if (!netdev || !buf || max_len < (int)sizeof(struct eth_hdr)) return -1;
    return netdev_recv(netdev, buf, max_len);
}

/* ================================================================
 * ARP Cache
 * ================================================================ */
#define ARP_CACHE_SIZE 16
struct arp_entry {
    uint8_t ip[4];
    uint8_t mac[6];
    int     age;
    int     valid;
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static int arp_age_counter = 0;
static spinlock_t arp_lock;  /* FIXED (v4.2.1): protect ARP cache (BUG-NET-M6) */

/*
 * FIXED (v4.2.5): BUG-ARP-TOCTOU — arp_cache_find now copies the MAC
 * address into the caller's buffer while holding arp_lock, instead of
 * returning a pointer to the internal cache entry after releasing the
 * lock.  This prevents a TOCTOU race where the entry could be evicted
 * or modified between the lookup and the caller's use of the pointer.
 * Returns 0 on success (MAC copied), -1 if not found.
 */
static int arp_cache_find(const uint8_t ip[4], uint8_t mac_out[6]) {
    int i;
    spin_lock(&arp_lock);
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            arp_cache[i].age = ++arp_age_counter;
            memcpy(mac_out, arp_cache[i].mac, 6);
            spin_unlock(&arp_lock);
            return 0;
        }
    }
    spin_unlock(&arp_lock);
    return -1;
}

/*
 * FIXED (v4.2.9): BUG-ARP-POINTER — arp_cache_add no longer returns a
 * pointer to an internal cache entry after releasing the lock.  The
 * pointer was already unused by the caller, but the signature invited
 * use-after-free bugs.  Changed to void return.
 */
static void arp_cache_add(const uint8_t ip[4],
                                        const uint8_t mac[6]) {
    /* Find an empty or oldest entry */
    int target = 0;
    int oldest_age = arp_cache[0].age;
    int i;

    /* FIXED (v4.2.1): Protect with arp_lock (BUG-NET-M6) */
    spin_lock(&arp_lock);
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            target = i;
            break;
        }
        if (arp_cache[i].age < oldest_age) {
            oldest_age = arp_cache[i].age;
            target = i;
        }
    }

    memcpy(arp_cache[target].ip, ip, 4);
    memcpy(arp_cache[target].mac, mac, 6);
    arp_cache[target].age = ++arp_age_counter;
    arp_cache[target].valid = 1;
    spin_unlock(&arp_lock);
}

/*
 * FIXED (v4.1.8): Age out ARP cache entries that haven't been refreshed.
 * Entries older than ARP_CACHE_AGE_THRESHOLD are invalidated.
 * (H-20: ARP cache no aging, stale MACs persist forever)
 */
#define ARP_CACHE_AGE_THRESHOLD 300  /* ~5 minutes at 1 poll/sec */

static void arp_cache_age(void) {
    int i;
    /* FIXED (v4.2.1): Protect with arp_lock (BUG-NET-M6) */
    spin_lock(&arp_lock);
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid &&
            (arp_age_counter - arp_cache[i].age) > ARP_CACHE_AGE_THRESHOLD) {
            arp_cache[i].valid = 0;
        }
    }
    spin_unlock(&arp_lock);
}

static void arp_send_request(struct net_if *iface, const uint8_t target_ip[4]) {
    uint8_t packet[sizeof(struct eth_hdr) + sizeof(struct arp_hdr)];
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    struct eth_hdr *eth = (struct eth_hdr *)packet;
    memcpy(eth->dst_mac, broadcast_mac, 6);
    memcpy(eth->src_mac, iface->mac, 6);
    eth->ethertype = htons(ETH_ARP);

    struct arp_hdr *arp = (struct arp_hdr *)(packet + sizeof(struct eth_hdr));
    arp->htype = htons(ARP_HTYPE_ETH);
    arp->ptype = htons(ETH_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(ARP_REQUEST);
    memcpy(arp->sha, iface->mac, 6);
    memcpy(arp->spa, iface->ip, 4);
    memset(arp->tha, 0, 6);
    memcpy(arp->tpa, target_ip, 4);

    netdev_send(iface->netdev, packet, sizeof(packet));
}

int arp_lookup(const uint8_t ip[4], uint8_t mac_out[6]) {
    /* FIXED (v4.2.5): BUG-ARP-TOCTOU — arp_cache_find now copies the MAC
     * atomically while holding arp_lock, eliminating the TOCTOU race. */
    if (arp_cache_find(ip, mac_out) == 0) {
        return 0;
    }

    /* Send ARP request on all UP interfaces */
    int i;
    for (i = 0; i < net_if_count; i++) {
        if ((net_ifs[i].flags & NETIF_FLAG_UP) && net_ifs[i].netdev) {
            arp_send_request(&net_ifs[i], ip);
        }
    }

    return -1;
}

static void arp_handle_packet(const uint8_t *data, int len) {
    if (len < (int)sizeof(struct arp_hdr)) return;

    const struct arp_hdr *arp = (const struct arp_hdr *)data;

    if (ntohs(arp->htype) != ARP_HTYPE_ETH) return;
    if (ntohs(arp->ptype) != ETH_IPV4) return;
    if (arp->hlen != 6 || arp->plen != 4) return;

    uint16_t oper = ntohs(arp->oper);

    if (oper == ARP_REQUEST) {
        /* Check if this ARP request is for one of our IPs */
        struct net_if *iface = net_if_find_by_ip(arp->tpa);
        if (!iface || !iface->netdev) return;

        /* Send ARP reply */
        uint8_t packet[sizeof(struct eth_hdr) + sizeof(struct arp_hdr)];

        struct eth_hdr *eth = (struct eth_hdr *)packet;
        memcpy(eth->dst_mac, arp->sha, 6);
        memcpy(eth->src_mac, iface->mac, 6);
        eth->ethertype = htons(ETH_ARP);

        struct arp_hdr *reply = (struct arp_hdr *)(packet + sizeof(struct eth_hdr));
        reply->htype = htons(ARP_HTYPE_ETH);
        reply->ptype = htons(ETH_IPV4);
        reply->hlen = 6;
        reply->plen = 4;
        reply->oper = htons(ARP_REPLY);
        memcpy(reply->sha, iface->mac, 6);
        memcpy(reply->spa, iface->ip, 4);
        memcpy(reply->tha, arp->sha, 6);
        memcpy(reply->tpa, arp->spa, 4);

        netdev_send(iface->netdev, packet, sizeof(packet));
    } else if (oper == ARP_REPLY) {
        /* Cache the mapping */
        arp_cache_add(arp->spa, arp->sha);
    }
}

/* ================================================================
 * IPv4 Layer
 * ================================================================ */
/*
 * FIXED (v4.1.8): Use atomic increment for IP ID to prevent
 * duplicate IDs in SMP environments.  (L-12: IP ID非原子递增)
 */
static uint16_t ip_id_counter = 0;

static inline uint16_t ip_id_atomic_inc(void) {
    /* Atomic fetch-and-increment for SMP safety.  (L-12) */
    return (uint16_t)__atomic_fetch_add(&ip_id_counter, 1, __ATOMIC_RELAXED);
}

int ip_send(const uint8_t dst_ip[4], uint8_t protocol,
            const void *data, uint16_t len) {
    if (!data && len > 0) return -1;
    struct net_if *iface = net_if_find_by_ip(dst_ip);
    if (!iface) {
        /* Use first available interface */
        if (net_if_count > 0) {
            iface = &net_ifs[0];
        } else {
            return -1;
        }
    }
    if (!iface->netdev) return -1;

    /* Resolve destination MAC via ARP */
    uint8_t dst_mac[6];
    if (arp_lookup(dst_ip, dst_mac) != 0) {
        /*
         * FIXED (v4.1.8): Do NOT send to broadcast MAC when ARP
         * resolution fails. Broadcast leaks the packet to all hosts
         * on the local network segment, which is a security risk.
         * ARP request was already sent by arp_lookup(); the caller
         * should retry after the ARP reply arrives.
         * (C-9: arp_lookup failure sends broadcast MAC)
         */
        return -1;
    }

    /*
     * FIXED (v4.2.5): BUG-IP-OVERFLOW — When len is near uint16_t max,
     * total = 20 + len could overflow 65535, causing truncation to
     * 65535 but then memcpy(packet+20, data, len) writes past the
     * allocated buffer.  Truncate len first, not total.
     */
    if (len > 65535 - sizeof(struct ipv4_hdr)) {
        len = 65535 - (uint16_t)sizeof(struct ipv4_hdr);
    }
    int total = (int)(sizeof(struct ipv4_hdr) + len);
    uint8_t *packet = (uint8_t *)kmalloc((size_t)total);
    if (!packet) return -1;

    struct ipv4_hdr *ip = (struct ipv4_hdr *)packet;
    ip->version_ihl = 0x45; /* Version 4, IHL = 5 (20 bytes) */
    ip->dscp_ecn = 0;
    ip->total_len = htons((uint16_t)total);
    ip->id = htons(ip_id_atomic_inc());
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    memcpy(ip->src_ip, iface->ip, 4);
    memcpy(ip->dst_ip, dst_ip, 4);

    ip->checksum = ip_checksum(ip, (int)sizeof(struct ipv4_hdr));

    memcpy(packet + sizeof(struct ipv4_hdr), data, len);

    int ret = eth_send(iface->netdev, dst_mac, ETH_IPV4, packet, total);
    kfree(packet);
    return ret;
}

static void ip_handle_packet(struct net_device *netdev,
                              const uint8_t *data, int len) {
    if (len < (int)sizeof(struct ipv4_hdr)) return;

    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)data;
    int ihl = (ip->version_ihl & 0x0F) * 4;
    if (ihl < (int)sizeof(struct ipv4_hdr) || ihl > len) return;

    uint16_t total_len = ntohs(ip->total_len);
    if (total_len < (uint16_t)ihl || total_len > (uint16_t)len) return;

    /* Verify checksum */
    uint16_t csum = ip_checksum(ip, ihl);
    if (csum != 0 && csum != 0xFFFF) return;

    /* Check if packet is for us */
    int i;
    int for_us = 0;
    for (i = 0; i < net_if_count; i++) {
        if (memcmp(net_ifs[i].ip, ip->dst_ip, 4) == 0) {
            for_us = 1;
            break;
        }
    }
    if (!for_us) return;

    const uint8_t *payload = data + ihl;
    int payload_len = (int)total_len - ihl;

    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        icmp_handle_packet(netdev, ip->src_ip, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        udp_handle_packet(ip->src_ip, ip->dst_ip, payload, payload_len);
        break;
    case IP_PROTO_TCP:
        tcp_handle_packet(ip->src_ip, ip->dst_ip, payload, payload_len);
        break;
    default:
        break;
    }
}

/* ================================================================
 * ICMP Layer
 * ================================================================ */
static void icmp_handle_packet(struct net_device *netdev,
                                const uint8_t src_ip[4],
                                const uint8_t *data, int len) {
    (void)netdev;
    if (len < (int)sizeof(struct icmp_hdr)) return;

    const struct icmp_hdr *icmp = (const struct icmp_hdr *)data;

    /* Verify checksum */
    uint16_t csum = checksum_calc(data, len);
    if (csum != 0 && csum != 0xFFFF) return;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        /* Send echo reply */
        int reply_len = len;
        uint8_t *reply = (uint8_t *)kmalloc((size_t)reply_len);
        if (!reply) return;

        memcpy(reply, data, (size_t)len);

        struct icmp_hdr *reply_icmp = (struct icmp_hdr *)reply;
        reply_icmp->type = ICMP_ECHO_REPLY;
        reply_icmp->code = 0;
        reply_icmp->checksum = 0;
        reply_icmp->checksum = checksum_calc(reply, reply_len);

        ip_send(src_ip, IP_PROTO_ICMP, reply, (uint16_t)reply_len);
        kfree(reply);
    }
}

/* ================================================================
 * UDP Layer
 * ================================================================ */
#define MAX_UDP_SOCKETS 16
#define UDP_RX_QUEUE_SIZE 8   /* FIXED (v4.2.2): circular packet queue */
#define UDP_RX_BUF_SIZE 2048

/* FIXED (v4.2.2): Per-packet structure for circular queue.
 * Each slot holds one received UDP datagram with source address. */
struct udp_pkt {
    uint8_t  data[UDP_RX_BUF_SIZE];
    int      len;
    uint8_t  src_ip[4];
    uint16_t src_port;
    int      valid;
};

struct udp_socket {
    uint16_t local_port;
    int      in_use;
    /* FIXED (v4.2.2): Circular packet queue replaces single rx_buf.
     * rx_head = next to dequeue, rx_tail = next to enqueue,
     * rx_count = number of packets waiting. */
    struct udp_pkt rx_queue[UDP_RX_QUEUE_SIZE];
    int      rx_head;
    int      rx_tail;
    int      rx_count;
};

static struct udp_socket udp_sockets[MAX_UDP_SOCKETS];
static spinlock_t udp_lock;

int udp_send(uint16_t src_port, const uint8_t dst_ip[4],
             uint16_t dst_port, const void *data, uint16_t len) {
    struct net_if *iface = net_if_find_by_ip(dst_ip);
    if (!iface || !iface->netdev) return -1;

    int total = (int)(sizeof(struct udp_hdr) + len);
    uint8_t *packet = (uint8_t *)kmalloc((size_t)total);
    if (!packet) return -1;

    struct udp_hdr *udp = (struct udp_hdr *)packet;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons((uint16_t)total);
    udp->checksum = 0;

    memcpy(packet + sizeof(struct udp_hdr), data, len);

    udp->checksum = tcp_udp_checksum(iface->ip, dst_ip, IP_PROTO_UDP,
                                      packet, total);

    int ret = ip_send(dst_ip, IP_PROTO_UDP, packet, (uint16_t)total);
    kfree(packet);
    return ret;
}

static void udp_handle_packet(const uint8_t src_ip[4],
                               const uint8_t dst_ip[4],
                               const uint8_t *data, int len) {
    if (len < (int)sizeof(struct udp_hdr)) return;

    const struct udp_hdr *udp = (const struct udp_hdr *)data;

    /*
     * FIXED (v4.1.8): Validate UDP checksum before accepting the packet.
     * A zero checksum means no checksum was computed (allowed in IPv4).
     * (H-19: UDP checksum not validated)
     */
    if (udp->checksum != 0) {
        uint16_t csum = tcp_udp_checksum(src_ip, dst_ip, IP_PROTO_UDP, data, len);
        if (csum != 0 && csum != 0xFFFF) {
            return;  /* Checksum mismatch, drop silently */
        }
    }

    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    int data_len = len - (int)sizeof(struct udp_hdr);

    spin_lock(&udp_lock);

    int i;
    for (i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_sockets[i].in_use && udp_sockets[i].local_port == dst_port) {
            struct udp_socket *sock = &udp_sockets[i];

            /* FIXED (v4.2.2): Enqueue into circular packet queue.
             * If the queue is full, drop the oldest packet (head)
             * to make room for the new one. */
            if (sock->rx_count >= UDP_RX_QUEUE_SIZE) {
                /* Drop oldest: advance head, marking slot as free */
                sock->rx_queue[sock->rx_head].valid = 0;
                sock->rx_head = (sock->rx_head + 1) % UDP_RX_QUEUE_SIZE;
                sock->rx_count--;
            }

            /* Write new packet at tail position */
            int copy_len = data_len;
            if (copy_len > (int)sizeof(sock->rx_queue[0].data)) {
                copy_len = (int)sizeof(sock->rx_queue[0].data);
            }
            if (copy_len > 0) {
                memcpy(sock->rx_queue[sock->rx_tail].data,
                       data + sizeof(struct udp_hdr), (size_t)copy_len);
            }
            sock->rx_queue[sock->rx_tail].len = copy_len;
            memcpy(sock->rx_queue[sock->rx_tail].src_ip, src_ip, 4);
            sock->rx_queue[sock->rx_tail].src_port = src_port;
            sock->rx_queue[sock->rx_tail].valid = 1;

            sock->rx_tail = (sock->rx_tail + 1) % UDP_RX_QUEUE_SIZE;
            sock->rx_count++;
            break;
        }
    }

    spin_unlock(&udp_lock);
}

int udp_recvfrom(uint16_t port, void *buf, int max_len,
                  uint8_t src_ip[4], uint16_t *src_port) {
    if (!buf || max_len < 0) return -1;

    spin_lock(&udp_lock);

    int i;
    for (i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_sockets[i].in_use && udp_sockets[i].local_port == port &&
            udp_sockets[i].rx_count > 0) {
            struct udp_socket *sock = &udp_sockets[i];

            /* FIXED (v4.2.2): Dequeue from circular packet queue.
             * Read the oldest packet at rx_head, then advance head. */
            struct udp_pkt *pkt = &sock->rx_queue[sock->rx_head];

            int copy_len = pkt->len;
            if (copy_len > max_len) copy_len = max_len;
            memcpy(buf, pkt->data, (size_t)copy_len);
            if (src_ip) memcpy(src_ip, pkt->src_ip, 4);
            if (src_port) *src_port = pkt->src_port;

            /* Mark slot as consumed and advance head */
            pkt->valid = 0;
            sock->rx_head = (sock->rx_head + 1) % UDP_RX_QUEUE_SIZE;
            sock->rx_count--;

            spin_unlock(&udp_lock);
            return copy_len;
        }
    }

    spin_unlock(&udp_lock);
    return -1;  /* No data available */
}

/* ================================================================
 * TCP Layer
 * ================================================================ */
#define MAX_TCP_SOCKETS 16
#define TCP_RX_BUF_SIZE 4096
#define TCP_MAX_SEGMENT 1460
#define TCP_BACKLOG_MAX 8

struct tcp_socket {
    int      id;
    int      in_use;
    int      state;
    uint8_t  local_ip[4];
    uint16_t local_port;
    uint8_t  remote_ip[4];
    uint16_t remote_port;
    uint32_t isn;          /* initial sequence number */
    uint32_t seq_num;      /* next sequence number to send */
    uint32_t ack_num;      /* next expected sequence number */
    uint32_t rcv_nxt;      /* next sequence number expected on recv */
    uint8_t  rx_buf[TCP_RX_BUF_SIZE];
    int      rx_len;
    /* Listen backlog for accept() */
    int      backlog;
    int      pending_count;
    int      pending_ids[TCP_BACKLOG_MAX];  /* IDs of pending connections */
    int      time_wait_count; /* FIXED (v4.1.8): TIME_WAIT cleanup counter */
    int      syn_recv_count;  /* FIXED (v4.1.8): SYN_RECV timeout counter */
};

static struct tcp_socket tcp_sockets[MAX_TCP_SOCKETS];
static spinlock_t tcp_lock;
static int tcp_next_id = 1;
static uint32_t tcp_iss = 0; /* initial send sequence counter */
static int syn_rate_count = 0;  /* FIXED (v4.2.9): BUG-SYN-FLOOD — per-poll-window SYN counter */

/*
 * FIXED (v4.1.8): Use TSC + large increment for better ISN randomness.
 * Previously used fixed increment 0x10000, making sequence numbers
 * completely predictable.  (H-17: TCP ISN predictable)
 */
static uint32_t tcp_get_iss(void) {
    uint32_t tsc_low, tsc_high;
    asm volatile ("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    /* Mix TSC into the ISN for unpredictability */
    tcp_iss += 0x10000 + (tsc_low & 0xFFFF);
    if (tcp_iss == 0) tcp_iss = 0x10000;
    return tcp_iss;
}

static struct tcp_socket *tcp_find_socket(int id) {
    int i;
    for (i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_sockets[i].in_use && tcp_sockets[i].id == id) {
            return &tcp_sockets[i];
        }
    }
    return NULL;
}

static struct tcp_socket *tcp_find_by_addr(const uint8_t src_ip[4],
                                            uint16_t src_port,
                                            const uint8_t dst_ip[4],
                                            uint16_t dst_port) {
    int i;
    for (i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_sockets[i].in_use &&
            memcmp(tcp_sockets[i].local_ip, dst_ip, 4) == 0 &&
            tcp_sockets[i].local_port == dst_port &&
            memcmp(tcp_sockets[i].remote_ip, src_ip, 4) == 0 &&
            tcp_sockets[i].remote_port == src_port) {
            return &tcp_sockets[i];
        }
    }
    return NULL;
}

static int tcp_send_packet(struct tcp_socket *sock, uint8_t flags,
                            const void *data, int data_len) {
    if (data_len < 0) return -1;
    struct net_if *iface = net_if_find_by_ip(sock->remote_ip);
    if (!iface || !iface->netdev) return -1;

    int total = (int)(sizeof(struct tcp_hdr) + data_len);
    uint8_t *packet = (uint8_t *)kmalloc((size_t)total);
    if (!packet) return -1;

    struct tcp_hdr *tcp = (struct tcp_hdr *)packet;
    tcp->src_port = htons(sock->local_port);
    tcp->dst_port = htons(sock->remote_port);
    tcp->seq_num = htonl(sock->seq_num);
    tcp->ack_num = htonl(sock->ack_num);
    tcp->data_offset_flags = htons((uint16_t)((5 << 12) | flags));
    tcp->window = htons((uint16_t)(TCP_RX_BUF_SIZE - sock->rx_len));  /* FIXED (v4.2.1): reflect actual free buffer space (BUG-NET-M7) */
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    if (data_len > 0) {
        memcpy(packet + sizeof(struct tcp_hdr), data, (size_t)data_len);
    }

    tcp->checksum = tcp_udp_checksum(iface->ip, sock->remote_ip,
                                      IP_PROTO_TCP, packet, total);

    int ret = ip_send(sock->remote_ip, IP_PROTO_TCP, packet, (uint16_t)total);
    kfree(packet);

    if (ret >= 0 && (flags & (TCP_SYN | TCP_FIN))) {
        sock->seq_num++;
    }
    if (ret >= 0 && data_len > 0) {
        sock->seq_num += (uint32_t)data_len;
    }
    return ret;
}

int tcp_socket_create(void) {
    spin_lock(&tcp_lock);

    int i;
    for (i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (!tcp_sockets[i].in_use) {
            int id = tcp_next_id++;
            if (tcp_next_id <= 0) tcp_next_id = 1;

            memset(&tcp_sockets[i], 0, sizeof(tcp_sockets[i]));
            tcp_sockets[i].id = id;
            tcp_sockets[i].in_use = 1;
            tcp_sockets[i].state = TCP_CLOSED;

            spin_unlock(&tcp_lock);
            return id;
        }
    }

    spin_unlock(&tcp_lock);
    return -1;
}

int tcp_bind(int sock, uint16_t port) {
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    s->local_port = port;

    /* Use the first available interface's IP */
    int i;
    for (i = 0; i < net_if_count; i++) {
        if ((net_ifs[i].flags & NETIF_FLAG_UP) && net_ifs[i].netdev) {
            memcpy(s->local_ip, net_ifs[i].ip, 4);
            break;
        }
    }

    spin_unlock(&tcp_lock);
    return 0;
}

int tcp_connect(int sock, const uint8_t dst_ip[4], uint16_t dst_port) {
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    if (s->state != TCP_CLOSED) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    /* Set local IP */
    struct net_if *iface = net_if_find_by_ip(dst_ip);
    if (!iface) {
        int i;
        for (i = 0; i < net_if_count; i++) {
            if ((net_ifs[i].flags & NETIF_FLAG_UP) && net_ifs[i].netdev) {
                iface = &net_ifs[i];
                break;
            }
        }
    }
    if (!iface) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    memcpy(s->local_ip, iface->ip, 4);
    if (s->local_port == 0) {
        s->local_port = (uint16_t)(49152 + (sock % 16384));
    }
    memcpy(s->remote_ip, dst_ip, 4);
    s->remote_port = dst_port;

    s->isn = tcp_get_iss();
    s->seq_num = s->isn;
    s->ack_num = 0;
    s->rcv_nxt = 0;
    s->state = TCP_SYN_SENT;

    /* FIXED (v4.2.1): Send SYN while holding tcp_lock to prevent
     * SMP race — tcp_send_packet accesses sock fields that another
     * CPU could modify if we release the lock first.  (BUG-NET-H2) */
    tcp_send_packet(s, TCP_SYN, NULL, 0);
    spin_unlock(&tcp_lock);
    return 0;
}

int tcp_send(int sock, const void *data, int len) {
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    /* FIXED (v4.2.9): BUG-HTTP-HANDSHAKE — Only allow send() in
     * ESTABLISHED or CLOSE_WAIT states.  Return -EAGAIN so the caller
     * can retry after the 3-way handshake completes. */
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) {
        spin_unlock(&tcp_lock);
        return -EAGAIN;
    }

    if (len <= 0) {
        spin_unlock(&tcp_lock);
        return 0;
    }

    int send_len = len;
    if (send_len > TCP_MAX_SEGMENT) send_len = TCP_MAX_SEGMENT;

    /* FIXED (v4.2.1): Hold lock through tcp_send_packet to prevent
     * SMP race on socket fields.  (BUG-NET-H2) */
    int ret = tcp_send_packet(s, TCP_PSH | TCP_ACK, data, send_len);
    spin_unlock(&tcp_lock);
    if (ret < 0) return ret;
    return send_len;
}

int tcp_recv(int sock, void *buf, int max_len) {
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    if (s->rx_len <= 0) {
        spin_unlock(&tcp_lock);
        return 0;
    }

    int copy_len = s->rx_len;
    if (copy_len > max_len) copy_len = max_len;
    memcpy(buf, s->rx_buf, (size_t)copy_len);

    /* Remove copied data from buffer */
    if (copy_len < s->rx_len) {
        int remaining = s->rx_len - copy_len;
        int j;
        for (j = 0; j < remaining; j++) {
            s->rx_buf[j] = s->rx_buf[copy_len + j];
        }
        s->rx_len = remaining;
    } else {
        s->rx_len = 0;
    }

    spin_unlock(&tcp_lock);
    return copy_len;
}

int tcp_close(int sock) {
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    if (s->state == TCP_ESTABLISHED) {
        s->state = TCP_FIN_WAIT1;
        /* FIXED (v4.2.1): Hold lock through tcp_send_packet.  (BUG-NET-H2) */
        tcp_send_packet(s, TCP_FIN | TCP_ACK, NULL, 0);
        spin_unlock(&tcp_lock);
        return 0;
    }

    if (s->state == TCP_CLOSE_WAIT) {
        s->state = TCP_LAST_ACK;
        /* FIXED (v4.2.1): Hold lock through tcp_send_packet.  (BUG-NET-H2) */
        tcp_send_packet(s, TCP_FIN | TCP_ACK, NULL, 0);
        spin_unlock(&tcp_lock);
        return 0;
    }

    if (s->state == TCP_CLOSED) {
        spin_unlock(&tcp_lock);
        return 0;
    }

    spin_unlock(&tcp_lock);
    return -1;
}

int tcp_listen(int sock, int backlog) {
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    if (backlog < 0) backlog = 0;
    if (backlog > TCP_BACKLOG_MAX) backlog = TCP_BACKLOG_MAX;
    s->backlog = backlog;
    s->state = TCP_LISTEN;
    s->pending_count = 0;

    spin_unlock(&tcp_lock);
    return 0;
}

int tcp_accept(int sock, uint8_t remote_ip[4], uint16_t *remote_port) {
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    if (s->state != TCP_LISTEN) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    /* Check for pending connections */
    if (s->pending_count <= 0) {
        spin_unlock(&tcp_lock);
        return -1;  /* No pending connections */
    }

    /* Dequeue the first pending connection */
    int accepted_id = s->pending_ids[0];
    s->pending_count--;
    for (int i = 0; i < s->pending_count; i++) {
        s->pending_ids[i] = s->pending_ids[i + 1];
    }

    /* Find the accepted socket and get its remote address */
    struct tcp_socket *accepted = tcp_find_socket(accepted_id);
    if (accepted) {
        /* FIXED (v4.2.9): BUG-ACCEPT-STATE — Only return sockets that
         * have completed the 3-way handshake (ESTABLISHED).  If the
         * socket is still in SYN_RECEIVED, tcp_send() would refuse to
         * send, breaking the server side. */
        if (accepted->state != TCP_ESTABLISHED) {
            spin_unlock(&tcp_lock);
            return -1;
        }
        if (remote_ip) memcpy(remote_ip, accepted->remote_ip, 4);
        if (remote_port) *remote_port = accepted->remote_port;
    }

    spin_unlock(&tcp_lock);
    return accepted_id;
}

int tcp_shutdown(int sock, int how) {
    (void)how;
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    if (s->state == TCP_ESTABLISHED) {
        s->state = TCP_FIN_WAIT1;
        /* FIXED (v4.2.1): Hold lock through tcp_send_packet.  (BUG-NET-H2) */
        tcp_send_packet(s, TCP_FIN | TCP_ACK, NULL, 0);
        spin_unlock(&tcp_lock);
        return 0;
    }

    if (s->state == TCP_CLOSE_WAIT) {
        s->state = TCP_LAST_ACK;
        /* FIXED (v4.2.1): Hold lock through tcp_send_packet.  (BUG-NET-H2) */
        tcp_send_packet(s, TCP_FIN | TCP_ACK, NULL, 0);
        spin_unlock(&tcp_lock);
        return 0;
    }

    spin_unlock(&tcp_lock);
    return -1;
}

int tcp_getsockname(int sock, uint8_t local_ip[4], uint16_t *local_port) {
    /* FIXED (v4.2.1): Validate output pointers before use.
     * (BUG-NET-M5) */
    if (!local_ip || !local_port) return -1;
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }
    memcpy(local_ip, s->local_ip, 4);
    *local_port = s->local_port;
    spin_unlock(&tcp_lock);
    return 0;
}

int tcp_getpeername(int sock, uint8_t remote_ip[4], uint16_t *remote_port) {
    /* FIXED (v4.2.1): Validate output pointers before use.
     * (BUG-NET-M5) */
    if (!remote_ip || !remote_port) return -1;
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }
    memcpy(remote_ip, s->remote_ip, 4);
    *remote_port = s->remote_port;
    spin_unlock(&tcp_lock);
    return 0;
}

static struct tcp_socket *tcp_find_listener(uint16_t port) {
    int i;
    for (i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_sockets[i].in_use &&
            tcp_sockets[i].state == TCP_LISTEN &&
            tcp_sockets[i].local_port == port) {
            return &tcp_sockets[i];
        }
    }
    return NULL;
}

static void tcp_handle_packet(const uint8_t src_ip[4],
                               const uint8_t dst_ip[4],
                               const uint8_t *data, int len) {
    if (len < (int)sizeof(struct tcp_hdr)) return;

    const struct tcp_hdr *tcp = (const struct tcp_hdr *)data;
    uint16_t data_offset = (ntohs(tcp->data_offset_flags) >> 12) * 4;
    uint8_t  flags = (uint8_t)(ntohs(tcp->data_offset_flags) & 0x3F);

    if (data_offset < sizeof(struct tcp_hdr) || (int)data_offset > len) return;

    /*
     * FIXED (v4.2.0): Verify TCP checksum on receive.
     * Without this check, forged TCP segments with incorrect checksums
     * would be accepted, allowing attackers to inject spoofed data or
     * RST packets.  The checksum covers the pseudo-header + TCP header
     * + payload.  (Top 10 #4)
     */
    uint16_t received_csum = tcp->checksum;
    /*
     * FIXED (v4.2.3): Compute the TCP checksum over the actual packet
     * data, not a 20-byte stack copy.  Previously, &tcp_copy (sizeof
     * tcp_hdr = 20 bytes) was passed with len (which includes the
     * payload), causing the checksum function to read stack garbage
     * for the payload bytes.  The checksum was effectively random.
     * (BUG-NET-01)
     */
    uint16_t computed_csum;
    {
        /* Build a temporary buffer: pseudo-header (12 bytes) + TCP header + payload */
        struct tcp_pseudo_hdr {
            uint8_t  src_ip[4];
            uint8_t  dst_ip[4];
            uint8_t  zero;
            uint8_t  protocol;
            uint16_t tcp_length;
        } __attribute__((packed)) pseudo;
        memcpy(pseudo.src_ip, src_ip, 4);
        memcpy(pseudo.dst_ip, dst_ip, 4);
        pseudo.zero = 0;
        pseudo.protocol = IP_PROTO_TCP;
        pseudo.tcp_length = htons((uint16_t)len);

        /*
         * FIXED (v4.2.5): BUG-TCP-CSUM-ENDIAN — tcp_udp_checksum() sums
         * raw 16-bit values (already in network byte order from htons()).
         * The verification code must NOT use ntohs() on each word, and
         * the final result must NOT be wrapped with htons(), or the
         * computed checksum will not match what tcp_udp_checksum() produced.
         */
        /* Compute checksum: pseudo-header (12) + raw packet data (len) */
        uint32_t sum = 0;
        uint16_t *ptr = (uint16_t *)&pseudo;
        for (int i = 0; i < 6; i++) sum += ptr[i];
        /* Add TCP header + payload, skipping the checksum field */
        uint16_t *data16 = (uint16_t *)data;
        for (int i = 0; i < len / 2; i++) {
            if (i == 8) continue;  /* Skip checksum field (offset 16-17) */
            sum += data16[i];
        }
        if (len & 1) sum += ((uint16_t)data[len - 1]) << 8;
        /* Fold carry */
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        computed_csum = (uint16_t)(~sum);
    }
    if (received_csum != 0 && computed_csum != received_csum) {
        /* Checksum mismatch — silently drop */
        return;
    }

    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq = ntohl(tcp->seq_num);

    int payload_len = len - (int)data_offset;
    const uint8_t *payload = data + data_offset;

    spin_lock(&tcp_lock);

    struct tcp_socket *sock = tcp_find_by_addr(src_ip, src_port,
                                                dst_ip, dst_port);
    if (!sock) {
        /* Also try matching by just remote address (for SYN_SENT) */
        int i;
        for (i = 0; i < MAX_TCP_SOCKETS; i++) {
            if (tcp_sockets[i].in_use &&
                tcp_sockets[i].local_port == dst_port &&
                tcp_sockets[i].state == TCP_SYN_SENT &&
                memcmp(tcp_sockets[i].remote_ip, src_ip, 4) == 0 &&
                tcp_sockets[i].remote_port == src_port) {
                sock = &tcp_sockets[i];
                break;
            }
        }
    }

    if (!sock) {
        /* Check for a listening socket on this port */
        struct tcp_socket *listener = tcp_find_listener(dst_port);
        if (listener && (flags & TCP_SYN) && !(flags & TCP_ACK)) {
            /* Incoming SYN for a listening socket — create a new connection */
            if (listener->pending_count >= listener->backlog) {
                /* Backlog full — drop silently */
                spin_unlock(&tcp_lock);
                return;
            }

            /* FIXED (v4.2.9): BUG-SYN-FLOOD — Rate limit incoming SYN
             * packets to prevent SYN flood attacks from exhausting the
             * 16 TCP slots.  Allow at most 5 SYN_RECEIVED sockets per
             * poll window.  The counter is reset in net_poll(). */
            if (syn_rate_count >= 5) {
                spin_unlock(&tcp_lock);
                return;
            }
            syn_rate_count++;

            /* Create a new socket for this connection */
            int new_id = tcp_next_id++;
            if (tcp_next_id <= 0) tcp_next_id = 1;

            int slot = -1;
            for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
                if (!tcp_sockets[i].in_use) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                spin_unlock(&tcp_lock);
                return;  /* No free socket slots */
            }

            memset(&tcp_sockets[slot], 0, sizeof(tcp_sockets[slot]));
            tcp_sockets[slot].id = new_id;
            tcp_sockets[slot].in_use = 1;
            tcp_sockets[slot].state = TCP_SYN_RECEIVED;
            memcpy(tcp_sockets[slot].local_ip, listener->local_ip, 4);
            tcp_sockets[slot].local_port = dst_port;
            memcpy(tcp_sockets[slot].remote_ip, src_ip, 4);
            tcp_sockets[slot].remote_port = src_port;
            tcp_sockets[slot].isn = tcp_get_iss();
            tcp_sockets[slot].seq_num = tcp_sockets[slot].isn;
            tcp_sockets[slot].ack_num = seq + 1;
            tcp_sockets[slot].rcv_nxt = seq + 1;

            /* Add to listener's pending queue */
            if (listener->pending_count < TCP_BACKLOG_MAX) {
                listener->pending_ids[listener->pending_count++] = new_id;
            }

            /* FIXED (v4.2.7): BUG-TCP-SYNACK-LOCK — Send SYN-ACK while
             * holding tcp_lock to prevent SMP race on socket fields.
             * Previously the lock was released before the packet was sent,
             * allowing another CPU to modify the socket state. */
            tcp_send_packet(&tcp_sockets[slot], TCP_SYN | TCP_ACK, NULL, 0);

            spin_unlock(&tcp_lock);
            return;
        }

        /* Send RST if no socket found and not a RST itself */
        spin_unlock(&tcp_lock);
        if (!(flags & TCP_RST)) {
            /* We can't send RST without a socket, skip */
        }
        return;
    }

    switch (sock->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            sock->ack_num = seq + 1;
            sock->rcv_nxt = seq + 1;
            sock->state = TCP_ESTABLISHED;
            /* FIXED (v4.1.8): Initialize congestion control for the new connection.
             * (BUG C-7) */
            /* FIXED (v4.2.3): Call tcp_cong_socket_init() to create the
             * congestion control slot before tcp_cong_on_ack().  Previously
             * the slot was never created, causing tcp_cong_on_ack() to
             * return immediately without doing anything.  (BUG-NET-03) */
            tcp_cong_socket_init(sock->id);
            tcp_cong_on_ack(sock->id, sock->ack_num, 0);
            /* FIXED (v4.2.8): BUG-TCP-LOCK — Send ACK inside lock to
             * prevent use-after-free race on socket.  Only copy sock->id
             * for the log_printf after unlock. */
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
            {
                int sock_id = sock->id;
                spin_unlock(&tcp_lock);
                log_printf(LOG_LEVEL_DEBUG,
                           "tcp: connection established sock=%d\n", sock_id);
            }
        } else if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            /* FIXED (v4.2.8): BUG-TCP-RST — Clear in_use when RST closes socket */
            sock->in_use = 0;
            spin_unlock(&tcp_lock);
        } else {
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_SYN_RECEIVED:
        if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            sock->in_use = 0;
            spin_unlock(&tcp_lock);
            break;
        }
        if (flags & TCP_ACK) {
            /* Third handshake: ACK received, connection established */
            sock->ack_num = seq;
            sock->state = TCP_ESTABLISHED;
            /* FIXED (v4.2.1): Initialize congestion control for server-side
             * connections.  Previously only the client side (SYN_SENT→ESTABLISHED)
             * initialized congestion control.  (BUG-NET-M2) */
            /* FIXED (v4.2.3): Call tcp_cong_socket_init() to create the
             * congestion control slot first.  (BUG-NET-03) */
            tcp_cong_socket_init(sock->id);
            tcp_cong_on_ack(sock->id, sock->ack_num, 0);
            /* FIXED (v4.2.8): BUG-TCP-LOCK — Copy sock->id before unlock */
            {
                int sock_id = sock->id;
                spin_unlock(&tcp_lock);
                log_printf(LOG_LEVEL_DEBUG,
                           "tcp: server connection established sock=%d\n", sock_id);
            }
        } else {
            /*
             * FIXED (v4.2.1): SYN_RECV counter is now incremented ONLY
             * in net_poll() to prevent double-increment from both
             * tcp_handle_packet and net_poll.  (BUG-NET-M1)
             */
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            /* FIXED (v4.2.8): BUG-TCP-RST — Clear in_use when RST closes socket */
            sock->in_use = 0;
            spin_unlock(&tcp_lock);
            break;
        }

        if (flags & TCP_FIN) {
            sock->ack_num = seq + 1;
            sock->state = TCP_CLOSE_WAIT;
            /* FIXED (v4.2.8): BUG-TCP-LOCK — Send ACK inside lock */
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
            spin_unlock(&tcp_lock);
            break;
        }

        if (payload_len > 0) {
            /*
             * FIXED (v4.1.8): Validate TCP sequence number before
             * accepting data.  Previously, data was blindly appended
             * without checking seq against rcv_nxt, allowing duplicate
             * or out-of-order segments to corrupt the receive buffer.
             * (BUG C-12)
             */
            if (seq != sock->rcv_nxt) {
                /* Out-of-order or duplicate: send ACK with current rcv_nxt */
                /* FIXED (v4.2.8): BUG-TCP-LOCK — Send ACK inside lock */
                tcp_send_packet(sock, TCP_ACK, NULL, 0);
                spin_unlock(&tcp_lock);
                break;
            }
            /* Accept data */
            int copy_len = payload_len;
            if (copy_len > (int)sizeof(sock->rx_buf) - sock->rx_len) {
                copy_len = (int)sizeof(sock->rx_buf) - sock->rx_len;
            }
            if (copy_len > 0) {
                memcpy(sock->rx_buf + sock->rx_len, payload, (size_t)copy_len);
                sock->rx_len += copy_len;
            }
            sock->ack_num = seq + (uint32_t)payload_len;
            sock->rcv_nxt = sock->ack_num;
            /* FIXED (v4.2.8): BUG-TCP-LOCK — Send ACK inside lock */
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
            spin_unlock(&tcp_lock);
        } else {
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            /* FIXED (v4.2.8): BUG-TCP-RST — Clear in_use when RST closes socket */
            sock->in_use = 0;
            spin_unlock(&tcp_lock);
            break;
        }

        if ((flags & (TCP_FIN | TCP_ACK)) == (TCP_FIN | TCP_ACK)) {
            sock->ack_num = seq + 1;
            sock->state = TCP_TIME_WAIT;
            /* FIXED (v4.2.8): BUG-TCP-LOCK — Send ACK inside lock */
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
            spin_unlock(&tcp_lock);
        } else if (flags & TCP_ACK) {
            sock->state = TCP_FIN_WAIT2;
            spin_unlock(&tcp_lock);
        } else {
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            /* FIXED (v4.2.8): BUG-TCP-RST — Clear in_use when RST closes socket */
            sock->in_use = 0;
            spin_unlock(&tcp_lock);
            break;
        }

        if (flags & TCP_FIN) {
            sock->ack_num = seq + 1;
            sock->state = TCP_TIME_WAIT;
            /* FIXED (v4.2.8): BUG-TCP-LOCK — Send ACK inside lock */
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
            spin_unlock(&tcp_lock);
        } else {
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_CLOSE_WAIT:
        /* Waiting for application to call close */
        spin_unlock(&tcp_lock);
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            /* FIXED (v4.2.8): BUG-TCP-RST — Clear in_use when RST closes socket */
            sock->in_use = 0;
            spin_unlock(&tcp_lock);
            break;
        }

        if (flags & TCP_ACK) {
            sock->state = TCP_CLOSED;
            /* FIXED (v4.2.8): BUG-TCP-RST — Clear in_use when connection closes */
            sock->in_use = 0;
            spin_unlock(&tcp_lock);
        } else {
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_TIME_WAIT:
        /*
         * FIXED (v4.2.1): TIME_WAIT counter is now incremented ONLY
         * in net_poll() to prevent double-increment from both
         * tcp_handle_packet and net_poll.  (BUG-NET-M1)
         */
        spin_unlock(&tcp_lock);
        break;

    case TCP_CLOSED:
        spin_unlock(&tcp_lock);
        break;

    default:
        spin_unlock(&tcp_lock);
        break;
    }
}

/* ================================================================
 * Loopback Device
 * ================================================================ */
#define LOOPBACK_MTU  65536

/* FIXED (v4.2.9): BUG-LOOPBACK — Use a ring buffer queue instead of a
 * single static buffer.  The old design used one buffer that was
 * overwritten by every subsequent send(), causing data loss when
 * multiple packets were sent before being consumed. */
#define LOOPBACK_QUEUE_SIZE 16
#define LOOPBACK_BUF_SIZE 2048

static struct {
    uint8_t data[LOOPBACK_BUF_SIZE];
    int len;
} loopback_queue[LOOPBACK_QUEUE_SIZE];
static int loopback_head = 0;  /* next to dequeue */
static int loopback_tail = 0;  /* next to enqueue */
static int loopback_count = 0;
static spinlock_t loopback_lock;

static int loopback_send(struct net_device *netdev, const void *data, int len) {
    (void)netdev;
    spin_lock(&loopback_lock);
    if (loopback_count >= LOOPBACK_QUEUE_SIZE) {
        /* Queue full — drop oldest */
        loopback_head = (loopback_head + 1) % LOOPBACK_QUEUE_SIZE;
        loopback_count--;
    }
    if (len > LOOPBACK_BUF_SIZE) len = LOOPBACK_BUF_SIZE;
    memcpy(loopback_queue[loopback_tail].data, data, (size_t)len);
    loopback_queue[loopback_tail].len = len;
    loopback_tail = (loopback_tail + 1) % LOOPBACK_QUEUE_SIZE;
    loopback_count++;
    spin_unlock(&loopback_lock);
    return len;
}

static int loopback_recv(struct net_device *netdev, void *buf, int max_len) {
    (void)netdev;
    spin_lock(&loopback_lock);
    if (loopback_count <= 0) {
        spin_unlock(&loopback_lock);
        return 0;
    }
    int copy_len = loopback_queue[loopback_head].len;
    if (copy_len > max_len) copy_len = max_len;
    memcpy(buf, loopback_queue[loopback_head].data, (size_t)copy_len);
    loopback_head = (loopback_head + 1) % LOOPBACK_QUEUE_SIZE;
    loopback_count--;
    spin_unlock(&loopback_lock);
    return copy_len;
}

static int loopback_up(struct net_device *netdev) {
    netdev->flags |= NETDEV_FLAG_UP;
    return 0;
}

static int loopback_down(struct net_device *netdev) {
    netdev->flags &= ~NETDEV_FLAG_UP;
    return 0;
}

static struct net_device loopback_netdev;

static void loopback_init(void) {
    memset(&loopback_netdev, 0, sizeof(loopback_netdev));

    const char *name = "lo";
    memcpy(loopback_netdev.name, name, strlen(name) + 1);
    loopback_netdev.mtu = LOOPBACK_MTU;
    loopback_netdev.send = loopback_send;
    loopback_netdev.recv = loopback_recv;
    loopback_netdev.up = loopback_up;
    loopback_netdev.down = loopback_down;
    loopback_netdev.priv = NULL;
    loopback_netdev.flags = NETDEV_FLAG_UP;

    netdev_register(&loopback_netdev);

    /* Register loopback as a network interface */
    if (net_if_count < MAX_NET_IF) {
        struct net_if *iface = &net_ifs[net_if_count];
        memset(iface, 0, sizeof(*iface));
        memcpy(iface->name, "lo", 3);
        iface->ip[0] = 127;
        iface->ip[1] = 0;
        iface->ip[2] = 0;
        iface->ip[3] = 1;
        iface->netmask[0] = 255;
        iface->netmask[1] = 0;
        iface->netmask[2] = 0;
        iface->netmask[3] = 0;
        iface->mtu = LOOPBACK_MTU;
        iface->flags = NETIF_FLAG_UP | NETIF_FLAG_RUNNING;
        iface->netdev = &loopback_netdev;
        net_if_count++;
    }

    log_printf(LOG_LEVEL_INFO, "net: loopback interface initialized\n");
}

/* ================================================================
 * Packet Processing
 * ================================================================ */
static void process_eth_frame(struct net_device *netdev,
                               const uint8_t *data, int len) {
    if (len < (int)sizeof(struct eth_hdr)) return;

    const struct eth_hdr *eth = (const struct eth_hdr *)data;
    uint16_t ethertype = ntohs(eth->ethertype);

    const uint8_t *payload = data + sizeof(struct eth_hdr);
    int payload_len = len - (int)sizeof(struct eth_hdr);

    switch (ethertype) {
    case ETH_ARP:
        arp_handle_packet(payload, payload_len);
        break;
    case ETH_IPV4:
        ip_handle_packet(netdev, payload, payload_len);
        break;
    case ETH_IPV6:
        ipv6_handle_packet(netdev, payload, payload_len);
        break;
    default:
        break;
    }
}

/* ================================================================
 * TCP Retransmit Timer
 *
 * FIXED (v4.2.8): BUG-TCP-RETRANSMIT — The retransmit timer was never
 * called from the timer interrupt handler, making TCP retransmission
 * dead code.  This function is called from pit_handler.c on each timer
 * tick.  It walks all TCP sockets and triggers congestion timeout
 * (tcp_cong_on_timeout) for sockets that have been waiting for an ACK
 * longer than their RTO.
 * ================================================================ */
void tcp_retransmit_timer(void) {
    /*
     * FIXED (v4.2.8): BUG-TCP-RETRANSMIT — This timer is now called
     * from the PIT timer interrupt handler (pit_handler.c).  It
     * walks all TCP sockets and triggers congestion timeout handling
     * via tcp_cong_on_timeout() for sockets in ESTABLISHED state.
     * The actual retransmission logic is in tcp_cong_on_timeout(),
     * which adjusts cwnd/ssthresh and backs off RTO.  The socket
     * layer will retransmit on the next send attempt.
     */
    int i;
    spin_lock(&tcp_lock);
    for (i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_sockets[i].in_use &&
            tcp_sockets[i].state == TCP_ESTABLISHED) {
            /*
             * Notify congestion control of a timeout.  This triggers
             * exponential RTO backoff and cwnd reset.  The actual
             * retransmission is driven by the application or by the
             * next ACK-triggered send.
             */
            tcp_cong_on_timeout(tcp_sockets[i].id);
        }
    }
    spin_unlock(&tcp_lock);
}

/* ================================================================
 * Main Initialization & Polling
 * ================================================================ */
void net_init(void) {
    log_printf(LOG_LEVEL_INFO, "net: initializing TCP/IP stack\n");

    spin_init(&udp_lock);
    spin_init(&tcp_lock);
    spin_init(&arp_lock);  /* FIXED (v4.2.1): init ARP cache lock (BUG-NET-M6) */

    /* FIXED (v4.1.8): Initialize TCP congestion control.
     * Previously, tcp_cong.c was compiled but never called,
     * making congestion control dead code.  (BUG C-7) */
    tcp_cong_init();
    spin_init(&loopback_lock);

    /* AF_UNIX (v4.2.6): Initialize Unix domain socket subsystem */
    unix_init();

    memset(net_ifs, 0, sizeof(net_ifs));
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(udp_sockets, 0, sizeof(udp_sockets));
    memset(tcp_sockets, 0, sizeof(tcp_sockets));
    memset(loopback_queue, 0, sizeof(loopback_queue));

    /* Initialize loopback */
    loopback_init();

    /* Discover and register network devices */
    struct net_device *netdev = netdev_get_first();
    int eth_count = 0;
    while (netdev) {
        /* Skip loopback (already registered) */
        if (strcmp(netdev->name, "lo") == 0) {
            netdev = netdev->next;
            continue;
        }

        /* Bring the interface up */
        if (netdev->up) {
            netdev->up(netdev);
        }

        if (net_if_count < MAX_NET_IF) {
            struct net_if *iface = &net_ifs[net_if_count];
            memset(iface, 0, sizeof(*iface));

            size_t name_len = strlen(netdev->name);
            if (name_len >= sizeof(iface->name)) name_len = sizeof(iface->name) - 1;
            memcpy(iface->name, netdev->name, name_len);
            iface->name[name_len] = '\0';

            memcpy(iface->mac, netdev->mac, 6);
            iface->mtu = netdev->mtu;

            /* Default IP configuration (10.0.2.15 for QEMU user-mode networking) */
            iface->ip[0] = 10;
            iface->ip[1] = 0;
            iface->ip[2] = 2;
            iface->ip[3] = (uint8_t)(15 + eth_count);

            iface->netmask[0] = 255;
            iface->netmask[1] = 255;
            iface->netmask[2] = 255;
            iface->netmask[3] = 0;

            iface->gateway[0] = 10;
            iface->gateway[1] = 0;
            iface->gateway[2] = 2;
            iface->gateway[3] = 2;

            iface->flags = NETIF_FLAG_UP | NETIF_FLAG_RUNNING;
            iface->netdev = netdev;

            net_if_count++;
            eth_count++;

            log_printf(LOG_LEVEL_INFO,
                       "net: interface '%s' ip=%d.%d.%d.%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                       iface->name,
                       iface->ip[0], iface->ip[1], iface->ip[2], iface->ip[3],
                       iface->mac[0], iface->mac[1], iface->mac[2],
                       iface->mac[3], iface->mac[4], iface->mac[5]);
        }

        netdev = netdev->next;
    }

    log_printf(LOG_LEVEL_INFO, "net: initialized %d interface(s)\n", net_if_count);

    /* Initialize DHCP client */
    dhcp_init();

    /* Initialize IPv6 stack */
    ipv6_init();

    /* FIXED (v4.2.2): Initialize DNS cache subsystem */
    dns_init();

    /*
     * FIXED (v4.2.2): Use dhcp_start() instead of dhcp_run().
     * dhcp_start() initiates the async DHCP state machine without
     * blocking.  The state machine is advanced by dhcp_poll() which
     * is called from net_poll() in the main loop.
     */
    dhcp_start();
}

void net_poll(void) {
    uint8_t buf[2048];
    int i;

    /* FIXED (v4.2.9): BUG-SYN-FLOOD — Reset SYN rate limit counter
     * each poll cycle so that the 5-SYN limit is per window. */
    syn_rate_count = 0;

    for (i = 0; i < net_if_count; i++) {
        if (!(net_ifs[i].flags & NETIF_FLAG_UP)) continue;
        if (!net_ifs[i].netdev) continue;

        int len = eth_recv(net_ifs[i].netdev, buf, (int)sizeof(buf));
        if (len > 0) {
            process_eth_frame(net_ifs[i].netdev, buf, len);
        }
    }

    /*
     * FIXED (v4.1.8): Periodic ARP cache aging.
     * Invalidate stale entries to prevent using outdated MAC addresses.
     * (H-20: ARP cache no aging)
     */
    arp_cache_age();

    /* FIXED (v4.2.9): BUG-IPV6-NEIGH — Age out stale IPv6 neighbor
     * cache entries to prevent the cache from filling up with
     * unreachable hosts. */
    ipv6_neighbor_age();

    /*
     * FIXED (v4.1.8): Periodic TCP timeout cleanup.
     * Clean up expired SYN_RECV and TIME_WAIT sockets to prevent
     * resource exhaustion.  (BUG C-10, C-11)
     */
    spin_lock(&tcp_lock);
    for (i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (!tcp_sockets[i].in_use) continue;
        if (tcp_sockets[i].state == TCP_SYN_RECEIVED) {
            tcp_sockets[i].syn_recv_count++;
            if (tcp_sockets[i].syn_recv_count > 500) {
                log_printf(LOG_LEVEL_DEBUG, "tcp: SYN_RECV timeout sock=%d\n",
                           tcp_sockets[i].id);
                tcp_sockets[i].state = TCP_CLOSED;
                tcp_sockets[i].in_use = 0;
            }
        }
        /*
         * FIXED (v4.1.8): Also clean up TIME_WAIT sockets.
         * (C-10: TIME_WAIT never cleaned)
         */
        if (tcp_sockets[i].state == TCP_TIME_WAIT) {
            tcp_sockets[i].time_wait_count++;
            if (tcp_sockets[i].time_wait_count > 1000) {
                tcp_sockets[i].state = TCP_CLOSED;
                tcp_sockets[i].in_use = 0;
            }
        }
    }
    spin_unlock(&tcp_lock);

    /*
     * FIXED (v4.1.9): Periodic DHCP lease renewal check.
     * Called from net_poll() to automatically renew the DHCP lease
     * before it expires.  (H-25: DHCP lease 24h no renewal)
     *
     * FIXED (v4.2.2): dhcp_poll() now also advances the async DHCP
     * state machine (DISCOVER → OFFER → REQUEST → ACK → BOUND)
     * without blocking the kernel.
     */
    dhcp_poll();
}