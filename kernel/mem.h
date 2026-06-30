/*
 * mem.h - Buddy physical page allocator + slab kernel heap allocator
 *
 * Replaces the original bitmap allocator and no-op kfree with:
 *  - E820 memory map parsing from Multiboot1/Multiboot2 info
 *  - Buddy system for page allocation (order 0..MAX_ORDER)
 *  - Slab-based kmalloc/kfree for small kernel objects
 *  - Spinlock protection for concurrency safety
 */
#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stddef.h>

/* ============ Constants ============ */
#define PAGE_SIZE   4096ULL
#define MAX_ORDER   10                /* max 2^10 = 1024 pages = 4 MiB */
#define MAX_PAGES   (256ULL * 1024 * 1024 / PAGE_SIZE)  /* 256 MiB max */

/* Page frame flags */
#define PAGE_FLAG_FREE      0x01
#define PAGE_FLAG_RESERVED  0x02
#define PAGE_FLAG_KERNEL    0x04
#define PAGE_FLAG_SLAB      0x08

/* Slab cache sizes (power of 2) */
#define SLAB_MIN_SHIFT  5    /* 32 bytes */
#define SLAB_MAX_SHIFT  12   /* 4096 bytes */
#define SLAB_NUM_CACHES (SLAB_MAX_SHIFT - SLAB_MIN_SHIFT + 1)

/* ============ Structures ============ */

/* Page frame descriptor */
struct page {
    uint32_t flags;           /* PAGE_FLAG_* */
    uint32_t order;           /* allocation order (0..MAX_ORDER) */
    uint32_t ref_count;       /* reference count for COW */
    uint64_t phys_addr;       /* physical address of this page */
    struct page *buddy;       /* buddy pointer for buddy system */
    struct page *next;        /* free list link */
    void *slab_cache;         /* owning slab cache if PAGE_FLAG_SLAB */
};

/* Slab cache descriptor */
struct slab_cache {
    size_t obj_size;          /* object size in bytes */
    size_t objs_per_page;     /* objects per page */
    struct page *partial;     /* partially free pages */
    struct page *full;        /* fully allocated pages */
    void *free_list;          /* free object list (intrusive) */
    volatile int growing;     /* prevent concurrent slab_grow() */
};

/* ============ API: Physical Memory ============ */

/*
 * phys_mem_init: Initialize physical memory manager.
 * @mb_info: pointer to Multiboot info structure from GRUB2 (physical addr).
 *           Supports both Multiboot1 and Multiboot2 (auto-detected).
 *           If NULL, falls back to a default 64 MiB.
 *
 * Parses E820/memory map, builds the buddy allocator,
 * and reserves kernel/ACPI/MMIO regions.
 */
void phys_mem_init(void *mb_info);

/*
 * phys_mem_init_uefi: Initialize physical memory manager from UEFI boot info.
 * @bi: pointer to struct uefi_boot_info passed by the UEFI bootloader.
 *
 * Parses the UEFI memory map, builds the buddy allocator,
 * and reserves kernel/ACPI/MMIO regions.
 */
void phys_mem_init_uefi(void *bi);

/*
 * alloc_pages: Allocate 2^order contiguous physical pages.
 * @order: allocation order (0..MAX_ORDER).
 * Returns: physical address of first page, or NULL on failure.
 */
void *alloc_pages(uint32_t order);

/*
 * free_pages: Free 2^order contiguous physical pages.
 * @ptr: physical address returned by alloc_pages.
 * @order: must match the order used at allocation.
 */
void free_pages(void *ptr, uint32_t order);

/*
 * alloc_page: Convenience wrapper for alloc_pages(0).
 */
void *alloc_page(void);

/*
 * free_page: Convenience wrapper for free_pages(ptr, 0).
 */
void free_page(void *page);

/*
 * get_page_struct: Get the page descriptor for a physical address.
 */
struct page *get_page_struct(uint64_t phys_addr);

/* ============ API: Kernel Heap (Slab) ============ */

/*
 * kmalloc: Allocate kernel heap memory.
 * @size: requested size in bytes.
 * Returns: pointer to allocated memory (aligned to sizeof(max_align_t)),
 *          or NULL on failure.
 *
 * Uses slab caches for sizes <= 4096; falls back to page allocation
 * for larger requests.
 */
void *kmalloc(size_t size);

/*
 * kfree: Free memory allocated by kmalloc.
 * @ptr: pointer returned by kmalloc. NULL is safe.
 */
void kfree(void *ptr);

/*
 * slab_init: Initialize the slab allocator subsystem.
 * Must be called after phys_mem_init and before any kmalloc.
 */
void slab_init(void);

/* ============ API: Statistics (for debugging) ============ */

void mem_get_stats(uint64_t *total, uint64_t *free, uint64_t *used);
void slab_get_stats(size_t *total_alloc, size_t *total_free);

#endif /* MEM_H */
