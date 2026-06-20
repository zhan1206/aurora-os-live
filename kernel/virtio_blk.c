/*
 * virtio_blk.c - VirtIO Block Device Driver (PCI transport)
 *
 * Implements a VirtIO 1.0 block device driver using the modern PCI transport.
 * Handles device initialization, virtqueue management, and block I/O.
 */
#include "virtio.h"
#include "pci.h"
#include "block_dev.h"
#include "include/portio.h"
#include "include/log.h"
#include "include/string.h"
#include "mem.h"
#include <stdint.h>

/* ================================================================
 * VirtIO PCI Vendor/Device IDs
 * ================================================================ */
#define VIRTIO_PCI_VENDOR_ID  0x1AF4

/* Transitional device range: 0x1000 - 0x103F */
#define VIRTIO_PCI_DEVICE_BLK_MODERN   0x1042
#define VIRTIO_PCI_DEVICE_BLK_LEGACY   0x1001

/* VirtIO PCI capability vendor-specific CFG type */
#define VIRTIO_PCI_CAP_VENDOR_CFG      0x09

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
 * VirtIO Block Device Private Data
 * ================================================================ */
struct virtio_blk_dev {
    struct pci_device *pci;
    struct block_device bdev;

    /* Transport type */
    int is_legacy;

    /* MMIO base addresses for modern transport */
    volatile uint32_t *common_cfg;
    volatile uint32_t *notify_cfg;
    volatile uint32_t *isr_cfg;
    volatile uint8_t  *device_cfg;

    /* Notify offset multiplier */
    uint32_t notify_off_multiplier;

    /* Virtqueue */
    struct virtq vq;
    int vq_ready;

    /* Device configuration */
    uint64_t capacity;
    uint32_t blk_size;
    int read_only;

    /* Discovery */
    struct virtio_blk_dev *next;
};

static struct virtio_blk_dev *blk_dev_list = NULL;

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
 * Virtqueue Management
 * ================================================================ */

void virtq_init(struct virtq *vq, uint32_t queue_size) {
    if (queue_size > VIRTIO_VQ_MAX_SIZE) queue_size = VIRTIO_VQ_MAX_SIZE;

    vq->num = queue_size;
    vq->free_head = 0;
    vq->last_used_idx = 0;
    vq->queue_index = 0;

    /* Allocate descriptor table */
    size_t desc_size = sizeof(struct virtq_desc) * queue_size;
    vq->desc = (struct virtq_desc *)kmalloc(desc_size);
    if (!vq->desc) return;
    memset(vq->desc, 0, desc_size);

    /* Allocate available ring */
    size_t avail_size = sizeof(struct virtq_avail);
    vq->avail = (struct virtq_avail *)kmalloc(avail_size);
    if (!vq->avail) { kfree(vq->desc); vq->desc = NULL; return; }
    memset(vq->avail, 0, avail_size);

    /* Allocate used ring */
    size_t used_size = sizeof(struct virtq_used);
    vq->used = (struct virtq_used *)kmalloc(used_size);
    if (!vq->used) { kfree(vq->desc); kfree(vq->avail); vq->desc = NULL; vq->avail = NULL; return; }
    memset(vq->used, 0, used_size);

    /* Initialize free descriptor chain */
    for (uint32_t i = 0; i < queue_size - 1; i++) {
        vq->desc[i].next = (uint16_t)(i + 1);
    }
    vq->desc[queue_size - 1].next = 0;
    vq->free_head = 0;

    /* Store "physical" addresses (identity-mapped) */
    vq->desc_phys  = (uint64_t)(uintptr_t)vq->desc;
    vq->avail_phys = (uint64_t)(uintptr_t)vq->avail;
    vq->used_phys  = (uint64_t)(uintptr_t)vq->used;
}

int virtq_add_descriptor(struct virtq *vq, uint64_t addr, uint32_t len,
                         uint16_t flags) {
    if (vq->free_head >= vq->num) return -1;

    uint32_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;

    vq->desc[idx].addr  = addr;
    vq->desc[idx].len   = len;
    vq->desc[idx].flags = flags;
    vq->desc[idx].next  = 0;

    return (int)idx;
}

