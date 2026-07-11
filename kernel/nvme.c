/*
 * nvme.c - NVMe Controller Driver Implementation
 *
 * Implements PCI enumeration, controller initialization, namespace
 * identification, I/O queue creation, and block read/write operations
 * using PRP lists. Integrates with the block device abstraction layer.
 */
#include "nvme.h"
#include "pci.h"
#include "block_dev.h"
#include "include/log.h"
#include "include/string.h"
#include "mem.h"
#include "smp.h"
#include <stdint.h>

/* ================================================================
 * NVMe Device List
 * ================================================================ */
static struct nvme_controller *nvme_ctrl_list = NULL;

/* ================================================================
 * MMIO Access Helpers
 * ================================================================ */

static inline uint32_t nvme_read32(volatile uint32_t *addr) {
    return *addr;
}

static inline uint64_t nvme_read64(volatile uint64_t *addr) {
    return *addr;
}

static inline void nvme_write32(volatile uint32_t *addr, uint32_t val) {
    *addr = val;
}

static inline void nvme_write64(volatile uint64_t *addr, uint64_t val) {
    *addr = val;
}

/* ================================================================
 * NVMe Queue Helpers
 * ================================================================ */

static void nvme_queue_init(struct nvme_queue *q, uint16_t qid,
                             uint16_t num_entries) {
    memset(q, 0, sizeof(*q));
    q->qid = qid;
    q->num_entries = num_entries;
    q->entry_size = sizeof(struct nvme_sqe);
    q->phase = 1;

    /* Allocate Submission Queue */
    size_t sq_size = sizeof(struct nvme_sqe) * num_entries;
    q->sq = (struct nvme_sqe *)kmalloc(sq_size);
    if (q->sq) {
        memset(q->sq, 0, sq_size);
        q->sq_phys = (uint64_t)(uintptr_t)q->sq;
    }

    /* Allocate Completion Queue */
    size_t cq_size = sizeof(struct nvme_cqe) * num_entries;
    q->cq = (struct nvme_cqe *)kmalloc(cq_size);
    if (q->cq) {
        memset(q->cq, 0, cq_size);
        q->cq_phys = (uint64_t)(uintptr_t)q->cq;
    }

    q->sq_tail = 0;
    q->sq_head = 0;
    q->cq_head = 0;
}

static int nvme_submit_cmd(struct nvme_controller *ctrl,
                            struct nvme_queue *sq, struct nvme_queue *cq,
                            struct nvme_sqe *cmd) {
    uint32_t stride = ctrl->doorbell_stride;

    /* Copy command to SQ at tail */
    uint16_t tail = sq->sq_tail;
    memcpy(&sq->sq[tail], cmd, sizeof(struct nvme_sqe));

    /* Advance tail */
    sq->sq_tail = (tail + 1) % sq->num_entries;

    /* Ring the doorbell */
    volatile uint32_t *doorbell =
        (volatile uint32_t *)((uintptr_t)ctrl->bar_addr +
                              NVME_SQ_TDBL(sq->qid, stride));
    nvme_write32(doorbell, sq->sq_tail);

    return 0;
}

static int nvme_wait_completion(struct nvme_controller *ctrl,
                                 struct nvme_queue *cq,
                                 struct nvme_cqe *result) {
    int timeout = 1000000;

    while (timeout > 0) {
        uint16_t head = cq->cq_head;
        struct nvme_cqe *entry = &cq->cq[head];

        /* Check phase tag to determine if entry is new */
        uint8_t phase = (uint8_t)(entry->status & 0x0001);
        if (phase == cq->phase) {
            /* Copy completion */
            if (result) {
                memcpy(result, entry, sizeof(struct nvme_cqe));
            }

            /* Advance head */
            cq->cq_head = (head + 1) % cq->num_entries;
            if (cq->cq_head == 0) {
                cq->phase ^= 1;  /* Toggle phase on wrap */
            }

            /* Ring completion queue head doorbell */
            uint32_t stride = ctrl->doorbell_stride;
            volatile uint32_t *doorbell =
                (volatile uint32_t *)((uintptr_t)ctrl->bar_addr +
                                      NVME_CQ_HDBL(cq->qid, stride));
            nvme_write32(doorbell, cq->cq_head);

            return 0;
        }

        timeout--;
        asm volatile ("pause" ::: "memory");
    }

    log_printf(LOG_LEVEL_WARN, "nvme: command timeout on queue %d\n", cq->qid);
    return -1;
}

