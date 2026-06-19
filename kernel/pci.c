/*
 * pci.c - PCI bus enumeration and configuration space access
 *
 * Implements PCI device discovery using CAM (Configuration Access Mechanism)
 * via I/O ports 0xCF8 (address) and 0xCFC (data).
 */
#include "pci.h"
#include "include/portio.h"
#include "include/log.h"
#include "mem.h"
#include "include/string.h"
#include <stdint.h>

/* PCI configuration address/data ports */
#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* Linked list of discovered PCI devices */
static struct pci_device *pci_device_list = NULL;
static struct pci_device *pci_device_tail = NULL;
static int pci_initialized = 0;

/* ================================================================
 * PCI Configuration Space Access
 * ================================================================ */

static inline uint32_t pci_make_addr(uint8_t bus, uint8_t device,
                                     uint8_t function, uint8_t offset) {
    return (uint32_t)(0x80000000U
           | ((uint32_t)bus << 16)
           | ((uint32_t)(device & 0x1F) << 11)
           | ((uint32_t)(function & 0x07) << 8)
           | (uint32_t)(offset & 0xFC));
}

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset) {
    uint32_t addr = pci_make_addr(bus, device, function, offset);
    outb(PCI_CONFIG_ADDR, (uint8_t)(addr & 0xFF));
    outb(PCI_CONFIG_ADDR + 1, (uint8_t)((addr >> 8) & 0xFF));
    outb(PCI_CONFIG_ADDR + 2, (uint8_t)((addr >> 16) & 0xFF));
    outb(PCI_CONFIG_ADDR + 3, (uint8_t)((addr >> 24) & 0xFF));
    /* Using 32-bit inl via two 16-bit reads on x86 properly */
    uint32_t lo = (uint32_t)inb(PCI_CONFIG_DATA)
               | ((uint32_t)inb(PCI_CONFIG_DATA + 1) << 8)
               | ((uint32_t)inb(PCI_CONFIG_DATA + 2) << 16)
               | ((uint32_t)inb(PCI_CONFIG_DATA + 3) << 24);
    return lo;
}

uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset) {
    uint32_t val = pci_read_config32(bus, device, function, offset & 0xFC);
    return (uint16_t)((val >> ((offset & 0x02) * 8)) & 0xFFFF);
}

uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function,
                         uint8_t offset) {
    uint32_t val = pci_read_config32(bus, device, function, offset & 0xFC);
    return (uint8_t)((val >> ((offset & 0x03) * 8)) & 0xFF);
}

void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint32_t value) {
    uint32_t addr = pci_make_addr(bus, device, function, offset);
    outb(PCI_CONFIG_ADDR, (uint8_t)(addr & 0xFF));
    outb(PCI_CONFIG_ADDR + 1, (uint8_t)((addr >> 8) & 0xFF));
    outb(PCI_CONFIG_ADDR + 2, (uint8_t)((addr >> 16) & 0xFF));
    outb(PCI_CONFIG_ADDR + 3, (uint8_t)((addr >> 24) & 0xFF));
    outb(PCI_CONFIG_DATA, (uint8_t)(value & 0xFF));
    outb(PCI_CONFIG_DATA + 1, (uint8_t)((value >> 8) & 0xFF));
    outb(PCI_CONFIG_DATA + 2, (uint8_t)((value >> 16) & 0xFF));
    outb(PCI_CONFIG_DATA + 3, (uint8_t)((value >> 24) & 0xFF));
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint16_t value) {
    uint32_t cur = pci_read_config32(bus, device, function, offset & 0xFC);
    int shift = (offset & 0x02) * 8;
    cur = (cur & ~(0xFFFFU << shift)) | ((uint32_t)value << shift);
    pci_write_config32(bus, device, function, offset & 0xFC, cur);
}

void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function,
                       uint8_t offset, uint8_t value) {
    uint32_t cur = pci_read_config32(bus, device, function, offset & 0xFC);
    int shift = (offset & 0x03) * 8;
    cur = (cur & ~(0xFFU << shift)) | ((uint32_t)value << shift);
    pci_write_config32(bus, device, function, offset & 0xFC, cur);
}

/* ================================================================
 * BAR Size Probing
 * ================================================================ */

