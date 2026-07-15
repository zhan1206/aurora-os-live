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
#include <stdint.h>

/* ================================================================
 * Byte Order Conversion
 * ================================================================ */
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
    if (len > 0) {
        sum += *(const uint8_t *)ptr;
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
        sum += *(const uint8_t *)ptr;
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

static struct arp_entry *arp_cache_find(const uint8_t ip[4]) {
    int i;
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            arp_cache[i].age = ++arp_age_counter;
            return &arp_cache[i];
        }
    }
    return NULL;
}

static struct arp_entry *arp_cache_add(const uint8_t ip[4],
                                        const uint8_t mac[6]) {
    /* Find an empty or oldest entry */
    int target = 0;
    int oldest_age = arp_cache[0].age;
    int i;

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
    return &arp_cache[target];
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
    struct arp_entry *entry = arp_cache_find(ip);
    if (entry) {
        memcpy(mac_out, entry->mac, 6);
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
static uint16_t ip_id_counter = 0;

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
        /* ARP not resolved yet, try broadcast */
        memset(dst_mac, 0xFF, 6);
    }

    int total = (int)(sizeof(struct ipv4_hdr) + len);
    if (total > 65535) total = 65535;
    uint8_t *packet = (uint8_t *)kmalloc((size_t)total);
    if (!packet) return -1;

    struct ipv4_hdr *ip = (struct ipv4_hdr *)packet;
    ip->version_ihl = 0x45; /* Version 4, IHL = 5 (20 bytes) */
    ip->dscp_ecn = 0;
    ip->total_len = htons((uint16_t)total);
    ip->id = htons(ip_id_counter++);
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
        udp_handle_packet(ip->src_ip, payload, payload_len);
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
struct udp_socket {
    uint16_t local_port;
    int      in_use;
    uint8_t  rx_buf[2048];
    int      rx_len;
    uint8_t  rx_src_ip[4];
    uint16_t rx_src_port;
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
                               const uint8_t *data, int len) {
    if (len < (int)sizeof(struct udp_hdr)) return;

    const struct udp_hdr *udp = (const struct udp_hdr *)data;
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    int data_len = len - (int)sizeof(struct udp_hdr);

    spin_lock(&udp_lock);

    int i;
    for (i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_sockets[i].in_use && udp_sockets[i].local_port == dst_port) {
            int copy_len = data_len;
            if (copy_len > (int)sizeof(udp_sockets[i].rx_buf)) {
                copy_len = (int)sizeof(udp_sockets[i].rx_buf);
            }
            if (copy_len > 0) {
                memcpy(udp_sockets[i].rx_buf,
                       data + sizeof(struct udp_hdr), (size_t)copy_len);
            }
            udp_sockets[i].rx_len = copy_len;
            memcpy(udp_sockets[i].rx_src_ip, src_ip, 4);
            udp_sockets[i].rx_src_port = src_port;
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
            udp_sockets[i].rx_len > 0) {
            int copy_len = udp_sockets[i].rx_len;
            if (copy_len > max_len) copy_len = max_len;
            memcpy(buf, udp_sockets[i].rx_buf, (size_t)copy_len);
            if (src_ip) memcpy(src_ip, udp_sockets[i].rx_src_ip, 4);
            if (src_port) *src_port = udp_sockets[i].rx_src_port;
            udp_sockets[i].rx_len = 0;
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
};

static struct tcp_socket tcp_sockets[MAX_TCP_SOCKETS];
static spinlock_t tcp_lock;
static int tcp_next_id = 1;
static uint32_t tcp_iss = 0; /* initial send sequence counter */

static uint32_t tcp_get_iss(void) {
    tcp_iss += 0x10000;
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
    tcp->window = htons(8192);
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

    spin_unlock(&tcp_lock);

    /* Send SYN */
    tcp_send_packet(s, TCP_SYN, NULL, 0);
    return 0;
}

int tcp_send(int sock, const void *data, int len) {
    spin_lock(&tcp_lock);
    struct tcp_socket *s = tcp_find_socket(sock);
    if (!s) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) {
        spin_unlock(&tcp_lock);
        return -1;
    }

    if (len <= 0) {
        spin_unlock(&tcp_lock);
        return 0;
    }

    int send_len = len;
    if (send_len > TCP_MAX_SEGMENT) send_len = TCP_MAX_SEGMENT;

    spin_unlock(&tcp_lock);

    int ret = tcp_send_packet(s, TCP_PSH | TCP_ACK, data, send_len);
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
        spin_unlock(&tcp_lock);
        tcp_send_packet(s, TCP_FIN | TCP_ACK, NULL, 0);
        return 0;
    }

    if (s->state == TCP_CLOSE_WAIT) {
        s->state = TCP_LAST_ACK;
        spin_unlock(&tcp_lock);
        tcp_send_packet(s, TCP_FIN | TCP_ACK, NULL, 0);
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
        spin_unlock(&tcp_lock);
        tcp_send_packet(s, TCP_FIN | TCP_ACK, NULL, 0);
        return 0;
    }

    if (s->state == TCP_CLOSE_WAIT) {
        s->state = TCP_LAST_ACK;
        spin_unlock(&tcp_lock);
        tcp_send_packet(s, TCP_FIN | TCP_ACK, NULL, 0);
        return 0;
    }

    spin_unlock(&tcp_lock);
    return -1;
}

