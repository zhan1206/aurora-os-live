/*
 * boot_info.h - Shared boot information structure
 *
 * Used by both the UEFI bootloader (boot/efi_main.c) and the kernel
 * (kernel/main.c) to pass boot-time data (memory map, framebuffer).
 *
 * For Multiboot boot, the kernel uses the Multiboot info structure.
 * For UEFI boot, the kernel uses struct uefi_boot_info.
 */
#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include <stdint.h>

/* Magic value indicating UEFI boot (passed as first argument to kernel_main) */
#define UEFI_BOOT_MAGIC   0xC0DEB007

/* Maximum number of UEFI memory map entries we can store */
#define UEFI_MMAP_MAX_ENTRIES  256

/* UEFI memory map entry (simplified from EFI_MEMORY_DESCRIPTOR) */
struct uefi_mmap_entry {
    uint64_t phys_start;       /* Physical start address */
    uint64_t num_pages;        /* Number of 4K pages */
    uint64_t attributes;       /* EFI_MEMORY_* flags */
    uint32_t type;             /* EfiConventionalMemory, etc. */
    uint32_t pad;
};

/* UEFI memory types (same as EFI memory types) */
#define UEFI_MMAP_RESERVED        0
#define UEFI_MMAP_LOADER_CODE     1
#define UEFI_MMAP_LOADER_DATA     2
#define UEFI_MMAP_BOOT_CODE       3
#define UEFI_MMAP_BOOT_DATA       4
#define UEFI_MMAP_RUNTIME_CODE    5
#define UEFI_MMAP_RUNTIME_DATA    6
#define UEFI_MMAP_CONVENTIONAL    7
#define UEFI_MMAP_UNUSABLE        8
#define UEFI_MMAP_ACPI_RECLAIM    9
#define UEFI_MMAP_ACPI_NVS       10
#define UEFI_MMAP_MMIO           11
#define UEFI_MMAP_MMIO_PORT      12
#define UEFI_MMAP_PAL_CODE       13
#define UEFI_MMAP_PERSISTENT     14

/* UEFI boot information structure */
struct uefi_boot_info {
    uint32_t magic;                             /* UEFI_BOOT_MAGIC */
    uint32_t version;                           /* 1 */

    /* Memory map */
    uint32_t mmap_num_entries;                  /* Number of entries in mmap[] */
    uint32_t mmap_desc_size;                    /* Size of each EFI_MEMORY_DESCRIPTOR */
    uint64_t mmap_key;                          /* Memory map key (for reference) */
    struct uefi_mmap_entry mmap[UEFI_MMAP_MAX_ENTRIES];

    /* Framebuffer (from GOP) */
    uint64_t fb_addr;                           /* Physical address of framebuffer */
    uint32_t fb_width;                          /* Horizontal resolution */
    uint32_t fb_height;                         /* Vertical resolution */
    uint32_t fb_pitch;                          /* Bytes per scanline */
    uint32_t fb_bpp;                            /* Bits per pixel (typically 32) */
    uint8_t  fb_red_mask_size;                  /* Red mask size (bits) */
    uint8_t  fb_red_mask_pos;                   /* Red mask position */
    uint8_t  fb_green_mask_size;                /* Green mask size (bits) */
    uint8_t  fb_green_mask_pos;                 /* Green mask position */
    uint8_t  fb_blue_mask_size;                 /* Blue mask size (bits) */
    uint8_t  fb_blue_mask_pos;                  /* Blue mask position */
    uint8_t  fb_pixel_format;                   /* EFI_GRAPHICS_PIXEL_FORMAT */
    uint8_t  fb_valid;                          /* 1 if framebuffer is valid */
};

#endif /* BOOT_INFO_H */