static int nvme_admin_cmd(struct nvme_controller *ctrl,
                           struct nvme_sqe *cmd,
                           struct nvme_cqe *result) {
    int ret = nvme_submit_cmd(ctrl, &ctrl->admin_sq, &ctrl->admin_cq, cmd);
    if (ret < 0) return ret;

    return nvme_wait_completion(ctrl, &ctrl->admin_cq, result);
}

/* ================================================================
 * Controller Initialization
 * ================================================================ */

static int nvme_controller_init(struct nvme_controller *ctrl) {
    if (!ctrl || !ctrl->pci_device || !ctrl->bar_addr) return -1;

    volatile uint32_t *bar = ctrl->bar_addr;

    /* Step 1: Disable controller by clearing CC.EN */
    uint32_t cc = nvme_read32(&bar[NVME_REG_CC / 4]);
    cc &= ~NVME_CC_ENABLE;
    nvme_write32(&bar[NVME_REG_CC / 4], cc);

    /* Wait for CSTS.RDY to clear */
    int timeout = 500000;
    while (timeout > 0) {
        uint32_t csts = nvme_read32(&bar[NVME_REG_CSTS / 4]);
        if (!(csts & NVME_CSTS_RDY)) break;
        timeout--;
        asm volatile ("pause" ::: "memory");
    }
    if (timeout <= 0) {
        log_printf(LOG_LEVEL_WARN, "nvme: controller disable timeout\n");
        return -1;
    }

    /* Wait for CSTS.CFS (Controller Fatal Status) to be clear */
    timeout = 500000;
    while (timeout > 0) {
        uint32_t csts = nvme_read32(&bar[NVME_REG_CSTS / 4]);
        if (!(csts & NVME_CSTS_CFS)) break;
        timeout--;
        asm volatile ("pause" ::: "memory");
    }

    /* Step 2: Read controller capabilities */
    uint64_t cap = nvme_read64((volatile uint64_t *)&bar[NVME_REG_CAP / 4]);
    uint16_t mqes = (uint16_t)(cap & NVME_CAP_MQES_MASK);
    uint32_t dstrd = (uint32_t)((cap & NVME_CAP_DSTRD_MASK) >> NVME_CAP_DSTRD_SHIFT);
    ctrl->doorbell_stride = (4U << dstrd);

    log_printf(LOG_LEVEL_INFO, "nvme: MQES=%u, doorbell_stride=%u\n",
               (unsigned)mqes, (unsigned)ctrl->doorbell_stride);

    /* Step 3: Configure Admin Queues */
    /* Set Admin Queue Attributes (AQA) */
    uint16_t admin_size = (NVME_ADMIN_QUEUE_SIZE > mqes) ? mqes : NVME_ADMIN_QUEUE_SIZE;
    uint32_t aqa = ((uint32_t)(admin_size - 1) << NVME_AQA_ASQS_SHIFT) |
                   ((uint32_t)(admin_size - 1) << NVME_AQA_ACQS_SHIFT);
    nvme_write32(&bar[NVME_REG_AQA / 4], aqa);

    /* Initialize admin queues */
    nvme_queue_init(&ctrl->admin_sq, NVME_QUEUE_ID, admin_size);
    nvme_queue_init(&ctrl->admin_cq, NVME_QUEUE_ID, admin_size);

    if (!ctrl->admin_sq.sq || !ctrl->admin_cq.cq) {
        log_printf(LOG_LEVEL_WARN, "nvme: failed to allocate admin queues\n");
        return -1;
    }

    /* Set Admin SQ Base Address */
    nvme_write64((volatile uint64_t *)&bar[NVME_REG_ASQ / 4], ctrl->admin_sq.sq_phys);

    /* Set Admin CQ Base Address */
    nvme_write64((volatile uint64_t *)&bar[NVME_REG_ACQ / 4], ctrl->admin_cq.cq_phys);

    /* Step 4: Configure Controller */
    uint32_t cc_new = NVME_CC_ENABLE;
    /* I/O Completion Queue Entry Size = 4 (16 bytes = 2^4) */
    cc_new |= (4U << NVME_CC_IOCQES_SHIFT);
    /* I/O Submission Queue Entry Size = 6 (64 bytes = 2^6) */
    cc_new |= (6U << NVME_CC_IOSQES_SHIFT);
    /* Round-Robin arbitration */
    cc_new |= NVME_CC_AMS_RR;

    nvme_write32(&bar[NVME_REG_CC / 4], cc_new);

    /* Step 5: Wait for CSTS.RDY to become 1 */
    timeout = 500000;
    while (timeout > 0) {
        uint32_t csts = nvme_read32(&bar[NVME_REG_CSTS / 4]);
        if (csts & NVME_CSTS_RDY) break;
        timeout--;
        asm volatile ("pause" ::: "memory");
    }
    if (timeout <= 0) {
        log_printf(LOG_LEVEL_WARN, "nvme: controller enable timeout\n");
        return -1;
    }

    log_printf(LOG_LEVEL_INFO, "nvme: controller enabled (bus=%02x, dev=%02x, func=%d)\n",
               ctrl->pci_device->bus, ctrl->pci_device->device,
               ctrl->pci_device->function);

    return 0;
}

