/*
 * virtio_net.c - VirtIO Network Device Driver (PCI transport)
 *
 * Implements a basic VirtIO 1.0 network device driver.
 * Supports packet send/receive and device registration.
 */
#include "virtio.h"
#include "pci.h"
#include "netdev.h"
#include "include/portio.h"
#include "include/log.h"
#include "include/string.h"
#include "mem.h"
#include <stdint.h>

/* ================================================================
 * VirtIO PCI Vendor/Device IDs for Network
 * ================================================================ */
#define VIRTIO_PCI_VENDOR_ID       0x1AF4
#define VIRTIO_PCI_DEVICE_NET_MODERN 0x1041
#define VIRTIO_PCI_DEVICE_NET_LEGACY 0x1000

#define VIRTIO_NET_RX_QUEUE        0
#define VIRTIO_NET_TX_QUEUE        1
#define VIRTIO_NET_MAX_PACKET      1514

/* VirtIO PCI capability vendor-specific CFG type */
#define VIRTIO_PCI_CAP_VENDOR_CFG  0x09

/* ================================================================
 * VirtIO PCI Capability Structure (in PCI config space)
 * ================================================================ */
struct virtio_pci_cap {
    uint8_t  cap_vndr;
    uint8_t  cap_next;
    uint8_t  cap_len;
    uint8_t  cfg_type;
    uint8_t  bar;
    uint8_t  id;
    uint8_t  padding[2];
    uint32_t offset;
    uint32_t length;
};

/* ================================================================
 * VirtIO Network Device Private Data
 * ================================================================ */
struct virtio_net_dev {
    struct pci_device *pci;
    struct net_device netdev;

    int is_legacy;

    /* MMIO base addresses */
    volatile uint32_t *common_cfg;
    volatile uint32_t *notify_cfg;
    volatile uint32_t *isr_cfg;
    volatile uint8_t  *device_cfg;

    uint32_t notify_off_multiplier;

    /* Virtqueues */
    struct virtq rx_vq;
    struct virtq tx_vq;
    int rx_ready;
    int tx_ready;

    /* Receive buffers */
    void *rx_buffers[VIRTIO_VQ_MAX_SIZE];
    int   rx_buffer_count;

    struct virtio_net_dev *next;
};

static struct virtio_net_dev *net_dev_list = NULL;

/* ================================================================
 * MMIO Access Helpers
 * ================================================================ */

static inline uint8_t mmio_read8(volatile uint8_t *addr) {
    return *addr;
}

static inline uint16_t mmio_read16(volatile uint16_t *addr) {
    return *addr;
}

static inline uint32_t mmio_read32(volatile uint32_t *addr) {
    return *addr;
}

static inline uint64_t mmio_read64(volatile uint64_t *addr) {
    return *addr;
}

static inline void mmio_write8(volatile uint8_t *addr, uint8_t val) {
    *addr = val;
}

static inline void mmio_write16(volatile uint16_t *addr, uint16_t val) {
    *addr = val;
}

static inline void mmio_write32(volatile uint32_t *addr, uint32_t val) {
    *addr = val;
}

static inline void mmio_write64(volatile uint64_t *addr, uint64_t val) {
    *addr = val;
}

/* ================================================================
 * VirtIO PCI Transport Helpers
 * ================================================================ */

static uint8_t virtio_pci_read_device_status(struct virtio_net_dev *dev) {
    if (dev->is_legacy) {
        return pci_read_config8(dev->pci->bus, dev->pci->device,
                                dev->pci->function, 0x14);
    }
    return mmio_read8(&((volatile uint8_t *)dev->common_cfg)
                       [VIRTIO_PCI_COMMON_DEVICE_STATUS]);
}

static void virtio_pci_write_device_status(struct virtio_net_dev *dev,
                                           uint8_t status) {
    if (dev->is_legacy) {
        pci_write_config8(dev->pci->bus, dev->pci->device,
                          dev->pci->function, 0x14, status);
        return;
    }
    mmio_write8(&((volatile uint8_t *)dev->common_cfg)
                [VIRTIO_PCI_COMMON_DEVICE_STATUS], status);
}

