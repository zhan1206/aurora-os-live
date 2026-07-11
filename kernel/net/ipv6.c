/*
 * ipv6.c - IPv6 Protocol Implementation (Simplified)
 *
 * Implements basic IPv6 packet sending/receiving framework,
 * IPv6 address handling, and Neighbor Discovery Protocol (NDP)
 * support (RFC 4861) for address resolution.
 */
#include "net.h"
#include "log.h"
#include "string.h"
#include "../netdev.h"
#include "../mem.h"
#include "../smp.h"
#include <stdint.h>

/* ================================================================
 * Byte Order Conversion (local, since net.c defines these static)
 * ================================================================ */
static inline uint16_t ntohs(uint16_t n) {
    return ((n & 0xFF) << 8) | ((n & 0xFF00) >> 8);
}

static inline uint16_t htons(uint16_t n) {
    return ntohs(n);
}

/* ================================================================
 * Ethernet Type for IPv6
 * ================================================================ */
#define ETH_IPV6  0x86DD

/* ================================================================
 * ICMPv6 Types
 * ================================================================ */
#define ICMPV6_ECHO_REQUEST      128
#define ICMPV6_ECHO_REPLY        129
#define ICMPV6_ND_ROUTER_SOL     133
#define ICMPV6_ND_ROUTER_ADV     134
#define ICMPV6_ND_NEIGHBOR_SOL   135
#define ICMPV6_ND_NEIGHBOR_ADV   136
#define ICMPV6_ND_REDIRECT       137

/* ================================================================
 * ICMPv6 Option Types (NDP)
 * ================================================================ */
#define NDP_OPT_SOURCE_LLADDR    1
#define NDP_OPT_TARGET_LLADDR    2

/* ================================================================
 * NDP Neighbor Solicitation
 * ================================================================ */
struct ndp_neighbor_sol {
    uint32_t reserved;
    uint8_t  target_addr[16];
    /* Options follow */
} __attribute__((packed));

/* NDP Neighbor Advertisement */
struct ndp_neighbor_adv {
    uint8_t  flags;          /* R(outer), S(olicited), O(verride) */
    uint8_t  reserved[3];
    uint8_t  target_addr[16];
    /* Options follow */
} __attribute__((packed));

#define NDP_FLAG_ROUTER    0x80
#define NDP_FLAG_SOLICITED  0x40
#define NDP_FLAG_OVERRIDE   0x20

/* ================================================================
 * IPv6 Neighbor Cache
 * ================================================================ */
#define IPV6_NEIGHBOR_CACHE_SIZE  16

struct ipv6_neighbor_entry {
    uint8_t  ip[16];
    uint8_t  mac[6];
    int      age;
    int      valid;
    int      state;   /* 0=incomplete, 1=reachable, 2=stale */
};

static struct ipv6_neighbor_entry ipv6_neighbor_cache[IPV6_NEIGHBOR_CACHE_SIZE];
static int ipv6_neighbor_age = 0;
static spinlock_t ipv6_neighbor_lock;

/* ================================================================
 * IPv6 Interface Configuration
 * ================================================================ */
#define MAX_IPV6_IF 4

struct ipv6_iface {
    int         in_use;
    ipv6_addr_t link_local;
    ipv6_addr_t global;
    uint8_t     mac[6];
    struct net_device *netdev;
};

static struct ipv6_iface ipv6_ifaces[MAX_IPV6_IF];
static int ipv6_iface_count = 0;
static spinlock_t ipv6_iface_lock;

/* ================================================================
 * IPv6 Address Helpers
 * ================================================================ */

