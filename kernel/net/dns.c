/*
 * dns.c - DNS Resolver
 */

#include "net.h"
#include "log.h"
#include "string.h"
#include "../mem.h"
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

/* DNS cache */
#define DNS_CACHE_SIZE 16
struct dns_cache_entry {
    uint32_t hash;
    uint8_t  ip[4];
    int      valid;
};

static struct dns_cache_entry dns_cache[DNS_CACHE_SIZE];

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

    /* Check cache */
    uint32_t hash = dns_hash(hostname);
    int i;
    for (i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && dns_cache[i].hash == hash) {
            memcpy(ip_out, dns_cache[i].ip, 4);
            log_printf(LOG_LEVEL_DEBUG, "dns: cache hit for %s\n", hostname);
            return 0;
        }
    }

    /* Build DNS query packet */
    int name_max = (int)strlen(hostname) * 2 + 4;  /* Worst case */
    int pkt_len = (int)sizeof(struct dns_header) + name_max + 4;
    uint8_t *pkt = (uint8_t *)kmalloc((size_t)pkt_len);
    if (!pkt) return -1;

    memset(pkt, 0, (size_t)pkt_len);

    /* DNS header */
    struct dns_header *hdr = (struct dns_header *)pkt;
    hdr->id = htons(0x0001);
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
    ret = udp_send(0, dns_server_ip, DNS_PORT, pkt, (uint16_t)total_len);
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
        net_poll();

        int rx_len = udp_recvfrom(0, rx_buf, (int)sizeof(rx_buf),
                                   src_ip, &src_port);
        if (rx_len < (int)sizeof(struct dns_header)) continue;

        struct dns_header *rx_hdr = (struct dns_header *)rx_buf;
        if (ntohs(rx_hdr->id) != 0x0001) continue;
        if (ntohs(rx_hdr->qdcount) != 1) continue;

        uint16_t ancount = ntohs(rx_hdr->ancount);
        if (ancount > 32) ancount = 32;
        if (ancount == 0) {
            log_printf(LOG_LEVEL_ERR, "dns: no answer for %s\n", hostname);
            return -1;
        }

        /* Skip question section */
        int pos = (int)sizeof(struct dns_header);
        /* Skip name (might be compressed) */
        while (pos < rx_len && rx_buf[pos] != 0) {
            if ((rx_buf[pos] & 0xC0) == 0xC0) {
                pos += 2;  /* Compressed name pointer */
                break;
            }
            pos += 1 + rx_buf[pos];
        }
        pos += 1;  /* Skip terminating zero */
        pos += 4;  /* Skip QTYPE + QCLASS */

        /* Parse answer section for A record */
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

                    /* Add to cache */
                    for (i = 0; i < DNS_CACHE_SIZE; i++) {
                        if (!dns_cache[i].valid) {
                            dns_cache[i].hash = hash;
                            memcpy(dns_cache[i].ip, ip_out, 4);
                            dns_cache[i].valid = 1;
                            break;
                        }
                    }

                    log_printf(LOG_LEVEL_DEBUG,
                               "dns: resolved %s -> %d.%d.%d.%d\n",
                               hostname,
                               ip_out[0], ip_out[1], ip_out[2], ip_out[3]);
                    return 0;
                }
            }
            pos += rdlen;
        }
    }

    log_printf(LOG_LEVEL_ERR, "dns: timeout for %s\n", hostname);
    return -1;
}