static void virtio_pci_add_status(struct virtio_net_dev *dev, uint8_t bit) {
    uint8_t status = virtio_pci_read_device_status(dev);
    status |= bit;
    virtio_pci_write_device_status(dev, status);
}

static uint64_t virtio_pci_read_device_features(struct virtio_net_dev *dev) {
    uint64_t features = 0;
    if (dev->is_legacy) {
        features = pci_read_config32(dev->pci->bus, dev->pci->device,
                                     dev->pci->function, 0x00);
        return features;
    }
    mmio_write32(&dev->common_cfg[VIRTIO_PCI_COMMON_DFSELECT / 4], 0);
    features = mmio_read32(&dev->common_cfg[VIRTIO_PCI_COMMON_DF / 4]);
    mmio_write32(&dev->common_cfg[VIRTIO_PCI_COMMON_DFSELECT / 4], 1);
    features |= ((uint64_t)mmio_read32(&dev->common_cfg[VIRTIO_PCI_COMMON_DF / 4])) << 32;
    return features;
}

static void virtio_pci_write_driver_features(struct virtio_net_dev *dev,
                                             uint64_t features) {
    if (dev->is_legacy) {
        pci_write_config32(dev->pci->bus, dev->pci->device,
                           dev->pci->function, 0x04, (uint32_t)features);
        return;
    }
    mmio_write32(&dev->common_cfg[VIRTIO_PCI_COMMON_GUEST_FEATURE_SEL / 4], 0);
    mmio_write32(&dev->common_cfg[VIRTIO_PCI_COMMON_GUEST_FEATURE / 4],
                 (uint32_t)(features & 0xFFFFFFFF));
    mmio_write32(&dev->common_cfg[VIRTIO_PCI_COMMON_GUEST_FEATURE_SEL / 4], 1);
    mmio_write32(&dev->common_cfg[VIRTIO_PCI_COMMON_GUEST_FEATURE / 4],
                 (uint32_t)(features >> 32));
}

static void virtio_pci_setup_virtqueue(struct virtio_net_dev *dev,
                                       uint16_t queue_idx, struct virtq *vq) {
    if (dev->is_legacy) return;

    mmio_write16((volatile uint16_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_SELECT / 4],
                 queue_idx);

    uint16_t queue_size = mmio_read16((volatile uint16_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_SIZE / 4]);
    if (queue_size > VIRTIO_VQ_MAX_SIZE) queue_size = VIRTIO_VQ_MAX_SIZE;

    virtq_init(vq, queue_size);
    vq->queue_index = queue_idx;

    mmio_write64((volatile uint64_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_DESC / 4],
                 vq->desc_phys);
    mmio_write64((volatile uint64_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_AVAIL / 4],
                 vq->avail_phys);
    mmio_write64((volatile uint64_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_USED / 4],
                 vq->used_phys);

    mmio_write16((volatile uint16_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_ENABLE / 4], 1);
}

static void virtio_pci_notify_queue(struct virtio_net_dev *dev,
                                    uint16_t queue_idx) {
    if (dev->is_legacy) {
        outb((uint16_t)(dev->pci->bars[0] & PCI_BAR_IO_MASK) + 16, queue_idx);
        return;
    }
    uint32_t off = dev->notify_off_multiplier * queue_idx;
    mmio_write16((volatile uint16_t *)((uintptr_t)dev->notify_cfg + off), queue_idx);
}

/* ================================================================
 * VirtIO PCI Capability Parsing
 * ================================================================ */

