/*
 * virtio.h - VirtIO device definitions and driver interface
 *
 * Supports both MMIO and PCI transport.
 * VirtIO 1.0 spec compliant.
 */
#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * VirtIO Device IDs
 * ================================================================ */
#define VIRTIO_DEV_ID_NET         1
#define VIRTIO_DEV_ID_BLOCK       2
#define VIRTIO_DEV_ID_CONSOLE     3
#define VIRTIO_DEV_ID_ENTROPY     4
#define VIRTIO_DEV_ID_BALLOON     5
#define VIRTIO_DEV_ID_SCSI        8
#define VIRTIO_DEV_ID_GPU         16
#define VIRTIO_DEV_ID_INPUT       18
#define VIRTIO_DEV_ID_VSOCK       19
#define VIRTIO_DEV_ID_FS          26

/* ================================================================
 * VirtIO MMIO Register Offsets
 * ================================================================ */
#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00C
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0A0
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0A4
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0FC
#define VIRTIO_MMIO_CONFIG              0x100

#define VIRTIO_MMIO_MAGIC               0x74726976

/* ================================================================
 * VirtIO PCI Capability Types
 * ================================================================ */
#define VIRTIO_PCI_CAP_COMMON_CFG       1
#define VIRTIO_PCI_CAP_NOTIFY_CFG       2
#define VIRTIO_PCI_CAP_ISR_CFG          3
#define VIRTIO_PCI_CAP_DEVICE_CFG       4
#define VIRTIO_PCI_CAP_PCI_CFG          5
#define VIRTIO_PCI_CAP_SHARED_MEM_CFG   8

/* ================================================================
 * VirtIO PCI Modern Configuration Structure Offsets
 * ================================================================ */
#define VIRTIO_PCI_COMMON_DFSELECT        0x00
#define VIRTIO_PCI_COMMON_DF              0x04
#define VIRTIO_PCI_COMMON_GUEST_FEATURE   0x08
#define VIRTIO_PCI_COMMON_GUEST_FEATURE_SEL \
                                          0x0C
#define VIRTIO_PCI_COMMON_MSIX_CONFIG     0x10
#define VIRTIO_PCI_COMMON_NUM_QUEUES      0x12
#define VIRTIO_PCI_COMMON_DEVICE_STATUS   0x14
#define VIRTIO_PCI_COMMON_CONFIG_GEN      0x15
#define VIRTIO_PCI_COMMON_QUEUE_SELECT    0x16
#define VIRTIO_PCI_COMMON_QUEUE_SIZE      0x18
#define VIRTIO_PCI_COMMON_QUEUE_MSIX_VEC  0x1A
#define VIRTIO_PCI_COMMON_QUEUE_ENABLE    0x1C
#define VIRTIO_PCI_COMMON_QUEUE_NOTIFY_OFF \
                                          0x1E
#define VIRTIO_PCI_COMMON_QUEUE_DESC      0x20
#define VIRTIO_PCI_COMMON_QUEUE_AVAIL     0x28
#define VIRTIO_PCI_COMMON_QUEUE_USED      0x30

/* ================================================================
 * VirtIO PCI ISR Status
 * ================================================================ */
#define VIRTIO_PCI_ISR_QUEUE_INTR   0x01
#define VIRTIO_PCI_ISR_DEV_CFG      0x02

/* ================================================================
 * VirtIO Status Flags
 * ================================================================ */
#define VIRTIO_STATUS_ACKNOWLEDGE       0x01
#define VIRTIO_STATUS_DRIVER            0x02
#define VIRTIO_STATUS_DRIVER_OK         0x04
#define VIRTIO_STATUS_FEATURES_OK       0x08
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET \
                                        0x40
#define VIRTIO_STATUS_FAILED            0x80

/* ================================================================
 * VirtIO Feature Bits
 * ================================================================ */
#define VIRTIO_F_RING_INDIRECT_DESC     (1ULL << 28)
#define VIRTIO_F_RING_EVENT_IDX         (1ULL << 29)
#define VIRTIO_F_VERSION_1              (1ULL << 32)

/* Block-specific features */
#define VIRTIO_BLK_F_SIZE_MAX           (1ULL << 1)
#define VIRTIO_BLK_F_SEG_MAX            (1ULL << 2)
#define VIRTIO_BLK_F_RO                 (1ULL << 5)
#define VIRTIO_BLK_F_BLK_SIZE           (1ULL << 6)
#define VIRTIO_BLK_F_FLUSH              (1ULL << 9)
#define VIRTIO_BLK_F_TOPOLOGY           (1ULL << 10)
#define VIRTIO_BLK_F_CONFIG_WCE         (1ULL << 11)
#define VIRTIO_BLK_F_DISCARD            (1ULL << 13)

