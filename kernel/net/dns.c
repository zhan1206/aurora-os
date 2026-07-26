/*
 * dns.c - DNS Resolver
 */

#include "net.h"
#include "log.h"
#include "string.h"
#include "../mem.h"
#include "../aslr.h"
#include "../sched.h"      /* FIXED (v4.2.8): BUG-DNS-BUSY — schedule() */
#include <stdint.h>

/* Byte order conversion */
static inline uint16_t ntohs(uint16_t n) {
    return ((n & 0xFF) << 8) | ((n & 0xFF00) >> 8);
}

static inline uint16_t htons(uint16_t n) {
    return ntohs(n);
}

/* DNS server address */
static uint8_t dns_server_ip[4] = { 8, 8, 8, 8 };  /* Default: Google DNS */

/* Ephemeral source port for DNS queries (non-zero!) */
#define DNS_SRC_PORT  53530

/* DNS cache */
#define DNS_CACHE_SIZE 16
#define DNS_CACHE_TTL 300  /* FIXED (v4.2.2): TTL ~300 seconds (approximate via counter) */

/* FIXED (v4.2.2): Added age field for LRU eviction and TTL-based expiration.
 * dns_age_counter increments on each dns_cache_lookup; the oldest entry
 * (lowest age) is evicted when the cache is full.  Entries with age
 * difference > DNS_CACHE_TTL are considered stale and invalidated. */
/* FIXED (v4.2.7): BUG-DNS-CACHE-STRING — Added hostname field to prevent
 * hash collision false positives.  Two different domain names with the
 * same hash (e.g. "abc.com" and "xyz.net") would previously return the
 * wrong IP.  Now both hash AND hostname string are compared. */
struct dns_cache_entry {
    uint32_t hash;
    char     hostname[256];
    uint8_t  ip[4];
    int      valid;
    int      age;
};

static struct dns_cache_entry dns_cache[DNS_CACHE_SIZE];
static int dns_age_counter = 0;    /* FIXED (v4.2.2): LRU age counter */
static spinlock_t dns_cache_lock;  /* FIXED (v4.2.2): protect DNS cache from SMP races */

/* Simple hash function for hostname */
static uint32_t dns_hash(const char *hostname) {
    uint32_t hash = 5381;
    int c;
    while ((c = (int)(unsigned char)*hostname++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;
    }
    return hash;
}

/* ================================================================
 * dns_set_server
 * ================================================================ */
void dns_set_server(const uint8_t ip[4]) {
    memcpy(dns_server_ip, ip, 4);
    log_printf(LOG_LEVEL_INFO, "dns: server set to %d.%d.%d.%d\n",
               ip[0], ip[1], ip[2], ip[3]);
}

/* FIXED (v4.2.2): Initialize DNS cache spinlock for SMP safety. */
void dns_init(void) {
    spin_init(&dns_cache_lock);
    log_printf(LOG_LEVEL_DEBUG, "dns: cache initialized\n");
}

/* FIXED (v4.2.2): Invalidate DNS cache entries whose age has exceeded
 * DNS_CACHE_TTL.  Called before each cache lookup to ensure stale
 * entries are not returned. */
static void dns_cache_age(void) {
    int i;
    spin_lock(&dns_cache_lock);
    for (i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid &&
            (dns_age_counter - dns_cache[i].age) > DNS_CACHE_TTL) {
            dns_cache[i].valid = 0;
        }
    }
    spin_unlock(&dns_cache_lock);
}

/* ================================================================
 * Encode hostname into DNS label format
 * "www.example.com" -> 3www7example3com0
 * Returns encoded length
 * ================================================================ */
static int dns_encode_name(uint8_t *out, const char *hostname) {
    const char *p = hostname;
    uint8_t *start = out;

    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int seg_len = (int)(dot - p);
        if (seg_len > 63) seg_len = 63;  /* Max label length */
        *out++ = (uint8_t)seg_len;
        memcpy(out, p, (size_t)seg_len);
        out += seg_len;
        p = dot;
        if (*p == '.') p++;
    }

    *out++ = 0;  /* Terminating zero-length label */
    return (int)(out - start);
}

/* ================================================================
 * dns_query - Send DNS A record query and parse response
 * ================================================================ */