static int virtio_net_parse_caps(struct virtio_net_dev *dev) {
    uint8_t cap_ptr = pci_find_capability(dev->pci, PCI_CAP_ID_VENDOR);
    if (!cap_ptr) {
        log_printf(LOG_LEVEL_DEBUG, "virtio-net: no vendor capability found\n");
        return -1;
    }

    while (cap_ptr) {
        uint8_t cap_vndr = pci_read_config8(dev->pci->bus, dev->pci->device,
                                            dev->pci->function, cap_ptr);
        uint8_t cap_next = pci_read_config8(dev->pci->bus, dev->pci->device,
                                            dev->pci->function, cap_ptr + 1);
        uint8_t cfg_type = pci_read_config8(dev->pci->bus, dev->pci->device,
                                            dev->pci->function, cap_ptr + 3);
        uint8_t bar_idx  = pci_read_config8(dev->pci->bus, dev->pci->device,
                                            dev->pci->function, cap_ptr + 4);
        uint32_t offset  = pci_read_config32(dev->pci->bus, dev->pci->device,
                                             dev->pci->function, cap_ptr + 8);

        if (cap_vndr != VIRTIO_PCI_CAP_VENDOR_CFG) {
            cap_ptr = cap_next;
            continue;
        }

        uint32_t bar_base = dev->pci->bars[bar_idx] & PCI_BAR_MEM_MASK;
        uintptr_t mmio_base = (uintptr_t)bar_base + offset;

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            dev->common_cfg = (volatile uint32_t *)mmio_base;
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            dev->notify_cfg = (volatile uint32_t *)mmio_base;
            dev->notify_off_multiplier = pci_read_config32(dev->pci->bus,
                dev->pci->device, dev->pci->function, cap_ptr + 16);
            break;
        case VIRTIO_PCI_CAP_ISR_CFG:
            dev->isr_cfg = (volatile uint32_t *)mmio_base;
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            dev->device_cfg = (volatile uint8_t *)mmio_base;
            break;
        default:
            break;
        }

        cap_ptr = cap_next;
    }

    if (!dev->common_cfg) {
        log_printf(LOG_LEVEL_DEBUG, "virtio-net: common_cfg not found\n");
        return -1;
    }

    return 0;
}

/* ================================================================
 * VirtIO Device Initialization Sequence
 * ================================================================ */

static int virtio_net_device_init(struct virtio_net_dev *dev) {
    /* Step 1: Reset */
    virtio_pci_write_device_status(dev, 0);
    (void)virtio_pci_read_device_status(dev);

    /* Step 2: Acknowledge */
    virtio_pci_add_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);

    /* Step 3: Driver loaded */
    virtio_pci_add_status(dev, VIRTIO_STATUS_DRIVER);

    /* Step 4: Negotiate features */
    uint64_t device_features = virtio_pci_read_device_features(dev);
    uint64_t driver_features = 0;

    if (!dev->is_legacy) {
        driver_features |= VIRTIO_F_VERSION_1;
    }

    if (device_features & VIRTIO_NET_F_MAC) {
        driver_features |= VIRTIO_NET_F_MAC;
    }

    driver_features &= device_features;
    virtio_pci_write_driver_features(dev, driver_features);

    /* Step 5: Features OK */
    virtio_pci_add_status(dev, VIRTIO_STATUS_FEATURES_OK);

    uint8_t status = virtio_pci_read_device_status(dev);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        log_printf(LOG_LEVEL_WARN,
                   "virtio-net: device rejected features (status=0x%02x)\n",
                   status);
        return -1;
    }

    /* Step 6: Setup virtqueues */
    virtio_pci_setup_virtqueue(dev, VIRTIO_NET_RX_QUEUE, &dev->rx_vq);
    dev->rx_ready = 1;

    virtio_pci_setup_virtqueue(dev, VIRTIO_NET_TX_QUEUE, &dev->tx_vq);
    dev->tx_ready = 1;

    /* Step 7: DRIVER_OK */
    virtio_pci_add_status(dev, VIRTIO_STATUS_DRIVER_OK);

    /* Read MAC address from device config */
    if (dev->device_cfg) {
        struct virtio_net_config *cfg = (struct virtio_net_config *)dev->device_cfg;
        memcpy(dev->netdev.mac, cfg->mac, 6);
        dev->netdev.mtu = cfg->mtu;
        if (dev->netdev.mtu == 0) dev->netdev.mtu = 1500;
    } else {
        dev->netdev.mtu = 1500;
    }

    /* Allocate receive buffers */
    dev->rx_buffer_count = 0;
    for (uint32_t i = 0; i < dev->rx_vq.num && i < VIRTIO_VQ_MAX_SIZE; i++) {
        void *buf = kmalloc(VIRTIO_NET_MAX_PACKET);
        if (!buf) break;
        dev->rx_buffers[i] = buf;

        /* Add descriptor with write-only flag (device writes to it) */
        int idx = virtq_add_descriptor(&dev->rx_vq,
                                       (uint64_t)(uintptr_t)buf,
                                       VIRTIO_NET_MAX_PACKET,
                                       VIRTQ_DESC_F_WRITE);
        if (idx < 0) {
            kfree(buf);
            break;
        }
        dev->rx_buffer_count++;
    }

    /* Submit all receive buffers */
    if (dev->rx_buffer_count > 0) {
        virtq_kick(&dev->rx_vq);
        virtio_pci_notify_queue(dev, VIRTIO_NET_RX_QUEUE);
    }

    log_printf(LOG_LEVEL_INFO,
               "virtio-net: mac=%02x:%02x:%02x:%02x:%02x:%02x mtu=%d\n",
               dev->netdev.mac[0], dev->netdev.mac[1],
               dev->netdev.mac[2], dev->netdev.mac[3],
               dev->netdev.mac[4], dev->netdev.mac[5],
               dev->netdev.mtu);

    return 0;
}