int virtq_add_chain(struct virtq *vq, uint64_t *addrs, uint32_t *lens,
                    uint16_t *flags, uint32_t num) {
    if (num == 0) return -1;

    int first = -1;
    int prev = -1;

    for (uint32_t i = 0; i < num; i++) {
        int idx = virtq_add_descriptor(vq, addrs[i], lens[i], flags[i]);
        if (idx < 0) return -1;

        if (first < 0) first = idx;
        if (prev >= 0) {
            vq->desc[prev].flags |= VIRTQ_DESC_F_NEXT;
            vq->desc[prev].next = (uint16_t)idx;
        }
        prev = idx;
    }

    return first;
}

void virtq_kick(struct virtq *vq) {
    /* Ensure writes are visible */
    asm volatile ("" ::: "memory");

    /* Place the descriptor head in the available ring */
    vq->avail->ring[vq->avail->idx % vq->num] = (uint16_t)vq->avail->idx;
    vq->avail->idx++;
}

int virtq_get_buf(struct virtq *vq, uint32_t *len) {
    if (vq->last_used_idx == vq->used->idx) {
        return -1;  /* No buffer available */
    }

    uint16_t used_idx = (uint16_t)(vq->last_used_idx % vq->num);
    struct virtq_used_elem *elem = &vq->used->ring[used_idx];

    if (len) *len = elem->len;

    /* Return descriptor to free list */
    uint32_t desc_id = elem->id;
    vq->desc[desc_id].next = (uint16_t)vq->free_head;
    vq->free_head = desc_id;

    vq->last_used_idx++;

    return (int)desc_id;
}

/* ================================================================
 * VirtIO PCI Transport Helpers
 * ================================================================ */

static uint8_t virtio_pci_read_device_status(struct virtio_blk_dev *dev) {
    if (dev->is_legacy) {
        return pci_read_config8(dev->pci->bus, dev->pci->device,
                                dev->pci->function, 0x14);
    }
    return mmio_read8(&((volatile uint8_t *)dev->common_cfg)
                       [VIRTIO_PCI_COMMON_DEVICE_STATUS]);
}

static void virtio_pci_write_device_status(struct virtio_blk_dev *dev,
                                           uint8_t status) {
    if (dev->is_legacy) {
        pci_write_config8(dev->pci->bus, dev->pci->device,
                          dev->pci->function, 0x14, status);
        return;
    }
    mmio_write8(&((volatile uint8_t *)dev->common_cfg)
                [VIRTIO_PCI_COMMON_DEVICE_STATUS], status);
}

static void virtio_pci_add_status(struct virtio_blk_dev *dev, uint8_t bit) {
    uint8_t status = virtio_pci_read_device_status(dev);
    status |= bit;
    virtio_pci_write_device_status(dev, status);
}

static uint64_t virtio_pci_read_device_features(struct virtio_blk_dev *dev) {
    uint64_t features = 0;
    if (dev->is_legacy) {
        features = pci_read_config32(dev->pci->bus, dev->pci->device,
                                     dev->pci->function, 0x00);
        return features;
    }
    /* Select feature page 0 */
    mmio_write32(&dev->common_cfg[VIRTIO_PCI_COMMON_DFSELECT / 4], 0);
    features = mmio_read32(&dev->common_cfg[VIRTIO_PCI_COMMON_DF / 4]);
    /* Select feature page 1 */
    mmio_write32(&dev->common_cfg[VIRTIO_PCI_COMMON_DFSELECT / 4], 1);
    features |= ((uint64_t)mmio_read32(&dev->common_cfg[VIRTIO_PCI_COMMON_DF / 4])) << 32;
    return features;
}