/* ================================================================
 * Identify Controller and Namespaces
 * ================================================================ */

static int nvme_identify(struct nvme_controller *ctrl) {
    if (!ctrl) return -1;

    /* Identify Controller */
    struct nvme_identify_ctrl *id_ctrl =
        (struct nvme_identify_ctrl *)kmalloc(sizeof(*id_ctrl));
    if (!id_ctrl) return -1;
    memset(id_ctrl, 0, sizeof(*id_ctrl));

    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 0;
    cmd.prp1 = (uint64_t)(uintptr_t)id_ctrl;
    cmd.cdw10 = NVME_IDENTIFY_CTRL;

    struct nvme_cqe result;
    int ret = nvme_admin_cmd(ctrl, &cmd, &result);
    if (ret < 0 || (result.status & NVME_STATUS_SC_MASK) != NVME_STATUS_SC_SUCCESS) {
        log_printf(LOG_LEVEL_WARN, "nvme: identify controller failed (status=0x%04x)\n",
                   result.status);
        kfree(id_ctrl);
        return -1;
    }

    log_printf(LOG_LEVEL_INFO, "nvme: controller VID=0x%04x, model=%.40s, fw=%.8s\n",
               id_ctrl->vid, id_ctrl->mn, id_ctrl->fr);

    kfree(id_ctrl);

    /* Identify Namespace 1 */
    struct nvme_identify_ns *id_ns =
        (struct nvme_identify_ns *)kmalloc(sizeof(*id_ns));
    if (!id_ns) return -1;
    memset(id_ns, 0, sizeof(*id_ns));

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 1;
    cmd.prp1 = (uint64_t)(uintptr_t)id_ns;
    cmd.cdw10 = NVME_IDENTIFY_NS;

    ret = nvme_admin_cmd(ctrl, &cmd, &result);
    if (ret < 0 || (result.status & NVME_STATUS_SC_MASK) != NVME_STATUS_SC_SUCCESS) {
        log_printf(LOG_LEVEL_WARN, "nvme: identify namespace 1 failed (status=0x%04x)\n",
                   result.status);
        kfree(id_ns);
        return -1;
    }

    /* Create namespace entry */
    struct nvme_namespace *ns = (struct nvme_namespace *)kmalloc(sizeof(*ns));
    if (ns) {
        memset(ns, 0, sizeof(*ns));
        ns->nsid = 1;
        ns->block_count = id_ns->nsze;

        /* Determine block size from active LBA format */
        uint8_t flbas = id_ns->flbas & 0x0F;
        if (flbas <= id_ns->nlbaf) {
            ns->block_size = (uint32_t)(1U << id_ns->lbaf[flbas].ds);
        } else {
            ns->block_size = 512;  /* Default */
        }

        ns->next = ctrl->namespaces;
        ctrl->namespaces = ns;

        log_printf(LOG_LEVEL_INFO,
                   "nvme: namespace %u: %llu blocks, block_size=%u\n",
                   (unsigned)ns->nsid,
                   (unsigned long long)ns->block_count,
                   (unsigned)ns->block_size);
    }

    kfree(id_ns);
    return 0;
}