/* ================================================================
 * Network Operations
 * ================================================================ */

static int virtio_net_send(struct net_device *netdev, const void *data,
                           int len) {
    struct virtio_net_dev *dev = (struct virtio_net_dev *)netdev->priv;
    if (!dev || !dev->tx_ready) return -1;
    if (len <= 0 || len > VIRTIO_NET_MAX_PACKET) return -1;

    /* Allocate a copy of the packet for the descriptor */
    void *pkt_buf = kmalloc((size_t)len);
    if (!pkt_buf) return -1;
    memcpy(pkt_buf, data, (size_t)len);

    /* Build descriptor: device reads from this buffer */
    int idx = virtq_add_descriptor(&dev->tx_vq,
                                   (uint64_t)(uintptr_t)pkt_buf,
                                   (uint32_t)len, 0);
    if (idx < 0) {
        kfree(pkt_buf);
        return -1;
    }

    /* Submit */
    virtq_kick(&dev->tx_vq);
    virtio_pci_notify_queue(dev, VIRTIO_NET_TX_QUEUE);

    /* Wait for completion */
    int timeout = 100000;
    int result = -1;
    while (timeout > 0) {
        uint32_t used_len;
        int used_idx = virtq_get_buf(&dev->tx_vq, &used_len);
        if (used_idx >= 0) {
            result = 0;
            break;
        }
        timeout--;
        asm volatile ("pause" ::: "memory");
    }

    kfree(pkt_buf);
    return result;
}

static int virtio_net_recv(struct net_device *netdev, void *buf, int max_len) {
    struct virtio_net_dev *dev = (struct virtio_net_dev *)netdev->priv;
    if (!dev || !dev->rx_ready) return -1;

    /* Check if any buffer is available */
    uint32_t used_len;
    int used_idx = virtq_get_buf(&dev->rx_vq, &used_len);
    if (used_idx < 0) return 0;  /* No data available */

    /* Find the buffer */
    void *rx_buf = dev->rx_buffers[used_idx];
    if (!rx_buf) return -1;

    /* Copy data to caller */
    int copy_len = (int)used_len;
    if (copy_len > max_len) copy_len = max_len;
    if (copy_len > 0) {
        memcpy(buf, rx_buf, (size_t)copy_len);
    }

    /* Re-submit the buffer for future receives */
    int new_idx = virtq_add_descriptor(&dev->rx_vq,
                                       (uint64_t)(uintptr_t)rx_buf,
                                       VIRTIO_NET_MAX_PACKET,
                                       VIRTQ_DESC_F_WRITE);
    if (new_idx >= 0) {
        virtq_kick(&dev->rx_vq);
        virtio_pci_notify_queue(dev, VIRTIO_NET_RX_QUEUE);
    }

    return copy_len;
}