static void virtio_pci_write_driver_features(struct virtio_blk_dev *dev,
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

static void virtio_pci_setup_virtqueue(struct virtio_blk_dev *dev,
                                       uint16_t queue_idx) {
    if (dev->is_legacy) {
        /* Legacy: queue info is in the BAR */
        return;
    }

    /* Select the queue */
    mmio_write16((volatile uint16_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_SELECT / 4],
                 queue_idx);

    uint16_t queue_size = mmio_read16((volatile uint16_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_SIZE / 4]);
    if (queue_size > VIRTIO_VQ_MAX_SIZE) queue_size = VIRTIO_VQ_MAX_SIZE;

    virtq_init(&dev->vq, queue_size);
    dev->vq.queue_index = queue_idx;

    /* Set descriptor table address */
    mmio_write64((volatile uint64_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_DESC / 4],
                 dev->vq.desc_phys);
    /* Set available ring address */
    mmio_write64((volatile uint64_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_AVAIL / 4],
                 dev->vq.avail_phys);
    /* Set used ring address */
    mmio_write64((volatile uint64_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_USED / 4],
                 dev->vq.used_phys);

    /* Enable the queue */
    mmio_write16((volatile uint16_t *)&dev->common_cfg[VIRTIO_PCI_COMMON_QUEUE_ENABLE / 4], 1);

    dev->vq_ready = 1;
}

static void virtio_pci_notify_queue(struct virtio_blk_dev *dev,
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

static int virtio_pci_parse_caps(struct virtio_blk_dev *dev) {
    uint8_t cap_ptr = pci_find_capability(dev->pci, PCI_CAP_ID_VENDOR);
    if (!cap_ptr) {
        log_printf(LOG_LEVEL_DEBUG, "virtio-blk: no vendor capability found\n");
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

        /* If cap_vndr != 0x09, this is not a VirtIO capability, skip */
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
            /* Read notify_off_multiplier from the cap */
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
        log_printf(LOG_LEVEL_DEBUG, "virtio-blk: common_cfg not found\n");
        return -1;
    }

    return 0;
}

/* ================================================================
 * VirtIO Device Initialization Sequence
 * ================================================================ */

static int virtio_device_init(struct virtio_blk_dev *dev) {
    /* Step 1: Reset the device */
    virtio_pci_write_device_status(dev, 0);
    /* Read back to ensure reset */
    (void)virtio_pci_read_device_status(dev);

    /* Step 2: Acknowledge the device */
    virtio_pci_add_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);

    /* Step 3: Driver loaded */
    virtio_pci_add_status(dev, VIRTIO_STATUS_DRIVER);

    /* Step 4: Negotiate features */
    uint64_t device_features = virtio_pci_read_device_features(dev);
    uint64_t driver_features = 0;

    /* We accept VIRTIO_F_VERSION_1 for modern transport */
    if (!dev->is_legacy) {
        driver_features |= VIRTIO_F_VERSION_1;
    }

    if (device_features & VIRTIO_BLK_F_RO) {
        dev->read_only = 1;
    }

    if (device_features & VIRTIO_BLK_F_BLK_SIZE) {
        driver_features |= VIRTIO_BLK_F_BLK_SIZE;
    }

    /* Negotiate: only features supported by both sides */
    driver_features &= device_features;
    virtio_pci_write_driver_features(dev, driver_features);

    /* Step 5: Features OK */
    virtio_pci_add_status(dev, VIRTIO_STATUS_FEATURES_OK);

    /* Verify FEATURES_OK was accepted */
    uint8_t status = virtio_pci_read_device_status(dev);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        log_printf(LOG_LEVEL_WARN,
                   "virtio-blk: device rejected features (status=0x%02x)\n",
                   status);
        return -1;
    }

    /* Step 6: Setup virtqueue */
    virtio_pci_setup_virtqueue(dev, 0);

    /* Step 7: DRIVER_OK */
    virtio_pci_add_status(dev, VIRTIO_STATUS_DRIVER_OK);

    /* Read device configuration */
    if (dev->device_cfg) {
        struct virtio_blk_config *cfg = (struct virtio_blk_config *)dev->device_cfg;
        dev->capacity = cfg->capacity;
        if (driver_features & VIRTIO_BLK_F_BLK_SIZE) {
            dev->blk_size = cfg->blk_size;
        } else {
            dev->blk_size = 512;
        }
    } else if (dev->is_legacy) {
        /* Legacy: read capacity from BAR */
        uint32_t cap_lo = pci_read_config32(dev->pci->bus, dev->pci->device,
                                            dev->pci->function, 0x14 + 0);
        uint32_t cap_hi = pci_read_config32(dev->pci->bus, dev->pci->device,
                                            dev->pci->function, 0x14 + 4);
        dev->capacity = ((uint64_t)cap_hi << 32) | cap_lo;
        dev->blk_size = 512;
    }

    log_printf(LOG_LEVEL_INFO,
               "virtio-blk: capacity=%llu sectors, blk_size=%u, ro=%d\n",
               (unsigned long long)dev->capacity, dev->blk_size, dev->read_only);

    return 0;
}

