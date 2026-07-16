/*
 * dhcp.c - DHCP Client (RFC 2131)
 */

#include "net.h"
#include "log.h"
#include "string.h"
#include "../mem.h"
#include <stdint.h>

/* Byte order conversion now in net.h */

/* DHCP client state */
static uint32_t dhcp_xid = 0;
static uint8_t  dhcp_offered_ip[4];
static uint8_t  dhcp_server_ip[4];
static int      dhcp_initialized = 0;

/* ================================================================
 * dhcp_init
 * ================================================================ */
int dhcp_init(void) {
    dhcp_xid = 0x12345678;
    memset(dhcp_offered_ip, 0, 4);
    memset(dhcp_server_ip, 0, 4);
    dhcp_initialized = 1;
    log_printf(LOG_LEVEL_INFO, "dhcp: client initialized\n");

    /* Auto-run DHCP if there is a network interface available */
    if (net_get_interface_count() > 0) {
        dhcp_run();
    }

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
 * dhcp_discover - Send DHCP DISCOVER broadcast
 * ================================================================ */
static int dhcp_discover(void) {
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
 * dhcp_handle_offer - Process DHCP OFFER
 * ================================================================ */
static int dhcp_handle_offer(void) {
    uint8_t buf[600];
    uint8_t src_ip[4];
    uint16_t src_port;
    int retry;

    for (retry = 0; retry < 50; retry++) {
        net_poll();

        int len = udp_recvfrom(DHCP_CLIENT_PORT, buf, (int)sizeof(buf),
                                src_ip, &src_port);
        if (len <= 0) continue;

        if (len < (int)sizeof(struct dhcp_hdr)) continue;

        struct dhcp_hdr *dhcp = (struct dhcp_hdr *)buf;
        if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE) continue;
        if (ntohl(dhcp->xid) != dhcp_xid) continue;
        if (ntohs(src_port) != DHCP_SERVER_PORT) continue;

        /* Check DHCP message type */
        uint8_t msg_type = 0;
        if (dhcp_find_option(buf, len, DHCP_OPT_MSG_TYPE,
                              &msg_type, 1) != 1) continue;
        if (msg_type != DHCP_OFFER) continue;

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

    log_printf(LOG_LEVEL_ERR, "dhcp: no OFFER received\n");
    return -1;
}

/* ================================================================
 * dhcp_request - Send DHCP REQUEST
 * ================================================================ */
static int dhcp_request(void) {
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
 * dhcp_handle_ack - Process DHCP ACK
 *
 * NOTE: Known limitation - this function does not cancel a retransmit
 * timer. A full implementation would track retransmit timers and
 * cancel them upon successful ACK receipt.
 * ================================================================ */
static int dhcp_handle_ack(void) {
    uint8_t buf[600];
    uint8_t src_ip[4];
    uint16_t src_port;
    int retry;

    for (retry = 0; retry < 50; retry++) {
        net_poll();

        int len = udp_recvfrom(DHCP_CLIENT_PORT, buf, (int)sizeof(buf),
                                src_ip, &src_port);
        if (len <= 0) continue;

        if (len < (int)sizeof(struct dhcp_hdr)) continue;

        struct dhcp_hdr *dhcp = (struct dhcp_hdr *)buf;
        if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE) continue;
        if (ntohl(dhcp->xid) != dhcp_xid) continue;
        if (ntohs(src_port) != DHCP_SERVER_PORT) continue;

        uint8_t msg_type = 0;
        if (dhcp_find_option(buf, len, DHCP_OPT_MSG_TYPE,
                              &msg_type, 1) != 1) continue;

        if (msg_type == DHCP_NAK) {
            log_printf(LOG_LEVEL_ERR, "dhcp: NAK received\n");
            return -1;
        }

        if (msg_type != DHCP_ACK) continue;

        /* Configure network interface */
        struct net_if *iface = net_get_interface(0);
        if (!iface) return -1;

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

        log_printf(LOG_LEVEL_INFO,
                   "dhcp: ACK received, ip=%d.%d.%d.%d "
                   "mask=%d.%d.%d.%d gw=%d.%d.%d.%d dns=%d.%d.%d.%d\n",
                   iface->ip[0], iface->ip[1], iface->ip[2], iface->ip[3],
                   iface->netmask[0], iface->netmask[1],
                   iface->netmask[2], iface->netmask[3],
                   iface->gateway[0], iface->gateway[1],
                   iface->gateway[2], iface->gateway[3],
                   iface->dns_server[0], iface->dns_server[1],
                   iface->dns_server[2], iface->dns_server[3]);
        return 0;
    }

    log_printf(LOG_LEVEL_ERR, "dhcp: no ACK received\n");
    return -1;
}

/* ================================================================
 * dhcp_run - Full DHCP state machine
 * ================================================================ */
int dhcp_run(void) {
    if (!dhcp_initialized) {
        dhcp_init();
    }

    /* DISCOVER */
    if (dhcp_discover() < 0) return -1;

    /* Wait for OFFER */
    if (dhcp_handle_offer() < 0) return -1;

    /* REQUEST */
    if (dhcp_request() < 0) return -1;

    /* Wait for ACK */
    if (dhcp_handle_ack() < 0) return -1;

    log_printf(LOG_LEVEL_INFO, "dhcp: configuration complete\n");
    return 0;
}