/* ================================================================
 * Create I/O Queues
 * ================================================================ */

static int nvme_create_io_queues(struct nvme_controller *ctrl) {
    if (!ctrl) return -1;

    uint16_t io_qsize = NVME_IO_QUEUE_SIZE;
    uint16_t io_qid = NVME_IO_QUEUE_ID;

    /* Initialize I/O Completion Queue */
    nvme_queue_init(&ctrl->io_cq, io_qid, io_qsize);
    if (!ctrl->io_cq.cq) {
        log_printf(LOG_LEVEL_WARN, "nvme: failed to allocate I/O CQ\n");
        return -1;
    }

    /* Create I/O Completion Queue (Admin command) */
    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IOCQ;
    cmd.prp1 = ctrl->io_cq.cq_phys;
    cmd.cdw10 = ((uint32_t)(io_qsize - 1) << 16) | (uint32_t)io_qid;
    /* Interrupt Vector (0 = no interrupt, or use pin-based) */
    cmd.cdw11 = (uint32_t)(ctrl->msix_enabled ? 1 : 0);

    struct nvme_cqe result;
    int ret = nvme_admin_cmd(ctrl, &cmd, &result);
    if (ret < 0 || (result.status & NVME_STATUS_SC_MASK) != NVME_STATUS_SC_SUCCESS) {
        log_printf(LOG_LEVEL_WARN, "nvme: create I/O CQ failed (status=0x%04x)\n",
                   result.status);
        return -1;
    }

    /* Initialize I/O Submission Queue */
    nvme_queue_init(&ctrl->io_sq, io_qid, io_qsize);
    if (!ctrl->io_sq.sq) {
        log_printf(LOG_LEVEL_WARN, "nvme: failed to allocate I/O SQ\n");
        return -1;
    }

    /* Create I/O Submission Queue (Admin command) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IOSQ;
    cmd.prp1 = ctrl->io_sq.sq_phys;
    cmd.cdw10 = ((uint32_t)(io_qsize - 1) << 16) | (uint32_t)io_qid;
    /* SQID of associated CQ in bits 16-31 */
    cmd.cdw11 = ((uint32_t)io_qid << 16) | 1;  /* CQID=io_qid, QPRIO=urgent(1) */

    ret = nvme_admin_cmd(ctrl, &cmd, &result);
    if (ret < 0 || (result.status & NVME_STATUS_SC_MASK) != NVME_STATUS_SC_SUCCESS) {
        log_printf(LOG_LEVEL_WARN, "nvme: create I/O SQ failed (status=0x%04x)\n",
                   result.status);
        return -1;
    }

    log_printf(LOG_LEVEL_INFO, "nvme: I/O queues created (qid=%u, size=%u)\n",
               (unsigned)io_qid, (unsigned)io_qsize);

    return 0;
}

/* ================================================================
 * I/O Command Submission (using PRP lists)
 * ================================================================ */

