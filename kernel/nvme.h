/*
 * nvme.h - NVMe Controller Driver Header
 *
 * Implements NVM Express 1.4-compatible register definitions,
 * command structures, and driver data structures.
 */
#ifndef NVME_H
#define NVME_H

#include <stdint.h>

/* Forward declarations for types used in this header */
struct pci_device;
struct block_device;

/* ================================================================
 * NVMe PCI Class Code
 * ================================================================ */
#define NVME_PCI_CLASS      0x01
#define NVME_PCI_SUBCLASS   0x08
#define NVME_PCI_PROG_IF    0x02

/* ================================================================
 * NVMe Controller Register Offsets (in BAR0 MMIO)
 * ================================================================ */
#define NVME_REG_CAP        0x0000  /* Controller Capabilities (64-bit) */
#define NVME_REG_VS         0x0008  /* Version (32-bit) */
#define NVME_REG_INTMS      0x000C  /* Interrupt Mask Set */
#define NVME_REG_INTMC      0x0010  /* Interrupt Mask Clear */
#define NVME_REG_CC         0x0014  /* Controller Configuration (32-bit) */
#define NVME_REG_RESERVED1  0x0018  /* Reserved */
#define NVME_REG_CSTS       0x001C  /* Controller Status (32-bit) */
#define NVME_REG_NSSR       0x0020  /* NVM Subsystem Reset */
#define NVME_REG_AQA        0x0024  /* Admin Queue Attributes (32-bit) */
#define NVME_REG_ASQ        0x0028  /* Admin Submission Queue Base Address (64-bit) */
#define NVME_REG_ACQ        0x0030  /* Admin Completion Queue Base Address (64-bit) */
#define NVME_REG_CMBLOC     0x0038  /* Controller Memory Buffer Location */
#define NVME_REG_CMBSZ      0x003C  /* Controller Memory Buffer Size */
#define NVME_REG_BPINFO     0x0040  /* Boot Partition Information */
#define NVME_REG_BPRSEL     0x0044  /* Boot Partition Read Select */
#define NVME_REG_BPMBL      0x0048  /* Boot Partition Memory Buffer Location */

/* SQ y Tail Doorbell: 0x1000 + (2 * y) * (4 << CAP.DSTRD) */
#define NVME_REG_SQ0TDBL    0x1000  /* Submission Queue 0 Tail Doorbell */

/* CQ y Head Doorbell: 0x1000 + (2 * y + 1) * (4 << CAP.DSTRD) */
#define NVME_REG_CQ0HDBL    0x1004  /* Completion Queue 0 Head Doorbell */

/* Convenience macros for doorbell stride */
#define NVME_SQ_TDBL(y, stride)  (0x1000 + ((2 * (y))     * (stride)))
#define NVME_CQ_HDBL(y, stride)  (0x1000 + ((2 * (y) + 1) * (stride)))

/* ================================================================
 * CAP (Controller Capabilities) Register Bitfields
 * ================================================================ */
#define NVME_CAP_MQES_MASK      0x0000FFFFULL  /* Maximum Queue Entries Supported */
#define NVME_CAP_CQR            0x00010000ULL  /* Contiguous Queues Required */
#define NVME_CAP_AMS_MASK       0x00060000ULL  /* Arbitration Mechanism Supported */
#define NVME_CAP_TO             0x00FF0000ULL  /* Timeout */
#define NVME_CAP_DSTRD_MASK     0x000F00000000ULL  /* Doorbell Stride */
#define NVME_CAP_NSSRS          0x001000000000ULL  /* NVM Subsystem Reset Supported */
#define NVME_CAP_CSS_MASK       0x00FF000000000000ULL  /* Command Sets Supported */

#define NVME_CAP_DSTRD_SHIFT    32
#define NVME_CAP_MPSMIN_SHIFT   48
#define NVME_CAP_MPSMAX_SHIFT   52

/* ================================================================
 * CC (Controller Configuration) Register Bitfields
 * ================================================================ */
#define NVME_CC_ENABLE          0x00000001U
#define NVME_CC_IOCQES_SHIFT    20
#define NVME_CC_IOSQES_SHIFT    16
#define NVME_CC_SHN_MASK        0x0000C000U
#define NVME_CC_SHN_NORMAL      0x00004000U
#define NVME_CC_SHN_ABRUPT      0x00008000U
#define NVME_CC_AMS_RR          0x00000000U

/* ================================================================
 * CSTS (Controller Status) Register Bitfields
 * ================================================================ */
#define NVME_CSTS_RDY           0x00000001U
#define NVME_CSTS_CFS           0x00000002U
#define NVME_CSTS_SHST_MASK     0x0000000CU
#define NVME_CSTS_NSSRO         0x00000010U

/* ================================================================
 * AQA (Admin Queue Attributes) Register
 * ================================================================ */
#define NVME_AQA_ASQS_SHIFT     0
#define NVME_AQA_ACQS_SHIFT     16