/* Net-specific features */
#define VIRTIO_NET_F_CSUM               (1ULL << 0)
#define VIRTIO_NET_F_GUEST_CSUM         (1ULL << 1)
#define VIRTIO_NET_F_MAC                (1ULL << 5)
#define VIRTIO_NET_F_GUEST_TSO4         (1ULL << 7)
#define VIRTIO_NET_F_GUEST_TSO6         (1ULL << 8)
#define VIRTIO_NET_F_GUEST_ECN          (1ULL << 9)
#define VIRTIO_NET_F_GUEST_UFO          (1ULL << 10)
#define VIRTIO_NET_F_HOST_TSO4          (1ULL << 11)
#define VIRTIO_NET_F_HOST_TSO6          (1ULL << 12)
#define VIRTIO_NET_F_HOST_ECN           (1ULL << 13)
#define VIRTIO_NET_F_HOST_UFO           (1ULL << 14)
#define VIRTIO_NET_F_MRG_RXBUF          (1ULL << 15)
#define VIRTIO_NET_F_STATUS             (1ULL << 16)
#define VIRTIO_NET_F_CTRL_VQ            (1ULL << 17)
#define VIRTIO_NET_F_CTRL_RX            (1ULL << 18)
#define VIRTIO_NET_F_CTRL_VLAN          (1ULL << 19)
#define VIRTIO_NET_F_GUEST_ANNOUNCE     (1ULL << 21)
#define VIRTIO_NET_F_MQ                 (1ULL << 22)
#define VIRTIO_NET_F_CTRL_MAC_ADDR      (1ULL << 23)

/* ================================================================
 * Virtqueue Structure
 * ================================================================ */
#define VIRTIO_VQ_MAX_SIZE        256

/* Descriptor flags */
#define VIRTQ_DESC_F_NEXT         0x01
#define VIRTQ_DESC_F_WRITE        0x02
#define VIRTQ_DESC_F_INDIRECT     0x04

/* Used ring flags */
#define VIRTQ_USED_F_NO_NOTIFY    0x01

/* Available ring flags */
#define VIRTQ_AVAIL_F_NO_INTERRUPT 0x01

/* Virtqueue descriptor */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

/* Virtqueue available ring */
struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_VQ_MAX_SIZE];
    uint16_t used_event;  /* only if VIRTIO_F_EVENT_IDX */
};

/* Virtqueue used ring element */
struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

/* Virtqueue used ring */
struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTIO_VQ_MAX_SIZE];
    uint16_t avail_event;  /* only if VIRTIO_F_EVENT_IDX */
};

/* Virtqueue */
struct virtq {
    uint32_t num;
    uint32_t free_head;
    uint16_t last_used_idx;
    uint16_t queue_index;

    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;

    /* Physical addresses for MMIO setup */
    uint64_t desc_phys;
    uint64_t avail_phys;
    uint64_t used_phys;
};

/* Callback type for virtqueue notifications */
typedef void (*virtq_callback_t)(struct virtq *vq);

/* ================================================================
 * VirtIO Block Request
 * ================================================================ */
#define VIRTIO_BLK_T_IN           0
#define VIRTIO_BLK_T_OUT          1
#define VIRTIO_BLK_T_FLUSH        4
#define VIRTIO_BLK_T_DISCARD      11
#define VIRTIO_BLK_T_WRITE_ZEROES 13

#define VIRTIO_BLK_S_OK           0
#define VIRTIO_BLK_S_IOERR        1
#define VIRTIO_BLK_S_UNSUPP       2

struct virtio_blk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

struct virtio_blk_req {
    struct virtio_blk_req_header header;
    uint8_t                      status;
};

/* VirtIO block device configuration space */
struct virtio_blk_config {
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;
    uint16_t cylinders;
    uint8_t  heads;
    uint8_t  sectors;
    uint32_t blk_size;
    uint8_t  physical_block_exp;
    uint8_t  alignment_offset;
    uint16_t min_io_size;
    uint32_t opt_io_size;
    uint8_t  writeback;
    uint8_t  unused0[3];
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t  write_zeroes_may_unmap;
    uint8_t  unused1[3];
};

/* ================================================================
 * VirtIO Net Configuration
 * ================================================================ */
struct virtio_net_config {
    uint8_t  mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
};

/* VirtIO net header */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};

/* ================================================================
 * VirtIO Utility Functions
 * ================================================================ */
void virtq_init(struct virtq *vq, uint32_t queue_size);
int  virtq_add_descriptor(struct virtq *vq, uint64_t addr, uint32_t len,
                          uint16_t flags);
int  virtq_add_chain(struct virtq *vq, uint64_t *addrs, uint32_t *lens,
                     uint16_t *flags, uint32_t num);
void virtq_kick(struct virtq *vq);
int  virtq_get_buf(struct virtq *vq, uint32_t *len);

#endif /* VIRTIO_H */