static int nvme_io_submit(struct nvme_controller *ctrl,
                           uint8_t opcode, uint32_t nsid,
                           uint64_t lba, uint32_t num_blocks,
                           void *buf) {
    if (!ctrl || !buf || num_blocks == 0) return -1;

    struct nvme_namespace *ns = ctrl->namespaces;
    if (!ns) return -1;

    uint32_t block_size = ns->block_size;
    uint32_t total_bytes = num_blocks * block_size;
    uint64_t buf_phys = (uint64_t)(uintptr_t)buf;

    /* Build PRP list if needed */
    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = opcode;
    cmd.nsid = nsid;
    cmd.prp1 = buf_phys;
    cmd.prp2 = 0;
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (uint16_t)(num_blocks - 1);  /* 0-based */

    /* If data spans more than one page, set up PRP2 */
    uint64_t offset_in_page = buf_phys & 0xFFFULL;
    uint64_t first_page_data = 0x1000ULL - offset_in_page;

    if (total_bytes > first_page_data) {
        uint64_t remaining = total_bytes - first_page_data;
        uint64_t remaining_pages = (remaining + 0xFFFULL) / 0x1000ULL;

        if (remaining_pages == 1) {
            /* Only one more page: PRP2 points directly to it */
            cmd.prp2 = (buf_phys + 0x1000ULL) & ~0xFFFULL;
        } else {
            /* Need a PRP list */
            struct nvme_prp_list *prp_list =
                (struct nvme_prp_list *)kmalloc(sizeof(*prp_list));
            if (!prp_list) return -1;
            memset(prp_list, 0, sizeof(*prp_list));

            uint64_t data_addr = (buf_phys + 0x1000ULL) & ~0xFFFULL;
            /* Skip the first page (already in PRP1) */
            data_addr += 0x1000ULL;

            for (uint64_t i = 0; i < remaining_pages && i < NVME_PRP_ENTRIES; i++) {
                prp_list->entries[i] = data_addr;
                data_addr += 0x1000ULL;
            }

            cmd.prp2 = (uint64_t)(uintptr_t)prp_list;
        }
    }

    /* Submit to I/O Submission Queue */
    struct nvme_queue *sq = &ctrl->io_sq;
    uint16_t tail = sq->sq_tail;
    memcpy(&sq->sq[tail], &cmd, sizeof(struct nvme_sqe));
    sq->sq_tail = (tail + 1) % sq->num_entries;

    /* Ring doorbell */
    uint32_t stride = ctrl->doorbell_stride;
    volatile uint32_t *doorbell =
        (volatile uint32_t *)((uintptr_t)ctrl->bar_addr +
                              NVME_SQ_TDBL(sq->qid, stride));
    nvme_write32(doorbell, sq->sq_tail);

    /* Wait for completion */
    struct nvme_cqe cqe;
    int ret = nvme_wait_completion(ctrl, &ctrl->io_cq, &cqe);

    if (ret < 0) {
        return -1;
    }

    if ((cqe.status & NVME_STATUS_SC_MASK) != NVME_STATUS_SC_SUCCESS) {
        log_printf(LOG_LEVEL_WARN, "nvme: I/O error opcode=%u status=0x%04x\n",
                   opcode, cqe.status);
        return -1;
    }

    return 0;
}

/* ================================================================
 * Block Device Operations
 * ================================================================ */

static struct nvme_controller *g_active_nvme = NULL;

static int nvme_read(void *buf, uint64_t sector, int count) {
    if (!g_active_nvme || !g_active_nvme->namespaces) return -1;

    uint32_t nsid = g_active_nvme->namespaces->nsid;
    return nvme_io_submit(g_active_nvme, NVME_NVM_READ, nsid,
                          sector, (uint32_t)count, buf);
}

static int nvme_write(const void *buf, uint64_t sector, int count) {
    if (!g_active_nvme || !g_active_nvme->namespaces) return -1;

    uint32_t nsid = g_active_nvme->namespaces->nsid;
    return nvme_io_submit(g_active_nvme, NVME_NVM_WRITE, nsid,
                          sector, (uint32_t)count, (void *)buf);
}

/* ================================================================
 * Device Probing and Registration
 * ================================================================ */