int dns_query(const char *hostname, uint8_t ip_out[4]) {
    if (!hostname || !ip_out) return -1;

    /* FIXED (v4.2.2): Age out stale entries and increment age counter
     * before cache lookup. */
    dns_cache_age();
    dns_age_counter++;

    /* Check cache */
    uint32_t hash = dns_hash(hostname);
    int i;
    spin_lock(&dns_cache_lock);
    for (i = 0; i < DNS_CACHE_SIZE; i++) {
        /* FIXED (v4.2.7): BUG-DNS-CACHE-STRING — Compare both hash
         * AND hostname string to prevent hash collision false positives. */
        if (dns_cache[i].valid && dns_cache[i].hash == hash &&
            strcmp(dns_cache[i].hostname, hostname) == 0) {
            memcpy(ip_out, dns_cache[i].ip, 4);
            /* FIXED (v4.2.2): Update LRU age on cache hit */
            dns_cache[i].age = dns_age_counter;
            spin_unlock(&dns_cache_lock);
            log_printf(LOG_LEVEL_DEBUG, "dns: cache hit for %s\n", hostname);
            return 0;
        }
    }
    spin_unlock(&dns_cache_lock);

    /* Build DNS query packet */
    int name_max = (int)strlen(hostname) * 2 + 4;  /* Worst case */
    int pkt_len = (int)sizeof(struct dns_header) + name_max + 4;
    uint8_t *pkt = (uint8_t *)kmalloc((size_t)pkt_len);
    if (!pkt) return -1;

    memset(pkt, 0, (size_t)pkt_len);

    /* DNS header */
    struct dns_header *hdr = (struct dns_header *)pkt;
    /*
     * FIXED (v4.2.5): BUG-DNS-ID — DNS query IDs are now generated using
     * the ChaCha20 CSPRNG instead of a predictable incrementing counter.
     * Each query gets a fresh 16-bit random ID, preventing DNS cache
     * poisoning and query correlation attacks.
     */
    {
        uint16_t qid;
        chacha20_random_bytes((uint8_t *)&qid, sizeof(qid));
        if (qid == 0) qid = 1;
        hdr->id = htons(qid);
    }
    hdr->flags = htons(DNS_QRY_STANDARD);
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    /* Question section: encode name */
    uint8_t *q = pkt + sizeof(struct dns_header);
    int name_len = dns_encode_name(q, hostname);

    /* QTYPE = A (1), QCLASS = IN (1) */
    q[name_len] = 0;
    q[name_len + 1] = (uint8_t)DNS_TYPE_A;
    q[name_len + 2] = 0;
    q[name_len + 3] = (uint8_t)DNS_CLASS_IN;

    int total_len = (int)sizeof(struct dns_header) + name_len + 4;
    int ret;

    /* Send DNS query via UDP */
    /* FIXED (v4.1.8): Use DNS_SRC_PORT instead of 0 as source port.
     * Port 0 is invalid; the UDP layer would reject it or cause
     * the DNS server to not respond.  (BUG C-8) */
    ret = udp_send(DNS_SRC_PORT, dns_server_ip, DNS_PORT, pkt, (uint16_t)total_len);
    /* Save the query ID for matching the response (before kfree) */
    uint16_t sent_id = ntohs(hdr->id);
    kfree(pkt);

    if (ret < 0) {
        log_printf(LOG_LEVEL_ERR, "dns: failed to send query for %s\n",
                   hostname);
        return -1;
    }

    /* Wait for response */
    uint8_t rx_buf[512];
    uint8_t src_ip[4];
    uint16_t src_port;
    int retry;

    for (retry = 0; retry < 30; retry++) {
        /* FIXED (v4.2.8): BUG-DNS-BUSY — Yield CPU to other tasks
         * and poll for network packets instead of busy-looping.
         * Without this, the DNS query blocks the entire kernel. */
        schedule();
        net_poll();

        int rx_len = udp_recvfrom(DNS_SRC_PORT, rx_buf, (int)sizeof(rx_buf),
                                   src_ip, &src_port);
        if (rx_len < (int)sizeof(struct dns_header)) continue;

        struct dns_header *rx_hdr = (struct dns_header *)rx_buf;
        /*
         * FIXED (v4.2.5): BUG-DNS-ID — Match against the CSPRNG-generated
         * sent query ID. (L-13)
         */
        if (ntohs(rx_hdr->id) != sent_id) continue;
        if (ntohs(rx_hdr->qdcount) != 1) continue;

        uint16_t ancount = ntohs(rx_hdr->ancount);
        if (ancount > 32) ancount = 32;
        if (ancount == 0) {
            log_printf(LOG_LEVEL_ERR, "dns: no answer for %s\n", hostname);
            return -1;
        }

        /* Skip question section */
        int pos = (int)sizeof(struct dns_header);
        /*
         * FIXED (v4.1.8): Add compression pointer loop counter and
         * offset validation.  Malicious DNS responses can use self-
         * referencing compression pointers to cause infinite loops.
         * (M-19: DNS compression pointer infinite loop, M-20: bounds)
         */
        int compress_steps = 0;
        while (pos < rx_len && rx_buf[pos] != 0 && compress_steps < 128) {
            if ((rx_buf[pos] & 0xC0) == 0xC0) {
                /* Validate compression pointer offset */
                if (pos + 1 >= rx_len) break;
                uint16_t ptr_offset = (uint16_t)(((rx_buf[pos] & 0x3F) << 8) | rx_buf[pos + 1]);
                if (ptr_offset >= (uint16_t)rx_len) break;
                compress_steps++;
                pos += 2;  /* Compressed name pointer */
                break;
            }
            /* FIXED (v4.2.1): Validate label length to prevent pos from
             * jumping past rx_len and causing out-of-bounds read. (BUG-NET-M9) */
            uint8_t label_len = rx_buf[pos];
            if (pos + 1 + label_len > rx_len) break;
            pos += 1 + label_len;
        }
        pos += 1;  /* Skip terminating zero */
        pos += 4;  /* Skip QTYPE + QCLASS */

        /*
         * Parse answer section for A record.
         *
         * FIXED (v4.1.2): Added bounds check after pos += rdlen to prevent
         * out-of-bounds read on the next iteration.  An attacker-controlled
         * rdlen value could cause pos to exceed rx_len, leading to a buffer
         * over-read (NH10).
         */
        int a;
        for (a = 0; a < (int)ancount && pos + 10 <= rx_len; a++) {
            /* Skip name (might be compressed) */
            if ((rx_buf[pos] & 0xC0) == 0xC0) {
                pos += 2;
            } else {
                while (pos < rx_len && rx_buf[pos] != 0) {
                    if (pos + 1 + rx_buf[pos] > rx_len) break;
                    pos += 1 + rx_buf[pos];
                }
                if (pos >= rx_len) break;
                pos += 1;
            }

            if (pos + 10 > rx_len) break;

            uint16_t rtype = ntohs(*(uint16_t *)(rx_buf + pos));
            uint16_t rclass = ntohs(*(uint16_t *)(rx_buf + pos + 2));
            uint16_t rdlen = ntohs(*(uint16_t *)(rx_buf + pos + 8));

            pos += 10;

            if (rtype == DNS_TYPE_A && rclass == DNS_CLASS_IN && rdlen == 4) {
                if (pos + 4 <= rx_len) {
                    memcpy(ip_out, rx_buf + pos, 4);

                    /* FIXED (v4.2.2): Add to cache with LRU eviction.
                     * If the cache is full, evict the least recently used
                     * entry (lowest age).  Otherwise use the first invalid slot. */
                    spin_lock(&dns_cache_lock);
                    int target = 0;
                    int oldest_age = dns_cache[0].age;
                    int found_slot = 0;
                    for (i = 0; i < DNS_CACHE_SIZE; i++) {
                        if (!dns_cache[i].valid) {
                            target = i;
                            found_slot = 1;
                            break;
                        }
                        if (dns_cache[i].age < oldest_age) {
                            oldest_age = dns_cache[i].age;
                            target = i;
                        }
                    }
                    /* If no empty slot, target is the LRU entry */
                    dns_cache[target].hash = hash;
                    /* FIXED (v4.2.7): BUG-DNS-CACHE-STRING — Store the
                     * domain name alongside the hash for collision-free lookup. */
                    {
                        int n = (int)strlen(hostname);
                        if (n > 255) n = 255;
                        memcpy(dns_cache[target].hostname, hostname, (size_t)n);
                        dns_cache[target].hostname[n] = '\0';
                    }
                    memcpy(dns_cache[target].ip, ip_out, 4);
                    dns_cache[target].age = dns_age_counter;
                    dns_cache[target].valid = 1;
                    spin_unlock(&dns_cache_lock);

                    log_printf(LOG_LEVEL_DEBUG,
                               "dns: resolved %s -> %d.%d.%d.%d\n",
                               hostname,
                               ip_out[0], ip_out[1], ip_out[2], ip_out[3]);
                    return 0;
                }
            }
            /* Advance past the RDATA, but validate against rx_len */
            if ((uint32_t)pos + (uint32_t)rdlen > (uint32_t)rx_len) break;
            pos += rdlen;
        }
    }

    log_printf(LOG_LEVEL_ERR, "dns: timeout for %s\n", hostname);
    return -1;
}