uint32_t pci_bar_size(uint8_t bus, uint8_t device, uint8_t function,
                      uint8_t offset) {
    uint32_t orig = pci_read_config32(bus, device, function, offset);
    pci_write_config32(bus, device, function, offset, 0xFFFFFFFFU);
    uint32_t result = pci_read_config32(bus, device, function, offset);
    pci_write_config32(bus, device, function, offset, orig);

    if (result == 0 || result == 0xFFFFFFFFU) return 0;

    /* IO BAR */
    if (orig & PCI_BAR_IO) {
        return (~(result & PCI_BAR_IO_MASK) + 1) & PCI_BAR_IO_MASK;
    }

    /* Memory BAR */
    return (~(result & PCI_BAR_MEM_MASK) + 1) & PCI_BAR_MEM_MASK;
}

void pci_get_bar(struct pci_device *dev, int bar_idx,
                 struct pci_bar_info *info) {
    if (bar_idx < 0 || bar_idx >= PCI_MAX_BARS || !dev || !info) return;

    uint32_t bar = dev->bars[bar_idx];
    memset(info, 0, sizeof(*info));

    if (bar == 0) return;

    info->base = bar;
    info->size = dev->bar_sizes[bar_idx];

    if (bar & PCI_BAR_IO) {
        info->is_io = 1;
        info->base = bar & PCI_BAR_IO_MASK;
    } else {
        info->is_io = 0;
        info->is_64bit = ((bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64);
        info->is_prefetchable = ((bar & PCI_BAR_PREFETCHABLE) != 0);
        info->base = bar & PCI_BAR_MEM_MASK;
    }
}

/* ================================================================
 * PCI Bus Scanning
 * ================================================================ */

static void pci_add_device(struct pci_device *dev) {
    if (pci_device_tail) {
        pci_device_tail->next = dev;
    } else {
        pci_device_list = dev;
    }
    pci_device_tail = dev;
    dev->next = NULL;
}

static void pci_scan_function(uint8_t bus, uint8_t device, uint8_t function) {
    uint16_t vendor_id = pci_read_config16(bus, device, function,
                                           PCI_CONFIG_VENDOR_ID);
    if (vendor_id == 0xFFFF) return;

    struct pci_device *dev = (struct pci_device *)kmalloc(sizeof(*dev));
    if (!dev) return;

    dev->vendor_id = vendor_id;
    dev->device_id = pci_read_config16(bus, device, function,
                                       PCI_CONFIG_DEVICE_ID);
    dev->class_code = pci_read_config8(bus, device, function,
                                       PCI_CONFIG_CLASS_CODE);
    dev->subclass   = pci_read_config8(bus, device, function,
                                       PCI_CONFIG_SUBCLASS);
    dev->prog_if    = pci_read_config8(bus, device, function,
                                       PCI_CONFIG_PROG_IF);
    dev->revision   = pci_read_config8(bus, device, function,
                                       PCI_CONFIG_REVISION_ID);
    dev->header_type = pci_read_config8(bus, device, function,
                                        PCI_CONFIG_HEADER_TYPE);
    dev->bus        = bus;
    dev->device     = device;
    dev->function   = function;
    dev->irq_line   = pci_read_config8(bus, device, function,
                                       PCI_CONFIG_IRQ_LINE);
    dev->irq_pin    = pci_read_config8(bus, device, function,
                                       PCI_CONFIG_IRQ_PIN);

    dev->subsystem_vendor = pci_read_config16(bus, device, function,
                                              PCI_CONFIG_SUBSYS_VENDOR);
    dev->subsystem_id     = pci_read_config16(bus, device, function,
                                              PCI_CONFIG_SUBSYS_ID);

    /* Read BARs */
    for (int i = 0; i < PCI_MAX_BARS; i++) {
        uint8_t bar_offset = (uint8_t)(PCI_CONFIG_BAR0 + i * 4);
        dev->bars[i] = pci_read_config32(bus, device, function, bar_offset);
        dev->bar_sizes[i] = pci_bar_size(bus, device, function, bar_offset);
    }

    pci_add_device(dev);

    log_printf(LOG_LEVEL_INFO,
               "PCI: %02x:%02x.%x [%04x:%04x] class=%02x.%02x irq=%d\n",
               bus, device, function, dev->vendor_id, dev->device_id,
               dev->class_code, dev->subclass, dev->irq_line);
}

static void pci_scan_device(uint8_t bus, uint8_t device) {
    uint16_t vendor = pci_read_config16(bus, device, 0, PCI_CONFIG_VENDOR_ID);
    if (vendor == 0xFFFF) return;

    pci_scan_function(bus, device, 0);

    uint8_t header_type = pci_read_config8(bus, device, 0,
                                           PCI_CONFIG_HEADER_TYPE);
    if ((header_type & 0x80) != 0) {
        for (uint8_t func = 1; func < 8; func++) {
            uint16_t v = pci_read_config16(bus, device, func,
                                           PCI_CONFIG_VENDOR_ID);
            if (v != 0xFFFF) {
                pci_scan_function(bus, device, func);
            }
        }
    }
}

static void pci_scan_bus(uint8_t bus) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        pci_scan_device(bus, dev);
    }
}