/* ================================================================
 * NVMe Queue Sizes
 * ================================================================ */
#define NVME_ADMIN_QUEUE_SIZE   64
#define NVME_IO_QUEUE_SIZE      256
#define NVME_QUEUE_ID            0  /* Admin SQ/CQ id */
#define NVME_IO_QUEUE_ID         1  /* First IO SQ/CQ id */

/* ================================================================
 * NVMe Submission Queue Entry (SQE) - 64 bytes
 * ================================================================ */
struct nvme_sqe {
    uint8_t  opcode;         /* DW0: Command opcode */
    uint8_t  flags;          /* DW0: Fused Operation (bits 0-1), PRP/SGL select (bit 6) */
    uint16_t cid;            /* DW0: Command Identifier */
    uint32_t nsid;           /* DW1: Namespace Identifier */
    uint64_t rsvd;           /* DW2-3: Reserved */
    uint64_t mptr;           /* DW4-5: Metadata Pointer */
    uint64_t prp1;           /* DW6-7: PRP Entry 1 (or Data Pointer) */
    uint64_t prp2;           /* DW8-9: PRP Entry 2 (or PRP List) */
    uint32_t cdw10;          /* DW10: Command-specific */
    uint32_t cdw11;          /* DW11: Command-specific */
    uint32_t cdw12;          /* DW12: Command-specific */
    uint32_t cdw13;          /* DW13: Command-specific */
    uint32_t cdw14;          /* DW14: Command-specific */
    uint32_t cdw15;          /* DW15: Command-specific */
} __attribute__((packed));

/* ================================================================
 * NVMe Completion Queue Entry (CQE) - 16 bytes
 * ================================================================ */
struct nvme_cqe {
    uint32_t cmd_specific;   /* DW0: Command-specific result */
    uint32_t rsvd;           /* DW1: Reserved */
    uint16_t sq_head;        /* DW2: SQ Head Pointer */
    uint16_t sq_id;          /* DW2: SQ Identifier */
    uint16_t cid;            /* DW3: Command Identifier */
    uint16_t status;         /* DW3: Status Field */
} __attribute__((packed));

/* Status field breakdown */
#define NVME_STATUS_SC_SHIFT     1
#define NVME_STATUS_SC_MASK      0x01FE
#define NVME_STATUS_SC_SUCCESS   0x0000
#define NVME_STATUS_DNR          0x4000
#define NVME_STATUS_MORE         0x8000

/* ================================================================
 * NVMe Admin Command Opcodes
 * ================================================================ */
#define NVME_ADMIN_DELETE_IOSQ   0x00
#define NVME_ADMIN_CREATE_IOSQ   0x01
#define NVME_ADMIN_GET_LOG_PAGE  0x02
#define NVME_ADMIN_DELETE_IOCQ   0x04
#define NVME_ADMIN_CREATE_IOCQ   0x05
#define NVME_ADMIN_IDENTIFY      0x06
#define NVME_ADMIN_ABORT         0x08
#define NVME_ADMIN_SET_FEATURES  0x09
#define NVME_ADMIN_GET_FEATURES  0x0A

/* ================================================================
 * NVMe NVM Command Opcodes
 * ================================================================ */
#define NVME_NVM_FLUSH           0x00
#define NVME_NVM_WRITE           0x01
#define NVME_NVM_READ            0x02

/* ================================================================
 * NVMe Identify CNS (Controller or Namespace Structure) Values
 * ================================================================ */
#define NVME_IDENTIFY_NS         0x00
#define NVME_IDENTIFY_CTRL       0x01
#define NVME_IDENTIFY_NS_LIST    0x02

/* ================================================================
 * NVMe Identify Controller Data Structure (partial, relevant fields)
 * ================================================================ */
struct nvme_identify_ctrl {
    uint16_t vid;
    uint16_t ssvid;
    char     sn[20];
    char     mn[40];
    char     fr[8];
    uint8_t  rab;
    uint8_t  ieee[3];
    uint8_t  cmic;
    uint8_t  mdts;
    uint16_t cntlid;
    uint32_t ver;
    uint32_t rtd3r;
    uint32_t rtd3e;
    uint32_t oaes;
    uint32_t ctratt;
    uint16_t rrls;
    uint8_t  rsvd1[9];
    uint8_t  cntrltype;
    uint8_t  fguid[16];
    uint16_t crdt1;
    uint16_t crdt2;
    uint16_t crdt3;
    uint8_t  rsvd2[122];
    uint8_t  rsvd3[4];
    uint32_t oacs;
    uint8_t  acl;
    uint8_t  aerl;
    uint8_t  frmw;
    uint8_t  lpa;
    uint8_t  elpe;
    uint8_t  npss;
    uint8_t  avscc;
    uint8_t  apsta;
    uint16_t wctemp;
    uint16_t cctemp;
    uint16_t mtfa;
    uint32_t hmpre;
    uint32_t hmmin;
    uint8_t  tnvmcap[16];
    uint8_t  unvmcap[16];
    uint32_t rpmbs;
    uint16_t edstt;
    uint8_t  dsto;
    uint8_t  fwug;
    uint16_t kas;
    uint16_t hctma;
    uint16_t mntmt;
    uint16_t mxtmt;
    uint32_t sanicap;
    uint32_t hmminds;
    uint16_t hmmaxd;
    uint16_t nsetidmax;
    uint32_t endgidmax;
    uint8_t  anatt;
    uint8_t  anacap;
    uint32_t anagrpmax;
    uint32_t nanagrpid;
    uint32_t pels;
    uint16_t domainid;
    uint8_t  rsvd4[10];
    uint8_t  megcap[16];
    uint8_t  rsvd5[128];
    uint8_t  rsvd6[128];
    uint8_t  rsvd7[1024];
    uint8_t  psd[32];
    uint8_t  vs[1024];
} __attribute__((packed));