static int virtio_net_up(struct net_device *netdev) {
    struct virtio_net_dev *dev = (struct virtio_net_dev *)netdev->priv;
    if (!dev) return -1;
    dev->netdev.flags |= NETDEV_FLAG_UP;
    log_printf(LOG_LEVEL_INFO, "virtio-net: interface up\n");
    return 0;
}

static int virtio_net_down(struct net_device *netdev) {
    struct virtio_net_dev *dev = (struct virtio_net_dev *)netdev->priv;
    if (!dev) return -1;
    dev->netdev.flags &= ~NETDEV_FLAG_UP;
    log_printf(LOG_LEVEL_INFO, "virtio-net: interface down\n");
    return 0;
}

/* ================================================================
 * Device Probing
 * ================================================================ */

static int virtio_net_probe_device(struct pci_device *pci) {
    struct virtio_net_dev *dev =
        (struct virtio_net_dev *)kmalloc(sizeof(*dev));
    if (!dev) return -1;

    memset(dev, 0, sizeof(*dev));
    dev->pci = pci;
    dev->is_legacy = 0;

    uint16_t dev_id = pci->device_id;
    if (dev_id >= 0x1000 && dev_id <= 0x103F) {
        dev->is_legacy = 1;
        log_printf(LOG_LEVEL_INFO, "virtio-net: legacy device detected\n");
    }

    /* Enable bus mastering and memory space */
    uint16_t cmd = pci_read_config16(pci->bus, pci->device, pci->function,
                                     PCI_CONFIG_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE;
    pci_write_config16(pci->bus, pci->device, pci->function,
                       PCI_CONFIG_COMMAND, cmd);

    if (!dev->is_legacy) {
        if (virtio_net_parse_caps(dev) != 0) {
            log_printf(LOG_LEVEL_DEBUG,
                       "virtio-net: failed to parse PCI caps, trying legacy\n");
            dev->is_legacy = 1;
        }
    }

    if (virtio_net_device_init(dev) != 0) {
        log_printf(LOG_LEVEL_WARN, "virtio-net: device init failed\n");
        kfree(dev);
        return -1;
    }

    /* Setup network device */
    {
        const char *name = "eth0";
        size_t len = strlen(name);
        if (len < sizeof(dev->netdev.name) - 1) {
            memcpy(dev->netdev.name, name, len + 1);
        }
    }
    dev->netdev.send  = virtio_net_send;
    dev->netdev.recv  = virtio_net_recv;
    dev->netdev.up    = virtio_net_up;
    dev->netdev.down  = virtio_net_down;
    dev->netdev.priv  = dev;
    dev->netdev.next  = NULL;
    dev->netdev.flags = 0;

    netdev_register(&dev->netdev);

    dev->next = net_dev_list;
    net_dev_list = dev;

    log_printf(LOG_LEVEL_INFO, "virtio-net: device '%s' registered\n",
               dev->netdev.name);
    return 0;
}

/* ================================================================
 * Driver Entry Point
 * ================================================================ */

static void virtio_net_probe(void) {
    struct pci_device *pci = pci_get_device_list();

    while (pci) {
        if (pci->vendor_id == VIRTIO_PCI_VENDOR_ID) {
            uint16_t dev_id = pci->device_id;
            if (dev_id == VIRTIO_PCI_DEVICE_NET_MODERN ||
                dev_id == VIRTIO_PCI_DEVICE_NET_LEGACY) {
                virtio_net_probe_device(pci);
            }
        }
        pci = pci->next;
    }
}

void virtio_net_init(void) {
    log_printf(LOG_LEVEL_INFO, "virtio-net: probing for devices...\n");
    virtio_net_probe();
}