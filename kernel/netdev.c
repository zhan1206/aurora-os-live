/*
 * netdev.c - Network device abstraction layer implementation
 */
#include "netdev.h"
#include "include/log.h"
#include "include/string.h"
#include <stdint.h>

static struct net_device *netdev_list = NULL;

int netdev_register(struct net_device *netdev) {
    if (!netdev) return -1;

    netdev->next = netdev_list;
    netdev_list = netdev;

    log_printf(LOG_LEVEL_INFO,
               "net: registered '%s' (mac=%02x:%02x:%02x:%02x:%02x:%02x mtu=%d)\n",
               netdev->name,
               netdev->mac[0], netdev->mac[1], netdev->mac[2],
               netdev->mac[3], netdev->mac[4], netdev->mac[5],
               netdev->mtu);
    return 0;
}

struct net_device *netdev_get_first(void) {
    return netdev_list;
}

struct net_device *netdev_find(const char *name) {
    if (!name) return NULL;
    struct net_device *netdev = netdev_list;
    while (netdev) {
        if (strcmp(netdev->name, name) == 0) return netdev;
        netdev = netdev->next;
    }
    return NULL;
}

int netdev_send(struct net_device *netdev, const void *data, int len) {
    if (!netdev || !netdev->send || !data) return -1;
    if (len <= 0) return -1;
    return netdev->send(netdev, data, len);
}

int netdev_recv(struct net_device *netdev, void *buf, int max_len) {
    if (!netdev || !netdev->recv || !buf) return -1;
    if (max_len <= 0) return -1;
    return netdev->recv(netdev, buf, max_len);
}