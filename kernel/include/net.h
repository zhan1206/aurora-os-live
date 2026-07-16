/*
 * net.h - TCP/IP Network Protocol Stack Header
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>

/* Network byte order helpers */
static inline uint16_t ntohs(uint16_t n) {
    return ((n & 0xFF) << 8) | ((n >> 8) & 0xFF);
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
 * Ethernet
 * ================================================================ */
#define ETH_IPV4  0x0800
#define ETH_ARP   0x0806
#define ETH_IPV6  0x86DD

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
 * TCP Options (RFC 1323 / RFC 2018)
 * ================================================================ */
#define TCP_OPT_EOL            0
#define TCP_OPT_NOP            1
#define TCP_OPT_MSS            2
#define TCP_OPT_WINDOW_SCALE   3
#define TCP_OPT_SACK_PERM      4
#define TCP_OPT_SACK           5
#define TCP_OPT_TIMESTAMP      8

struct tcp_option {
    uint8_t  kind;
    uint8_t  length;
    uint8_t  data[];
} __attribute__((packed));

/* TCP option MSS (kind=2, length=4) */
struct tcp_opt_mss {
    uint8_t  kind;
    uint8_t  length;
    uint16_t mss;
} __attribute__((packed));

/* TCP option Window Scale (kind=3, length=3) */
struct tcp_opt_window_scale {
    uint8_t  kind;
    uint8_t  length;
    uint8_t  shift_cnt;
} __attribute__((packed));

/* TCP option SACK Permitted (kind=4, length=2) */
struct tcp_opt_sack_perm {
    uint8_t  kind;
    uint8_t  length;
} __attribute__((packed));

/* TCP option Timestamp (kind=8, length=10) */
struct tcp_opt_timestamp {
    uint8_t  kind;
    uint8_t  length;
    uint32_t ts_val;
    uint32_t ts_ecr;
} __attribute__((packed));

/* TCP option SACK (kind=5, variable length) */
struct tcp_opt_sack {
    uint8_t  kind;
    uint8_t  length;
    /* Followed by 8-byte blocks (left_edge, right_edge) */
} __attribute__((packed));

/* TCP congestion control states */
enum tcp_cong_state {
    TCP_CONG_SLOW_START,
    TCP_CONG_AVOIDANCE,
    TCP_CONG_RECOVERY
};

/* ================================================================
 * IPv6 (RFC 2460)
 * ================================================================ */
#define IPV6_ADDR_LEN 16

typedef struct ipv6_addr {
    uint8_t  addr[16];
} ipv6_addr_t;

struct ipv6_hdr {
    uint8_t  version_traffic_flow[4];  /* Version(4), Traffic Class(8), Flow Label(20) */
    uint16_t payload_len;
    uint8_t  next_header;
    uint8_t  hop_limit;
    uint8_t  src_addr[16];
    uint8_t  dst_addr[16];
} __attribute__((packed));

#define IPV6_PROTO_ICMPV6  58
#define IPV6_PROTO_TCP      6
#define IPV6_PROTO_UDP     17

/* IPv6 address helpers */
#define IPV6_IS_UNSPECIFIED(a)  \
    (memcmp((a)->addr, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0)
#define IPV6_IS_LOOPBACK(a)     \
    (memcmp((a)->addr, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x01", 16) == 0)
#define IPV6_IS_LINKLOCAL(a)    \
    (((a)->addr[0] == 0xFE) && ((a)->addr[1] == 0x80))

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
    uint8_t  dns_server[4];
    int      mtu;
    int      flags;
    struct net_device *netdev;
};

/* ================================================================
 * DHCP (RFC 2131)
 * ================================================================ */
#define DHCP_CLIENT_PORT  68
#define DHCP_SERVER_PORT  67
#define DHCP_MAGIC_COOKIE 0x63825363

#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_ACK       5
#define DHCP_NAK       6

#define DHCP_OPT_SUBNET_MASK  1
#define DHCP_OPT_ROUTER       3
#define DHCP_OPT_DNS          6
#define DHCP_OPT_REQ_IP       50
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_END          255

struct dhcp_hdr {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
} __attribute__((packed));

/* ================================================================
 * DNS
 * ================================================================ */
#define DNS_PORT           53
#define DNS_QRY_STANDARD   0x0100
#define DNS_TYPE_A         1
#define DNS_CLASS_IN       1

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

/* ================================================================
 * HTTP
 * ================================================================ */
#define HTTP_DEFAULT_PORT  80

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

/* Socket name resolution */
int  tcp_getsockname(int sock, uint8_t local_ip[4], uint16_t *local_port);
int  tcp_getpeername(int sock, uint8_t remote_ip[4], uint16_t *remote_port);

/* Utility */
struct net_if *net_get_interface(int index);
int net_get_interface_count(void);

/* ================================================================
 * IPv6 API
 * ================================================================ */
void ipv6_init(void);
int  ipv6_addr_from_str(const char *str, ipv6_addr_t *addr);
void ipv6_addr_to_str(const ipv6_addr_t *addr, char *buf, size_t bufsz);
int  ipv6_send(const ipv6_addr_t *dst, uint8_t next_header,
               const void *data, uint16_t len);
int  ipv6_recv(void *buf, int max_len, ipv6_addr_t *src, ipv6_addr_t *dst);

/* IPv6 Neighbor Discovery */
void ndp_init(void);
int  ndp_lookup(const ipv6_addr_t *ip, uint8_t mac_out[6]);
int  ndp_send_solicitation(const ipv6_addr_t *target);

/* IPv6 packet handler (called from Ethernet layer) */
void ipv6_handle_packet(struct net_device *netdev, const uint8_t *data, int len);

/* ================================================================
 * TCP Congestion Control API
 * ================================================================ */
void tcp_cong_init(void);
void tcp_cong_on_ack(int sock, uint32_t ack_seq, int dup_ack_count);
void tcp_cong_on_timeout(int sock);

/* DHCP */
int dhcp_init(void);
int dhcp_run(void);

/* DNS */
int  dns_query(const char *hostname, uint8_t ip_out[4]);
void dns_set_server(const uint8_t ip[4]);

/* HTTP */
int  http_get(const char *url, char *response_buf, size_t buf_size);

#endif /* NET_H */