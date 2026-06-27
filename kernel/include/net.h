/*
 * net.h - TCP/IP Network Protocol Stack Header
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>

/* ================================================================
 * Ethernet
 * ================================================================ */
#define ETH_IPV4  0x0800
#define ETH_ARP   0x0806

struct eth_hdr {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} __attribute__((packed));

/* ================================================================
 * ARP
 * ================================================================ */
#define ARP_REQUEST  1
#define ARP_REPLY    2
#define ARP_HTYPE_ETH 1

struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} __attribute__((packed));

/* ================================================================
 * IPv4
 * ================================================================ */
#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

struct ipv4_hdr {
    uint8_t  version_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} __attribute__((packed));

/* ================================================================
 * ICMP
 * ================================================================ */
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    union {
        uint32_t rest;
        struct {
            uint16_t id;
            uint16_t seq;
        } echo;
    };
} __attribute__((packed));

/* ================================================================
 * UDP
 * ================================================================ */
struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

/* ================================================================
 * TCP
 * ================================================================ */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t data_offset_flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

/* TCP states */
enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
};

/* ================================================================
 * Network Interface
 * ================================================================ */
#define NETIF_FLAG_UP      0x0001
#define NETIF_FLAG_RUNNING 0x0002

struct net_if {
    char     name[32];
    uint8_t  ip[4];
    uint8_t  mac[6];
    uint8_t  netmask[4];
    uint8_t  gateway[4];
    int      mtu;
    int      flags;
    struct net_device *netdev;
};

/* ================================================================
 * Network API
 * ================================================================ */
void net_init(void);
void net_poll(void);

/* ARP */
int arp_lookup(const uint8_t ip[4], uint8_t mac_out[6]);

/* IPv4 */
int ip_send(const uint8_t dst_ip[4], uint8_t protocol,
            const void *data, uint16_t len);

/* UDP */
int udp_send(uint16_t src_port, const uint8_t dst_ip[4],
             uint16_t dst_port, const void *data, uint16_t len);

/* TCP */
int  tcp_socket_create(void);
int  tcp_connect(int sock, const uint8_t dst_ip[4], uint16_t dst_port);
int  tcp_send(int sock, const void *data, int len);
int  tcp_recv(int sock, void *buf, int max_len);
int  tcp_close(int sock);
int  tcp_bind(int sock, uint16_t port);
int  tcp_accept(int sock, uint8_t remote_ip[4], uint16_t *remote_port);
int  tcp_listen(int sock, int backlog);
int  tcp_shutdown(int sock, int how);

/* UDP recvfrom */
int  udp_recvfrom(uint16_t port, void *buf, int max_len,
                  uint8_t src_ip[4], uint16_t *src_port);

/* Utility */
struct net_if *net_get_interface(int index);
int net_get_interface_count(void);

#endif /* NET_H */