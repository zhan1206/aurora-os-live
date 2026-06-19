/*
 * pci.h - PCI bus definitions and driver interface
 */
#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* ================================================================
 * PCI Configuration Space Register Offsets
 * ================================================================ */
#define PCI_CONFIG_VENDOR_ID        0x00
#define PCI_CONFIG_DEVICE_ID        0x02
#define PCI_CONFIG_COMMAND          0x04
#define PCI_CONFIG_STATUS           0x06
#define PCI_CONFIG_REVISION_ID      0x08
#define PCI_CONFIG_PROG_IF          0x09
#define PCI_CONFIG_SUBCLASS         0x0A
#define PCI_CONFIG_CLASS_CODE       0x0B
#define PCI_CONFIG_CACHE_LINE_SIZE  0x0C
#define PCI_CONFIG_LATENCY_TIMER    0x0D
#define PCI_CONFIG_HEADER_TYPE      0x0E
#define PCI_CONFIG_BIST             0x0F
#define PCI_CONFIG_BAR0             0x10
#define PCI_CONFIG_BAR1             0x14
#define PCI_CONFIG_BAR2             0x18
#define PCI_CONFIG_BAR3             0x1C
#define PCI_CONFIG_BAR4             0x20
#define PCI_CONFIG_BAR5             0x24
#define PCI_CONFIG_CARDBUS_CIS      0x28
#define PCI_CONFIG_SUBSYS_VENDOR    0x2C
#define PCI_CONFIG_SUBSYS_ID        0x2E
#define PCI_CONFIG_CAP_PTR          0x34
#define PCI_CONFIG_IRQ_LINE         0x3C
#define PCI_CONFIG_IRQ_PIN          0x3D
#define PCI_CONFIG_MIN_GRANT        0x3E
#define PCI_CONFIG_MAX_LATENCY      0x3F

/* ================================================================
 * PCI Class Codes
 * ================================================================ */
#define PCI_CLASS_UNCLASSIFIED      0x00
#define PCI_CLASS_MASS_STORAGE      0x01
#define PCI_CLASS_NETWORK           0x02
#define PCI_CLASS_DISPLAY           0x03
#define PCI_CLASS_MULTIMEDIA        0x04
#define PCI_CLASS_MEMORY            0x05
#define PCI_CLASS_BRIDGE            0x06
#define PCI_CLASS_SERIAL_BUS        0x0C

/* ================================================================
 * PCI Command Register Bits
 * ================================================================ */
#define PCI_CMD_IO_SPACE            0x0001
#define PCI_CMD_MEMORY_SPACE        0x0002
#define PCI_CMD_BUS_MASTER          0x0004
#define PCI_CMD_SPECIAL             0x0008
#define PCI_CMD_MEM_WRITE_INVAL     0x0010
#define PCI_CMD_VGA_PALETTE         0x0020
#define PCI_CMD_PARITY              0x0040
#define PCI_CMD_SERR                0x0100
#define PCI_CMD_FAST_BACK           0x0200
#define PCI_CMD_INT_DISABLE         0x0400

/* ================================================================
 * PCI BAR bit definitions
 * ================================================================ */
#define PCI_BAR_IO                  0x1
#define PCI_BAR_TYPE_32             0x0
#define PCI_BAR_TYPE_64             0x4
#define PCI_BAR_TYPE_MASK           0x6
#define PCI_BAR_PREFETCHABLE        0x8
#define PCI_BAR_IO_MASK             0xFFFFFFFCU
#define PCI_BAR_MEM_MASK            0xFFFFFFF0U

/* ================================================================
 * PCI Device Structure
 * ================================================================ */
#define PCI_MAX_BARS 6

struct pci_device {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint8_t  header_type;
    uint8_t  irq_line;
    uint8_t  irq_pin;
    uint32_t bars[PCI_MAX_BARS];
    uint32_t bar_sizes[PCI_MAX_BARS];
    uint32_t subsystem_vendor;
    uint32_t subsystem_id;
    struct pci_device *next;
};

/* ================================================================
 * PCI BAR info (after probing size)
 * ================================================================ */
struct pci_bar_info {
    uint32_t base;
    uint32_t size;
    uint8_t  is_io;
    uint8_t  is_64bit;
    uint8_t  is_prefetchable;
};

/* ================================================================
 * PCI Function Declarations
 * ================================================================ */
void pci_init(void);
void pci_scan(void);
struct pci_device *pci_find_device(uint16_t vendor, uint16_t device);
struct pci_device *pci_find_by_class(uint8_t class_code, uint8_t subclass);
struct pci_device *pci_get_device_list(void);

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset);
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset);
uint8_t  pci_read_config8(uint8_t bus, uint8_t device, uint8_t function,
                          uint8_t offset);

void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint32_t value);
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint16_t value);
void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function,
                       uint8_t offset, uint8_t value);

void pci_enable_bus_mastering(struct pci_device *dev);
void pci_get_bar(struct pci_device *dev, int bar_idx, struct pci_bar_info *info);
uint32_t pci_bar_size(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

/* ================================================================
 * PCI Capability list parsing
 * ================================================================ */
#define PCI_CAP_ID_MSI       0x05
#define PCI_CAP_ID_VENDOR    0x09
#define PCI_CAP_ID_MSIX      0x11

uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id);
uint32_t pci_read_capability(struct pci_device *dev, uint8_t cap_offset,
                             uint8_t reg_offset);

#endif /* PCI_H */