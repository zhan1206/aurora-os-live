/*
 * netdev.h - Network device abstraction layer
 */
#ifndef NETDEV_H
#define NETDEV_H

#include <stdint.h>

/* ================================================================
 * Network Device Structure
 * ================================================================ */
struct net_device {
    char     name[32];
    uint8_t  mac[6];
    int      mtu;
    int      flags;

    /* Operations */
    int  (*send)(struct net_device *netdev, const void *data, int len);
    int  (*recv)(struct net_device *netdev, void *buf, int max_len);
    int  (*up)(struct net_device *netdev);
    int  (*down)(struct net_device *netdev);

    void *priv;                /* driver-private data */

    struct net_device *next;
};

/* Network device flags */
#define NETDEV_FLAG_UP      0x0001
#define NETDEV_FLAG_RUNNING 0x0002

/* ================================================================
 * Network Device API
 * ================================================================ */
int  netdev_register(struct net_device *netdev);
struct net_device *netdev_find(const char *name);
int  netdev_send(struct net_device *netdev, const void *data, int len);
int  netdev_recv(struct net_device *netdev, void *buf, int max_len);

#endif /* NETDEV_H */