int ipv6_addr_from_str(const char *str, ipv6_addr_t *addr) {
    if (!str || !addr) return -1;

    memset(addr, 0, sizeof(*addr));

    /* Handle "::" shorthand */
    const char *p = str;
    int group = 0;
    int colon_colon = -1;  /* position of "::" */

    while (*p && group < 8) {
        if (*p == ':' && *(p + 1) == ':') {
            colon_colon = group;
            p += 2;
            if (*p == '\0') break;
            continue;
        }

        uint32_t val = 0;
        int digits = 0;
        while (*p && *p != ':') {
            char c = *p;
            val <<= 4;
            if (c >= '0' && c <= '9')      val += (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') val += (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val += (uint32_t)(c - 'A' + 10);
            else return -1;
            digits++;
            p++;
        }
        if (digits == 0) return -1;

        addr->addr[group * 2]     = (uint8_t)((val >> 8) & 0xFF);
        addr->addr[group * 2 + 1] = (uint8_t)(val & 0xFF);
        group++;

        if (*p == ':') p++;
        if (*p == '\0') break;
    }

    if (colon_colon >= 0 && group < 8) {
        /* Expand "::" */
        int shift = 8 - group;
        int i;
        for (i = 7; i >= colon_colon + shift; i--) {
            addr->addr[i * 2]     = addr->addr[(i - shift) * 2];
            addr->addr[i * 2 + 1] = addr->addr[(i - shift) * 2 + 1];
        }
        for (i = colon_colon; i < colon_colon + shift; i++) {
            addr->addr[i * 2]     = 0;
            addr->addr[i * 2 + 1] = 0;
        }
    }

    return 0;
}

void ipv6_addr_to_str(const ipv6_addr_t *addr, char *buf, size_t bufsz) {
    if (!addr || !buf || bufsz < 40) return;

    /* Simple hex format: xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx */
    int pos = 0;
    int i;
    for (i = 0; i < 16; i += 2) {
        uint16_t group = ((uint16_t)addr->addr[i] << 8) | addr->addr[i + 1];

        if (pos + 5 < (int)bufsz) {
            if (i > 0) {
                buf[pos++] = ':';
            }
            /* Convert hex */
            const char hex[] = "0123456789abcdef";
            if (group >= 0x1000) {
                buf[pos++] = hex[(group >> 12) & 0xF];
                buf[pos++] = hex[(group >> 8) & 0xF];
                buf[pos++] = hex[(group >> 4) & 0xF];
                buf[pos++] = hex[group & 0xF];
            } else if (group >= 0x100) {
                buf[pos++] = hex[(group >> 8) & 0xF];
                buf[pos++] = hex[(group >> 4) & 0xF];
                buf[pos++] = hex[group & 0xF];
            } else if (group >= 0x10) {
                buf[pos++] = hex[(group >> 4) & 0xF];
                buf[pos++] = hex[group & 0xF];
            } else {
                buf[pos++] = hex[group & 0xF];
            }
        }
    }
    buf[pos] = '\0';
}

/* ================================================================
 * Generate link-local IPv6 address from MAC (EUI-64)
 * ================================================================ */
static void ipv6_gen_link_local(const uint8_t mac[6], ipv6_addr_t *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->addr[0] = 0xFE;
    addr->addr[1] = 0x80;

    /* EUI-64 from MAC: insert 0xFFFE in middle, flip bit 7 of byte 0 */
    addr->addr[8]  = mac[0] ^ 0x02;  /* Flip universal/local bit */
    addr->addr[9]  = mac[1];
    addr->addr[10] = mac[2];
    addr->addr[11] = 0xFF;
    addr->addr[12] = 0xFE;
    addr->addr[13] = mac[3];
    addr->addr[14] = mac[4];
    addr->addr[15] = mac[5];
}

/* ================================================================
 * IPv6 Neighbor Cache
 * ================================================================ */

static struct ipv6_neighbor_entry *ipv6_neighbor_find(const ipv6_addr_t *ip) {
    int i;
    for (i = 0; i < IPV6_NEIGHBOR_CACHE_SIZE; i++) {
        if (ipv6_neighbor_cache[i].valid &&
            memcmp(ipv6_neighbor_cache[i].ip, ip->addr, 16) == 0) {
            ipv6_neighbor_cache[i].age = ++ipv6_neighbor_age;
            return &ipv6_neighbor_cache[i];
        }
    }
    return NULL;
}

static struct ipv6_neighbor_entry *ipv6_neighbor_add(const ipv6_addr_t *ip,
                                                      const uint8_t mac[6]) {
    int target = 0;
    int oldest_age = ipv6_neighbor_cache[0].age;
    int i;

    for (i = 0; i < IPV6_NEIGHBOR_CACHE_SIZE; i++) {
        if (!ipv6_neighbor_cache[i].valid) {
            target = i;
            break;
        }
        if (ipv6_neighbor_cache[i].age < oldest_age) {
            oldest_age = ipv6_neighbor_cache[i].age;
            target = i;
        }
    }

    memcpy(ipv6_neighbor_cache[target].ip, ip->addr, 16);
    memcpy(ipv6_neighbor_cache[target].mac, mac, 6);
    ipv6_neighbor_cache[target].age = ++ipv6_neighbor_age;
    ipv6_neighbor_cache[target].valid = 1;
    ipv6_neighbor_cache[target].state = 1;  /* reachable */
    return &ipv6_neighbor_cache[target];
}

int ndp_lookup(const ipv6_addr_t *ip, uint8_t mac_out[6]) {
    spin_lock(&ipv6_neighbor_lock);
    struct ipv6_neighbor_entry *entry = ipv6_neighbor_find(ip);
    if (entry) {
        memcpy(mac_out, entry->mac, 6);
        spin_unlock(&ipv6_neighbor_lock);
        return 0;
    }
    spin_unlock(&ipv6_neighbor_lock);

    /* Trigger neighbor solicitation */
    ndp_send_solicitation(ip);
    return -1;
}

/* ================================================================
 * NDP: Neighbor Solicitation
 * ================================================================ */
int ndp_send_solicitation(const ipv6_addr_t *target) {
    if (!target) return -1;

    /* Find an active IPv6 interface */
    struct ipv6_iface *iface = NULL;
    int i;
    for (i = 0; i < ipv6_iface_count; i++) {
        if (ipv6_ifaces[i].in_use && ipv6_ifaces[i].netdev) {
            iface = &ipv6_ifaces[i];
            break;
        }
    }
    if (!iface) return -1;

    /* Build Neighbor Solicitation packet */
    int ns_len = (int)sizeof(struct ndp_neighbor_sol);
    /* Option: Source Link-Layer Address */
    int opt_len = 2 + 6;  /* type(1) + length(1) + addr(6) */
    int total = ns_len + opt_len;

    uint8_t *packet = (uint8_t *)kmalloc((size_t)total);
    if (!packet) return -1;
    memset(packet, 0, (size_t)total);

    struct ndp_neighbor_sol *ns = (struct ndp_neighbor_sol *)packet;
    ns->reserved = 0;
    memcpy(ns->target_addr, target->addr, 16);

    /* Source Link-Layer Address option */
    uint8_t *opt = packet + ns_len;
    opt[0] = NDP_OPT_SOURCE_LLADDR;
    opt[1] = 1;  /* Length in units of 8 bytes */
    memcpy(opt + 2, iface->mac, 6);

    /* Multicast solicited-node address: ff02::1:ffxx:xxxx */
    ipv6_addr_t dst;
    memset(&dst, 0, sizeof(dst));
    dst.addr[0] = 0xFF;
    dst.addr[1] = 0x02;
    dst.addr[11] = 0x01;
    dst.addr[12] = 0xFF;
    dst.addr[13] = target->addr[13];
    dst.addr[14] = target->addr[14];
    dst.addr[15] = target->addr[15];

    /* Send via IPv6 with ICMPv6 next header */
    ipv6_send(&dst, IPV6_PROTO_ICMPV6, packet, (uint16_t)total);

    kfree(packet);
    return 0;
}

/* ================================================================
 * IPv6 Packet Sending
 * ================================================================ */
int ipv6_send(const ipv6_addr_t *dst, uint8_t next_header,
              const void *data, uint16_t len) {
    if (!dst || !data) return -1;

    /* Find an active IPv6 interface */
    struct ipv6_iface *iface = NULL;
    int i;
    for (i = 0; i < ipv6_iface_count; i++) {
        if (ipv6_ifaces[i].in_use && ipv6_ifaces[i].netdev) {
            iface = &ipv6_ifaces[i];
            break;
        }
    }
    if (!iface || !iface->netdev) return -1;

    /* Resolve destination MAC using NDP */
    uint8_t dst_mac[6];
    int ndp_ret = ndp_lookup(dst, dst_mac);
    if (ndp_ret != 0) {
        /* Use multicast MAC for solicited-node multicast */
        dst_mac[0] = 0x33;
        dst_mac[1] = 0x33;
        dst_mac[2] = 0xFF;
        dst_mac[3] = dst->addr[13];
        dst_mac[4] = dst->addr[14];
        dst_mac[5] = dst->addr[15];
    }

    int total = (int)(sizeof(struct ipv6_hdr) + len);
    uint8_t *packet = (uint8_t *)kmalloc((size_t)total);
    if (!packet) return -1;

    struct ipv6_hdr *ip6 = (struct ipv6_hdr *)packet;

    /* Version=6, Traffic Class=0, Flow Label=0 */
    ip6->version_traffic_flow[0] = 0x60;
    ip6->version_traffic_flow[1] = 0x00;
    ip6->version_traffic_flow[2] = 0x00;
    ip6->version_traffic_flow[3] = 0x00;

    /* Payload length */
    ip6->payload_len = htons(len);

    ip6->next_header = next_header;
    ip6->hop_limit = 64;

    memcpy(ip6->src_addr, iface->link_local.addr, 16);
    memcpy(ip6->dst_addr, dst->addr, 16);

    memcpy(packet + sizeof(struct ipv6_hdr), data, len);

    /* Send via Ethernet */
    int eth_total = (int)sizeof(struct eth_hdr) + total;
    uint8_t *frame = (uint8_t *)kmalloc((size_t)eth_total);
    if (!frame) {
        kfree(packet);
        return -1;
    }

    struct eth_hdr *eth = (struct eth_hdr *)frame;
    memcpy(eth->dst_mac, dst_mac, 6);
    memcpy(eth->src_mac, iface->mac, 6);
    eth->ethertype = htons(ETH_IPV6);

    memcpy(frame + sizeof(struct eth_hdr), packet, (size_t)total);

    int ret = netdev_send(iface->netdev, frame, eth_total);
    kfree(frame);
    kfree(packet);
    return ret;
}

/* ================================================================
 * IPv6 Packet Receiving
 * ================================================================ */
int ipv6_recv(void *buf, int max_len, ipv6_addr_t *src, ipv6_addr_t *dst) {
    (void)buf;
    (void)max_len;
    (void)src;
    (void)dst;
    /* Full receive path is implemented in the main net_poll loop */
    return -1;
}

/* ================================================================
 * IPv6 Packet Handler (called from Ethernet layer)
 * ================================================================ */
static void ipv6_handle_packet(struct net_device *netdev,
                                const uint8_t *data, int len) {
    (void)netdev;
    if (len < (int)sizeof(struct ipv6_hdr)) return;

    const struct ipv6_hdr *ip6 = (const struct ipv6_hdr *)data;

    uint16_t payload_len = ntohs(ip6->payload_len);
    int hdr_len = (int)sizeof(struct ipv6_hdr);

    if (payload_len > (uint16_t)(len - hdr_len)) return;

    const uint8_t *payload = data + hdr_len;

    /* Check if packet is for us */
    int for_us = 0;
    int i;
    for (i = 0; i < ipv6_iface_count; i++) {
        if (ipv6_ifaces[i].in_use) {
            if (memcmp(ipv6_ifaces[i].link_local.addr, ip6->dst_addr, 16) == 0) {
                for_us = 1;
                break;
            }
        }
    }

    /* Also accept multicast */
    if (ip6->dst_addr[0] == 0xFF) {
        for_us = 1;
    }

    if (!for_us) return;

    /* Handle ICMPv6 */
    if (ip6->next_header == IPV6_PROTO_ICMPV6) {
        if (payload_len < 4) return;

        uint8_t icmp_type = payload[0];

        switch (icmp_type) {
        case ICMPV6_ECHO_REQUEST: {
            /* Build echo reply */
            int reply_len = (int)payload_len;
            uint8_t *reply = (uint8_t *)kmalloc((size_t)reply_len);
            if (!reply) return;
            memcpy(reply, payload, (size_t)reply_len);

            /* Change type to Echo Reply */
            reply[0] = ICMPV6_ECHO_REPLY;
            /* Clear checksum */
            reply[2] = 0;
            reply[3] = 0;

            /* Build ICMPv6 pseudo-header checksum */
            uint32_t sum = 0;
            /* Source address */
            for (i = 0; i < 8; i++) {
                sum += ((uint16_t)ip6->src_addr[i * 2] << 8) | ip6->src_addr[i * 2 + 1];
            }
            /* Destination address */
            for (i = 0; i < 8; i++) {
                sum += ((uint16_t)ip6->dst_addr[i * 2] << 8) | ip6->dst_addr[i * 2 + 1];
            }
            /* Upper-layer packet length */
            sum += (uint32_t)payload_len;
            /* Next header */
            sum += (uint32_t)IPV6_PROTO_ICMPV6;

            /* ICMPv6 data */
            for (i = 0; i < reply_len / 2; i++) {
                sum += ((uint16_t)reply[i * 2] << 8) | reply[i * 2 + 1];
            }
            if (reply_len & 1) {
                sum += (uint16_t)reply[reply_len - 1] << 8;
            }

            while (sum >> 16) {
                sum = (sum & 0xFFFF) + (sum >> 16);
            }
            uint16_t csum = (uint16_t)(~sum);
            reply[2] = (uint8_t)((csum >> 8) & 0xFF);
            reply[3] = (uint8_t)(csum & 0xFF);

            /* Build source address for reply */
            ipv6_addr_t reply_src;
            memcpy(reply_src.addr, ip6->dst_addr, 16);

            /* Build destination for reply */
            ipv6_addr_t reply_dst;
            memcpy(reply_dst.addr, ip6->src_addr, 16);

            ipv6_send(&reply_dst, IPV6_PROTO_ICMPV6, reply, payload_len);

            kfree(reply);
            break;
        }

        case ICMPV6_ND_NEIGHBOR_SOL: {
            /* Handle Neighbor Solicitation */
            if (payload_len < (int)sizeof(struct ndp_neighbor_sol)) break;

            const struct ndp_neighbor_sol *ns =
                (const struct ndp_neighbor_sol *)payload;

            /* Check if target is our address */
            int target_is_ours = 0;
            for (i = 0; i < ipv6_iface_count; i++) {
                if (ipv6_ifaces[i].in_use &&
                    memcmp(ipv6_ifaces[i].link_local.addr,
                           ns->target_addr, 16) == 0) {
                    target_is_ours = 1;
                    break;
                }
            }

            if (!target_is_ours) break;

            /* Parse Source Link-Layer Address option */
            int opt_offset = (int)sizeof(struct ndp_neighbor_sol);
            uint8_t src_mac[6] = {0};
            int has_src_mac = 0;

            if (opt_offset + 8 <= (int)payload_len) {
                const uint8_t *opt = payload + opt_offset;
                if (opt[0] == NDP_OPT_SOURCE_LLADDR) {
                    memcpy(src_mac, opt + 2, 6);
                    has_src_mac = 1;
                }
            }

            /* Update neighbor cache */
            if (has_src_mac) {
                ipv6_addr_t src_addr;
                memcpy(src_addr.addr, ip6->src_addr, 16);
                spin_lock(&ipv6_neighbor_lock);
                ipv6_neighbor_add(&src_addr, src_mac);
                spin_unlock(&ipv6_neighbor_lock);
            }

            /* Build Neighbor Advertisement */
            int adv_len = (int)sizeof(struct ndp_neighbor_adv);
            int adv_opt_len = 2 + 6;  /* Target Link-Layer Address */
            int adv_total = adv_len + adv_opt_len;

            uint8_t *adv_pkt = (uint8_t *)kmalloc((size_t)adv_total);
            if (!adv_pkt) break;
            memset(adv_pkt, 0, (size_t)adv_total);

            struct ndp_neighbor_adv *na = (struct ndp_neighbor_adv *)adv_pkt;
            na->flags = NDP_FLAG_SOLICITED | NDP_FLAG_OVERRIDE;
            memcpy(na->target_addr, ns->target_addr, 16);

            /* Target Link-Layer Address option */
            uint8_t *adv_opt = adv_pkt + adv_len;
            adv_opt[0] = NDP_OPT_TARGET_LLADDR;
            adv_opt[1] = 1;  /* Length = 1 (8 bytes) */

            /* Find our MAC */
            for (i = 0; i < ipv6_iface_count; i++) {
                if (ipv6_ifaces[i].in_use &&
                    memcmp(ipv6_ifaces[i].link_local.addr,
                           ns->target_addr, 16) == 0) {
                    memcpy(adv_opt + 2, ipv6_ifaces[i].mac, 6);
                    break;
                }
            }

            ipv6_addr_t adv_dst;
            memcpy(adv_dst.addr, ip6->src_addr, 16);

            ipv6_send(&adv_dst, IPV6_PROTO_ICMPV6, adv_pkt, (uint16_t)adv_total);

            kfree(adv_pkt);
            break;
        }

        case ICMPV6_ND_NEIGHBOR_ADV: {
            /* Handle Neighbor Advertisement */
            if (payload_len < (int)sizeof(struct ndp_neighbor_adv)) break;

            const struct ndp_neighbor_adv *na =
                (const struct ndp_neighbor_adv *)payload;

            /* Parse Target Link-Layer Address option */
            int opt_offset = (int)sizeof(struct ndp_neighbor_adv);
            if (opt_offset + 8 <= (int)payload_len) {
                const uint8_t *opt = payload + opt_offset;
                if (opt[0] == NDP_OPT_TARGET_LLADDR) {
                    ipv6_addr_t na_addr;
                    memcpy(na_addr.addr, na->target_addr, 16);

                    spin_lock(&ipv6_neighbor_lock);
                    ipv6_neighbor_add(&na_addr, opt + 2);
                    spin_unlock(&ipv6_neighbor_lock);
                }
            }
            break;
        }

        default:
            break;
        }
    }
}

/* ================================================================
 * IPv6 Initialization
 * ================================================================ */
void ndp_init(void) {
    /* Already initialized as part of ipv6_init */
}

void ipv6_init(void) {
    log_printf(LOG_LEVEL_INFO, "ipv6: initializing IPv6 stack\n");

    spin_init(&ipv6_neighbor_lock);
    spin_init(&ipv6_iface_lock);

    memset(ipv6_neighbor_cache, 0, sizeof(ipv6_neighbor_cache));
    memset(ipv6_ifaces, 0, sizeof(ipv6_ifaces));

    /* Discover network devices and assign link-local addresses */
    struct net_device *netdev = netdev_get_first();
    while (netdev && ipv6_iface_count < MAX_IPV6_IF) {
        /* Skip loopback */
        if (strcmp(netdev->name, "lo") == 0) {
            netdev = netdev->next;
            continue;
        }

        if (ipv6_iface_count < MAX_IPV6_IF) {
            struct ipv6_iface *iface = &ipv6_ifaces[ipv6_iface_count];
            memset(iface, 0, sizeof(*iface));
            iface->in_use = 1;
            iface->netdev = netdev;
            memcpy(iface->mac, netdev->mac, 6);

            /* Generate link-local address from MAC */
            ipv6_gen_link_local(iface->mac, &iface->link_local);

            char addr_str[40];
            ipv6_addr_to_str(&iface->link_local, addr_str, sizeof(addr_str));

            log_printf(LOG_LEVEL_INFO,
                       "ipv6: interface '%s' link-local=%s\n",
                       netdev->name, addr_str);

            ipv6_iface_count++;
        }

        netdev = netdev->next;
    }

    log_printf(LOG_LEVEL_INFO, "ipv6: initialized %d interface(s)\n",
               ipv6_iface_count);
}