static int nvme_probe_device(struct pci_device *pci) {
    struct nvme_controller *ctrl =
        (struct nvme_controller *)kmalloc(sizeof(*ctrl));
    if (!ctrl) return -1;

    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->pci_device = pci;

    /* Enable bus mastering and memory space access */
    uint16_t cmd_reg = pci_read_config16(pci->bus, pci->device, pci->function,
                                         PCI_CONFIG_COMMAND);
    cmd_reg |= PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE;
    pci_write_config16(pci->bus, pci->device, pci->function,
                       PCI_CONFIG_COMMAND, cmd_reg);

    /* Get BAR0 (MMIO base) */
    uint32_t bar0 = pci->bars[0];
    if (!bar0 || (bar0 & PCI_BAR_IO)) {
        log_printf(LOG_LEVEL_WARN, "nvme: BAR0 is not a valid MMIO BAR\n");
        kfree(ctrl);
        return -1;
    }

    ctrl->bar_addr = (volatile uint32_t *)(uintptr_t)(bar0 & PCI_BAR_MEM_MASK);

    /* Check for MSI-X capability */
    uint8_t msix_cap = pci_find_capability(pci, PCI_CAP_ID_MSIX);
    if (msix_cap) {
        ctrl->msix_enabled = 1;
        log_printf(LOG_LEVEL_INFO, "nvme: MSI-X capability found\n");
    }

    /* Initialize controller */
    if (nvme_controller_init(ctrl) != 0) {
        log_printf(LOG_LEVEL_WARN, "nvme: controller init failed\n");
        kfree(ctrl);
        return -1;
    }

    /* Identify controller and namespaces */
    if (nvme_identify(ctrl) != 0) {
        log_printf(LOG_LEVEL_WARN, "nvme: identify failed\n");
        kfree(ctrl);
        return -1;
    }

    /* Create I/O queues */
    if (nvme_create_io_queues(ctrl) != 0) {
        log_printf(LOG_LEVEL_WARN, "nvme: I/O queue creation failed\n");
        kfree(ctrl);
        return -1;
    }

    /* Register block device */
    if (ctrl->namespaces) {
        struct nvme_namespace *ns = ctrl->namespaces;

        memset(&ctrl->bdev, 0, sizeof(ctrl->bdev));
        {
            const char *name = "nvme0";
            size_t len = strlen(name);
            if (len < sizeof(ctrl->bdev.name) - 1) {
                memcpy(ctrl->bdev.name, name, len + 1);
            }
        }
        ctrl->bdev.block_size = ns->block_size;
        ctrl->bdev.total_sectors = ns->block_count;
        ctrl->bdev.read = nvme_read;
        ctrl->bdev.write = nvme_write;
        ctrl->bdev.ioctl = NULL;
        ctrl->bdev.priv = ctrl;

        g_active_nvme = ctrl;
        block_dev_register(&ctrl->bdev);

        log_printf(LOG_LEVEL_INFO, "nvme: block device '%s' registered\n",
                   ctrl->bdev.name);
    }

    /* Add to global list */
    ctrl->next = nvme_ctrl_list;
    nvme_ctrl_list = ctrl;

    return 0;
}

/* ================================================================
 * Driver Entry Point
 * ================================================================ */

void nvme_init(void) {
    log_printf(LOG_LEVEL_INFO, "nvme: probing for NVMe controllers...\n");

    struct pci_device *pci = pci_get_device_list();
    int found = 0;

    while (pci) {
        if (pci->class_code == NVME_PCI_CLASS &&
            pci->subclass == NVME_PCI_SUBCLASS &&
            pci->prog_if == NVME_PCI_PROG_IF) {
            log_printf(LOG_LEVEL_INFO,
                       "nvme: found device at %02x:%02x.%x [%04x:%04x]\n",
                       pci->bus, pci->device, pci->function,
                       pci->vendor_id, pci->device_id);

            if (nvme_probe_device(pci) == 0) {
                found++;
            }
        }
        pci = pci->next;
    }

    log_printf(LOG_LEVEL_INFO, "nvme: %d controller(s) initialized\n", found);
}