void pci_scan(void) {
    uint8_t header_type = pci_read_config8(0, 0, 0, PCI_CONFIG_HEADER_TYPE);
    if ((header_type & 0x80) == 0) {
        /* Single host controller */
        pci_scan_bus(0);
    } else {
        /* Multiple buses: host controller at 0,0,0 is PCI-to-PCI bridge */
        for (uint8_t func = 0; func < 8; func++) {
            uint16_t vendor = pci_read_config16(0, 0, func,
                                                PCI_CONFIG_VENDOR_ID);
            if (vendor == 0xFFFF) break;
            pci_scan_bus(func);
        }
    }

    log_printf(LOG_LEVEL_INFO, "PCI: scan complete\n");
}

void pci_init(void) {
    if (pci_initialized) return;
    pci_initialized = 1;

    log_printf(LOG_LEVEL_INFO, "PCI: initializing bus...\n");
    pci_scan();
}

/* ================================================================
 * Device Lookup
 * ================================================================ */

struct pci_device *pci_find_device(uint16_t vendor, uint16_t device) {
    struct pci_device *dev = pci_device_list;
    while (dev) {
        if (dev->vendor_id == vendor && dev->device_id == device) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

struct pci_device *pci_find_by_class(uint8_t class_code, uint8_t subclass) {
    struct pci_device *dev = pci_device_list;
    while (dev) {
        if (dev->class_code == class_code && dev->subclass == subclass) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

struct pci_device *pci_get_device_list(void) {
    return pci_device_list;
}

/* ================================================================
 * Device Control
 * ================================================================ */

void pci_enable_bus_mastering(struct pci_device *dev) {
    if (!dev) return;
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function,
                                     PCI_CONFIG_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_write_config16(dev->bus, dev->device, dev->function,
                       PCI_CONFIG_COMMAND, cmd);
    log_printf(LOG_LEVEL_DEBUG,
               "PCI: bus mastering enabled for %02x:%02x.%x\n",
               dev->bus, dev->device, dev->function);
}

/* ================================================================
 * Capability List
 * ================================================================ */

uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id) {
    if (!dev) return 0;

    uint16_t status = pci_read_config16(dev->bus, dev->device, dev->function,
                                        PCI_CONFIG_STATUS);
    if (!(status & 0x0010)) return 0;  /* No capabilities list */

    uint8_t ptr = pci_read_config8(dev->bus, dev->device, dev->function,
                                   PCI_CONFIG_CAP_PTR);
    ptr &= 0xFC;

    int count = 0;
    while (ptr && count < 48) {
        uint8_t id = pci_read_config8(dev->bus, dev->device, dev->function,
                                      ptr);
        if (id == cap_id) return ptr;
        ptr = pci_read_config8(dev->bus, dev->device, dev->function,
                               ptr + 1);
        ptr &= 0xFC;
        count++;
    }

    return 0;
}

uint32_t pci_read_capability(struct pci_device *dev, uint8_t cap_offset,
                             uint8_t reg_offset) {
    if (!dev || !cap_offset) return 0;
    return pci_read_config32(dev->bus, dev->device, dev->function,
                             cap_offset + reg_offset);
}