/* ================================================================
 * Block I/O Operations
 * ================================================================ */

static int virtio_blk_do_io(struct virtio_blk_dev *dev, uint32_t type,
                            uint64_t sector, void *data, uint32_t count) {
    if (!dev->vq_ready) return -1;
    if (dev->read_only && type == VIRTIO_BLK_T_OUT) return -1;

    uint32_t sectors_to_io = count;
    if (sectors_to_io == 0) return 0;

    /* Allocate request header */
    struct virtio_blk_req_header *req =
        (struct virtio_blk_req_header *)kmalloc(sizeof(*req));
    uint8_t *status_byte = (uint8_t *)kmalloc(1);

    if (!req || !status_byte) {
        if (req) kfree(req);
        if (status_byte) kfree(status_byte);
        return -1;
    }

    /* Build the request */
    req->type     = type;
    req->reserved = 0;
    req->sector   = sector;

    /* Build descriptor chain */
    uint64_t addrs[3];
    uint32_t lens[3];
    uint16_t flags_to_use[3];

    /* Descriptor 0: request header (device-read) */
    addrs[0] = (uint64_t)(uintptr_t)req;
    lens[0]  = sizeof(*req);
    flags_to_use[0] = 0;  /* device reads */

    /* Descriptor 1: data buffer (device-write for read, device-read for write) */
    addrs[1] = (uint64_t)(uintptr_t)data;
    lens[1]  = sectors_to_io * dev->blk_size;
    flags_to_use[1] = (type == VIRTIO_BLK_T_IN) ? VIRTQ_DESC_F_WRITE : 0;

    /* Descriptor 2: status byte (device-write) */
    addrs[2] = (uint64_t)(uintptr_t)status_byte;
    lens[2]  = 1;
    flags_to_use[2] = VIRTQ_DESC_F_WRITE;

    int head = virtq_add_chain(&dev->vq, addrs, lens, flags_to_use, 3);
    if (head < 0) {
        kfree(req);
        kfree(status_byte);
        return -1;
    }

    /* Submit to device */
    virtq_kick(&dev->vq);
    virtio_pci_notify_queue(dev, 0);

    /* Wait for completion (poll used ring) */
    int timeout = 1000000;
    while (virtq_get_buf(&dev->vq, NULL) < 0 && timeout > 0) {
        timeout--;
        asm volatile ("pause" ::: "memory");
    }

    if (timeout <= 0) {
        log_printf(LOG_LEVEL_WARN, "virtio-blk: I/O timeout\n");
        kfree(req);
        kfree(status_byte);
        return -1;
    }

    int result = (*status_byte == VIRTIO_BLK_S_OK) ? 0 : -1;

    kfree(req);
    kfree(status_byte);

    return result;
}

/* Global pointer for block device callback dispatch.
 * Since the block_device->read/write callbacks do not receive the
 * bdev pointer, we use a global to track the current active device.
 * Multi-device support can be added by extending this to a list. */
static struct virtio_blk_dev *g_active_blk = NULL;

static int virtio_blk_read(void *buf, uint64_t sector, int count) {
    if (!g_active_blk) return -1;
    return virtio_blk_do_io(g_active_blk, VIRTIO_BLK_T_IN, sector, buf,
                            (uint32_t)count);
}

static int virtio_blk_write(const void *buf, uint64_t sector, int count) {
    if (!g_active_blk) return -1;
    return virtio_blk_do_io(g_active_blk, VIRTIO_BLK_T_OUT, sector,
                            (void *)buf, (uint32_t)count);
}