/* ================================================================
 * NVMe Identify Namespace Data Structure (partial)
 * ================================================================ */
struct nvme_identify_ns {
    uint64_t nsze;       /* Namespace Size */
    uint64_t ncap;       /* Namespace Capacity */
    uint64_t nuse;       /* Namespace Utilization */
    uint8_t  nsfeat;
    uint8_t  nlbaf;      /* Number of LBA Formats */
    uint8_t  flbas;
    uint8_t  mc;
    uint8_t  dpc;
    uint8_t  dps;
    uint8_t  nmic;
    uint8_t  rescap;
    uint8_t  fpi;
    uint8_t  dlfeat;
    uint16_t nawun;
    uint16_t nawupf;
    uint16_t nacwu;
    uint16_t nabsn;
    uint16_t nabo;
    uint16_t nabspf;
    uint16_t noiob;
    uint8_t  nvmcap[16];
    uint16_t npwg;
    uint16_t npwa;
    uint16_t npdg;
    uint16_t npda;
    uint16_t nows;
    uint16_t mssrl;
    uint32_t mcl;
    uint8_t  msrc;
    uint8_t  rsvd1[11];
    uint32_t anagrpid;
    uint8_t  rsvd2[3];
    uint8_t  nsattr;
    uint16_t nvmsetid;
    uint16_t endgid;
    uint8_t  nguid[16];
    uint64_t eui64;
    struct {
        uint64_t ms;
        uint8_t  ds;
        uint16_t rp;
    } __attribute__((packed)) lbaf[16];
    uint8_t  rsvd3[192];
    uint8_t  vendor_specific[3712];
} __attribute__((packed));

/* ================================================================
 * NVMe PRP List
 * ================================================================ */
#define NVME_PRP_LIST_SIZE  512
#define NVME_PRP_ENTRIES    (NVME_PRP_LIST_SIZE / sizeof(uint64_t))

struct nvme_prp_list {
    uint64_t entries[NVME_PRP_ENTRIES];
} __attribute__((packed));

/* ================================================================
 * NVMe Namespace
 * ================================================================ */
struct nvme_namespace {
    uint32_t nsid;
    uint32_t block_size;
    uint64_t block_count;
    struct nvme_namespace *next;
};

/* ================================================================
 * NVMe Queue
 * ================================================================ */
struct nvme_queue {
    uint16_t  qid;
    uint16_t  num_entries;
    uint32_t  entry_size;
    struct nvme_sqe *sq;     /* Submission Queue */
    struct nvme_cqe *cq;     /* Completion Queue */
    uint64_t sq_phys;        /* Physical address of SQ */
    uint64_t cq_phys;        /* Physical address of CQ */
    uint16_t  sq_tail;       /* Submission Queue Tail */
    uint16_t  sq_head;       /* Submission Queue Head */
    uint16_t  cq_head;       /* Completion Queue Head */
    uint8_t   phase;         /* Phase tag for CQ entry validity */
};

/* ================================================================
 * NVMe Controller
 * ================================================================ */
struct nvme_controller {
    struct pci_device   *pci_device;
    volatile uint32_t   *bar_addr;       /* MMIO BAR0 */
    uint32_t            doorbell_stride; /* Doorbell stride (from CAP.DSTRD) */
    struct nvme_queue    admin_sq;       /* Admin Submission Queue */
    struct nvme_queue    admin_cq;       /* Admin Completion Queue */
    struct nvme_queue    io_sq;          /* I/O Submission Queue */
    struct nvme_queue    io_cq;          /* I/O Completion Queue */
    struct nvme_namespace *namespaces;   /* Linked list of namespaces */
    struct block_device  bdev;          /* Block device abstraction */
    int                  msix_enabled;
    struct nvme_controller *next;
};

/* ================================================================
 * NVMe Driver API
 * ================================================================ */
void nvme_init(void);

#endif /* NVME_H */