int tcp_getsockname(int sock, uint8_t local_ip[4], uint16_t *local_port) {
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

            spin_unlock(&tcp_lock);

            /* Send SYN-ACK */
            tcp_send_packet(&tcp_sockets[slot], TCP_SYN | TCP_ACK, NULL, 0);
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
            spin_unlock(&tcp_lock);

            /* Send ACK */
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
            log_printf(LOG_LEVEL_DEBUG,
                       "tcp: connection established sock=%d\n", sock->id);
        } else if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            spin_unlock(&tcp_lock);
        } else {
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_SYN_RECEIVED:
        if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            spin_unlock(&tcp_lock);
            break;
        }
        if (flags & TCP_ACK) {
            /* Third handshake: ACK received, connection established */
            sock->ack_num = seq;
            sock->state = TCP_ESTABLISHED;
            spin_unlock(&tcp_lock);
            log_printf(LOG_LEVEL_DEBUG,
                       "tcp: server connection established sock=%d\n", sock->id);
        } else {
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            spin_unlock(&tcp_lock);
            break;
        }

        if (flags & TCP_FIN) {
            sock->ack_num = seq + 1;
            sock->state = TCP_CLOSE_WAIT;
            spin_unlock(&tcp_lock);

            /* Send ACK for the FIN */
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
            break;
        }

        if (payload_len > 0) {
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
            spin_unlock(&tcp_lock);

            /* Send ACK */
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
        } else {
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            spin_unlock(&tcp_lock);
            break;
        }

        if ((flags & (TCP_FIN | TCP_ACK)) == (TCP_FIN | TCP_ACK)) {
            sock->ack_num = seq + 1;
            sock->state = TCP_TIME_WAIT;
            spin_unlock(&tcp_lock);
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
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
            spin_unlock(&tcp_lock);
            break;
        }

        if (flags & TCP_FIN) {
            sock->ack_num = seq + 1;
            sock->state = TCP_TIME_WAIT;
            spin_unlock(&tcp_lock);
            tcp_send_packet(sock, TCP_ACK, NULL, 0);
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
            spin_unlock(&tcp_lock);
            break;
        }

        if (flags & TCP_ACK) {
            sock->state = TCP_CLOSED;
            spin_unlock(&tcp_lock);
        } else {
            spin_unlock(&tcp_lock);
        }
        break;

    case TCP_TIME_WAIT:
        /* Just acknowledge and stay in TIME_WAIT */
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
#define LOOPBACK_BUF_SIZE 8192

static uint8_t loopback_buf[LOOPBACK_BUF_SIZE];
static int loopback_buf_len = 0;
static spinlock_t loopback_lock;

static int loopback_send(struct net_device *netdev, const void *data, int len) {
    (void)netdev;
    spin_lock(&loopback_lock);
    if (len > LOOPBACK_BUF_SIZE) len = LOOPBACK_BUF_SIZE;
    memcpy(loopback_buf, data, (size_t)len);
    loopback_buf_len = len;
    spin_unlock(&loopback_lock);
    return len;
}

static int loopback_recv(struct net_device *netdev, void *buf, int max_len) {
    (void)netdev;
    spin_lock(&loopback_lock);
    if (loopback_buf_len <= 0) {
        spin_unlock(&loopback_lock);
        return 0;
    }
    int copy_len = loopback_buf_len;
    if (copy_len > max_len) copy_len = max_len;
    memcpy(buf, loopback_buf, (size_t)copy_len);
    loopback_buf_len = 0;
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
 * Main Initialization & Polling
 * ================================================================ */
void net_init(void) {
    log_printf(LOG_LEVEL_INFO, "net: initializing TCP/IP stack\n");

    spin_init(&udp_lock);
    spin_init(&tcp_lock);
    spin_init(&loopback_lock);

    memset(net_ifs, 0, sizeof(net_ifs));
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(udp_sockets, 0, sizeof(udp_sockets));
    memset(tcp_sockets, 0, sizeof(tcp_sockets));
    memset(loopback_buf, 0, sizeof(loopback_buf));

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

    /* Try to obtain IP via DHCP */
    dhcp_run();
}

void net_poll(void) {
    uint8_t buf[2048];
    int i;

    for (i = 0; i < net_if_count; i++) {
        if (!(net_ifs[i].flags & NETIF_FLAG_UP)) continue;
        if (!net_ifs[i].netdev) continue;

        int len = eth_recv(net_ifs[i].netdev, buf, (int)sizeof(buf));
        if (len > 0) {
            process_eth_frame(net_ifs[i].netdev, buf, len);
        }
    }
}