int virtio_blk_get_capacity(struct virtio_blk_dev *dev) {
    if (!dev) return -1;
    return (int)dev->capacity;
}

/* ================================================================
 * Device Probing
 * ================================================================ */

static int virtio_blk_probe_device(struct pci_device *pci) {
    struct virtio_blk_dev *dev =
        (struct virtio_blk_dev *)kmalloc(sizeof(*dev));
    if (!dev) return -1;

    memset(dev, 0, sizeof(*dev));
    dev->pci = pci;
    dev->blk_size = 512;
    dev->is_legacy = 0;

    /* Check if legacy (transitional device) */
    uint16_t dev_id = pci->device_id;
    if (dev_id >= 0x1000 && dev_id <= 0x103F) {
        dev->is_legacy = 1;
        log_printf(LOG_LEVEL_INFO, "virtio-blk: legacy device detected\n");
    }

    /* Enable bus mastering and memory space */
    uint16_t cmd = pci_read_config16(pci->bus, pci->device, pci->function,
                                     PCI_CONFIG_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE;
    pci_write_config16(pci->bus, pci->device, pci->function,
                       PCI_CONFIG_COMMAND, cmd);

    if (!dev->is_legacy) {
        /* Parse PCI capabilities to find config areas */
        if (virtio_pci_parse_caps(dev) != 0) {
            log_printf(LOG_LEVEL_DEBUG,
                       "virtio-blk: failed to parse PCI caps, trying legacy\n");
            dev->is_legacy = 1;
        }
    }

    /* Initialize the device */
    if (virtio_device_init(dev) != 0) {
        log_printf(LOG_LEVEL_WARN, "virtio-blk: device init failed\n");
        kfree(dev);
        return -1;
    }

    /* Setup block device */
    dev->bdev.name[0] = '\0';
    {
        const char *name = "vda";
        size_t len = strlen(name);
        if (len < sizeof(dev->bdev.name) - 1) {
            memcpy(dev->bdev.name, name, len + 1);
        }
    }
    dev->bdev.block_size    = dev->blk_size;
    dev->bdev.total_sectors = dev->capacity;
    dev->bdev.read          = virtio_blk_read;
    dev->bdev.write         = virtio_blk_write;
    dev->bdev.ioctl         = NULL;
    dev->bdev.priv          = dev;

    /* Set driver as active for callback dispatch */
    g_active_blk = dev;
    dev->bdev.next          = NULL;

    /* Register block device */
    block_dev_register(&dev->bdev);

    /* Add to local list */
    dev->next = blk_dev_list;
    blk_dev_list = dev;

    log_printf(LOG_LEVEL_INFO, "virtio-blk: device '%s' registered\n",
               dev->bdev.name);
    return 0;
}

/* ================================================================
 * Driver Entry Point
 * ================================================================ */

void virtio_blk_probe(void) {
    struct pci_device *pci = pci_get_device_list();

    while (pci) {
        if (pci->vendor_id == VIRTIO_PCI_VENDOR_ID) {
            uint16_t dev_id = pci->device_id;
            /* Check for block device IDs */
            if (dev_id == VIRTIO_PCI_DEVICE_BLK_MODERN ||
                dev_id == VIRTIO_PCI_DEVICE_BLK_LEGACY ||
                (dev_id >= 0x1001 && dev_id <= 0x1001)) {
                /* Extended transitional check */
                if (dev_id >= 0x1000 && dev_id <= 0x103F) {
                    /* Transitional device - check subsystem for block */
                    if (dev_id == VIRTIO_PCI_DEVICE_BLK_LEGACY) {
                        virtio_blk_probe_device(pci);
                    }
                } else if (dev_id == VIRTIO_PCI_DEVICE_BLK_MODERN) {
                    virtio_blk_probe_device(pci);
                }
            }
        }
        pci = pci->next;
    }
}

void virtio_blk_init(void) {
    log_printf(LOG_LEVEL_INFO, "virtio-blk: probing for devices...\n");
    virtio_blk_probe();
}