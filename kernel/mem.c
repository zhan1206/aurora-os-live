/*
 * mem.c - Buddy system physical page allocator + slab kernel heap
 * Author: AuroraOS Team (self-developed from scratch)
 * Date: 2026-06-12
 *
 * Design:
 *   Physical allocator: Buddy system with MAX_ORDER=10.
 *     - Maintains free_area[] arrays, one per order.
 *     - alloc_pages(order): finds smallest available block >= order,
 *       splits down, returns physical address.
 *     - free_pages(ptr, order): merges with buddy if free, coalesces up.
 *     - Parses Multiboot2 E820 memory map to avoid ACPI/MMIO regions.
 *   Kernel heap: Simple slab allocator with 8 size classes (32..4096).
 *     - Each slab cache manages pages divided into fixed-size objects.
 *     - Free list is intrusive (first 8 bytes of free object = next ptr).
 *     - For sizes > 4096, falls back to alloc_pages directly.
 *   Concurrency: Simple spinlock (CLI/STI based, single-core safe).
 *
 *   Locking rules:
 *     - buddy_lock protects: free_area[], page->flags, page->order.
 *     - slab_lock protects: all slab_cache structures and free lists.
 *     - Lock order: always buddy_lock before slab_lock.
 */

#include "mem.h"
#include "include/log.h"
#include "include/assert.h"
#include "../boot/boot_info.h"
#include "perf.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ================================================================
 * Multiboot constants (Multiboot1 + Multiboot2)
 *
 * AuroraOS boots via Multiboot1 (GRUB2 \`multiboot\` command).
 * The kernel also supports Multiboot2 for forward compatibility.
 * ================================================================ */

/* Multiboot1 magic */
#define MULTIBOOT1_MAGIC        0x2BADB002

/* Multiboot1 info structure fields (offsets from mb_info base) */
#define MB1_FLAGS_OFFSET        0
#define MB1_MEM_LOWER_OFFSET    4
#define MB1_MEM_UPPER_OFFSET    8
#define MB1_MMAP_LENGTH_OFFSET  44
#define MB1_MMAP_ADDR_OFFSET    48

/* Multiboot1 flags */
#define MB1_FLAG_MEMINFO        (1 << 0)   /* mem_* fields valid */
#define MB1_FLAG_MMAP           (1 << 6)   /* mmap_* fields valid */

/* Multiboot1 memory map entry */
struct mb1_mmap_entry {
    uint32_t size;          /* entry size (>= 20) */
    uint64_t base_addr;     /* 64-bit base address */
    uint64_t length;        /* 64-bit length */
    uint32_t type;          /* 1=available, 2=reserved, 3=ACPI, 4=NVS, 5=bad */
};
#define MB1_MMAP_AVAILABLE  1

/* Multiboot2 magic */
#define MULTIBOOT2_MAGIC        0x36d76289

/* Multiboot2 tag type constants */
#define MULTIBOOT2_TAG_END      0
#define MULTIBOOT2_TAG_MMAP     6
#define MULTIBOOT2_TAG_BASIC    4

/* Multiboot2 tag header */
struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

/* Multiboot2 memory map entry */
struct mb2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

#define MB2_MMAP_AVAILABLE  1
#define MB2_MMAP_RESERVED   2
#define MB2_MMAP_ACPI       3
#define MB2_MMAP_NVS        4
#define MB2_MMAP_BADRAM     5

/* ================================================================
 * Spinlock (atomic lock cmpxchg with pause — SMP-safe)
 *
 * Upgraded from CLI/STI (single-core only) to atomic test-and-set
 * for SMP correctness. Uses lock cmpxchg with PAUSE in the spin loop
 * to reduce bus contention on multi-core systems.
 * ================================================================ */
typedef struct spinlock {
    volatile uint32_t locked;
} spinlock_t;

static inline void spin_lock(spinlock_t *lock) {
    while (1) {
        uint32_t old = 0;
        uint32_t new = 1;
        asm volatile (
            "lock cmpxchgl %2, %1"
            : "=a"(old), "+m"(lock->locked)
            : "r"(new), "0"(old)
            : "memory"
        );
        if (old == 0) break;
        asm volatile ("pause" ::: "memory");
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    asm volatile ("movl $0, %0" : "=m"(lock->locked) : : "memory");
}

static spinlock_t buddy_lock_ = {0};
static spinlock_t slab_lock_  = {0};

#define buddy_lock()   spin_lock(&buddy_lock_)
#define buddy_unlock() spin_unlock(&buddy_lock_)
#define slab_lock()    spin_lock(&slab_lock_)
#define slab_unlock()  spin_unlock(&slab_lock_)

/* ================================================================
 * Buddy system internal data
 * ================================================================ */

/* Maximum pages the buddy system can manage */
#define BUDDY_MAX_PAGES  (256 * 1024 * 1024 / 4096)  /* 256 MiB → 65536 pages */

/* Page descriptor array */
static struct page page_array[BUDDY_MAX_PAGES];
static uint64_t total_phys_pages = 0;
static uint64_t phys_mem_base = 0x100000;   /* 1 MiB */
static uint64_t phys_mem_end  = 0;

/* Free area heads (one linked list per order) */
static struct page *free_area[MAX_ORDER + 1];

/* Track alloc/free for stats */
static uint64_t stat_total_pages = 0;
static uint64_t stat_used_pages  = 0;

/* ================================================================
 * Page descriptor helpers
 * ================================================================ */

static inline uint64_t page_to_pfn(struct page *p) {
    return (uint64_t)(p - page_array);
}

static inline struct page *pfn_to_page(uint64_t pfn) {
    if (pfn >= BUDDY_MAX_PAGES) return NULL;
    return &page_array[pfn];
}

static inline uint64_t phys_to_pfn(uint64_t pa) {
    if (pa < phys_mem_base) return (uint64_t)-1;
    return (pa - phys_mem_base) / PAGE_SIZE;
}

static inline uint64_t pfn_to_phys(uint64_t pfn) {
    return phys_mem_base + pfn * PAGE_SIZE;
}

/* Buddy calculation: page ^ (1 << order) gives the buddy */
static inline struct page *get_buddy(struct page *p, uint32_t order) {
    uint64_t pfn = page_to_pfn(p);
    uint64_t buddy_pfn = pfn ^ (1ULL << order);
    if (buddy_pfn >= BUDDY_MAX_PAGES) return NULL;
    return &page_array[buddy_pfn];
}

/* ================================================================
 * Free list operations (buddy system)
 * ================================================================ */

static void list_add(struct page **head, struct page *p) {
    p->next = *head;
    *head = p;
}

static void list_del(struct page **head, struct page *p) {
    if (*head == p) {
        *head = p->next;
        p->next = NULL;
        return;
    }
    struct page *prev = *head;
    while (prev && prev->next != p) prev = prev->next;
    if (prev) {
        prev->next = p->next;
        p->next = NULL;
    }
}

/* ================================================================
 * Buddy allocator core
 * ================================================================ */

/* Mark a page range as reserved */
static void buddy_mark_reserved(uint64_t start_pfn, uint64_t end_pfn) {
    if (start_pfn >= BUDDY_MAX_PAGES) return;
    if (end_pfn > BUDDY_MAX_PAGES) end_pfn = BUDDY_MAX_PAGES;
    for (uint64_t pfn = start_pfn; pfn < end_pfn; ++pfn) {
        page_array[pfn].flags |= PAGE_FLAG_RESERVED;
    }
}

/* Mark a page range as available and add to free lists */
static void buddy_mark_available(uint64_t start_pfn, uint64_t end_pfn) {
    if (start_pfn >= BUDDY_MAX_PAGES) return;
    if (end_pfn > BUDDY_MAX_PAGES) end_pfn = BUDDY_MAX_PAGES;

    /* Add pages at order 0; buddy_merge in free_pages will coalesce them */
    for (uint64_t pfn = start_pfn; pfn < end_pfn; ++pfn) {
        if (page_array[pfn].flags & PAGE_FLAG_RESERVED) continue;
        struct page *p = &page_array[pfn];
        p->flags      = PAGE_FLAG_FREE;
        p->order      = 0;
        p->ref_count  = 0;
        p->phys_addr  = pfn_to_phys(pfn);
        p->buddy      = NULL;
        p->next       = NULL;
        p->slab_cache = NULL;
        list_add(&free_area[0], p);
        stat_total_pages++;
    }
}

/* Split a block of given order into two blocks of order-1 */
static struct page *buddy_split(uint32_t order) {
    if (order == 0 || order > MAX_ORDER) return NULL;
    if (!free_area[order]) return NULL;

    struct page *p = free_area[order];
    list_del(&free_area[order], p);

    uint32_t new_order = order - 1;
    uint64_t pfn = page_to_pfn(p);
    uint64_t buddy_pfn = pfn + (1ULL << new_order);

    struct page *left  = &page_array[pfn];
    struct page *right = &page_array[buddy_pfn];

    left->flags  = PAGE_FLAG_FREE;
    left->order  = new_order;
    left->phys_addr = pfn_to_phys(pfn);

    right->flags  = PAGE_FLAG_FREE;
    right->order  = new_order;
    right->phys_addr = pfn_to_phys(buddy_pfn);

    list_add(&free_area[new_order], left);
    list_add(&free_area[new_order], right);

    return left;
}

/* Try to merge a page with its buddy, recursively */
static struct page *buddy_merge(struct page *p) {
    uint32_t order = p->order;
    if (order >= MAX_ORDER) return p;

    struct page *buddy = get_buddy(p, order);
    if (!buddy) return p;
    if (!(buddy->flags & PAGE_FLAG_FREE)) return p;
    if (buddy->order != order) return p;

    /* Remove both from free list */
    list_del(&free_area[order], p);
    list_del(&free_area[order], buddy);

    /* Merge into larger block */
    uint64_t pfn = page_to_pfn(p);
    uint64_t buddy_pfn = page_to_pfn(buddy);
    uint64_t base_pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;

    struct page *merged = &page_array[base_pfn];
    merged->flags = PAGE_FLAG_FREE;
    merged->order = order + 1;
    merged->phys_addr = pfn_to_phys(base_pfn);

    list_add(&free_area[order + 1], merged);

    /* Recursively try to merge further */
    return buddy_merge(merged);
}

/* ================================================================
 * Public API: Physical Memory
 * ================================================================ */

void phys_mem_init(void *mb_info) {
    /* Initialize page array */
    memset(page_array, 0, sizeof(page_array));
    for (uint32_t o = 0; o <= MAX_ORDER; ++o) free_area[o] = NULL;

    uint64_t total_ram = 0;

    if (mb_info) {
        /*
         * Detect Multiboot version by checking magic.
         * The magic is passed separately to kernel_main and must
         * be stored before calling phys_mem_init.  Since we receive
         * only the info pointer, we use a heuristic:
         *   - Multiboot1 info: starts with uint32_t flags at offset 0
         *   - Multiboot2 info: starts with uint32_t total_size, then uint32_t reserved
         *
         * We check if offset 0 looks like a valid MB1 flags value
         * (low bits set, high bits zero).
         */
        uint32_t first_word = *(uint32_t *)mb_info;

        /* Heuristic: MB1 flags usually have bits 0-6 set and no high bits */
        if ((first_word & 0xFFFF0000) == 0 && (first_word & 0x7F) != 0) {
            /* === Multiboot1 parsing === */
            uint32_t flags = first_word;
            uint8_t *info = (uint8_t *)mb_info;

            /* Basic memory info (mem_lower, mem_upper in KB) */
            if (flags & MB1_FLAG_MEMINFO) {
                uint32_t mem_lower = *(uint32_t *)(info + MB1_MEM_LOWER_OFFSET);
                uint32_t mem_upper = *(uint32_t *)(info + MB1_MEM_UPPER_OFFSET);
                total_ram = (uint64_t)(mem_lower + mem_upper) * 1024;
                /* mem_upper is KB above 1MB, so total = (lower + upper) KB */
                if (total_ram > 256ULL * 1024 * 1024)
                    total_ram = 256ULL * 1024 * 1024;
            }

            /* E820 memory map (more precise than mem_upper) */
            if (flags & MB1_FLAG_MMAP) {
                uint32_t mmap_length = *(uint32_t *)(info + MB1_MMAP_LENGTH_OFFSET);
                uint32_t mmap_addr   = *(uint32_t *)(info + MB1_MMAP_ADDR_OFFSET);
                uint64_t e820_total  = 0;

                uint8_t *entries = (uint8_t *)(uintptr_t)mmap_addr;
                uint8_t *end     = entries + mmap_length;

                while (entries < end) {
                    struct mb1_mmap_entry *e = (struct mb1_mmap_entry *)entries;
                    if (e->size < 20) break;  /* minimum entry size */

                    if (e->type == MB1_MMAP_AVAILABLE) {
                        uint64_t end_addr = e->base_addr + e->length;
                        if (end_addr > e820_total && e->base_addr < 256ULL * 1024 * 1024)
                            e820_total = (end_addr > 256ULL * 1024 * 1024)
                                         ? 256ULL * 1024 * 1024 : end_addr;
                    }

                    entries += e->size;
                }

                if (e820_total > 0) total_ram = e820_total;
            }

        } else {
            /* === Multiboot2 parsing === */
            struct mb2_tag *tag = (struct mb2_tag *)((uint8_t*)mb_info + 8);
            uint32_t total_size = *(uint32_t*)mb_info;

            while (tag->type != MULTIBOOT2_TAG_END &&
                   (uintptr_t)tag < (uintptr_t)mb_info + total_size) {

                if (tag->type == MULTIBOOT2_TAG_BASIC) {
                    uint32_t *basic = (uint32_t*)((uint8_t*)tag + 8);
                    uint32_t mem_upper = basic[1];
                    total_ram = (uint64_t)(mem_upper + 1024) * 1024;
                    if (total_ram > 256ULL * 1024 * 1024) total_ram = 256ULL * 1024 * 1024;
                }

                if (tag->type == MULTIBOOT2_TAG_MMAP) {
                    uint32_t entry_size  = *(uint32_t*)((uint8_t*)tag + 8);
                    uint8_t *entries = (uint8_t*)tag + 16;
                    uint32_t tag_end = (uint32_t)((uintptr_t)tag + tag->size);

                    while ((uintptr_t)entries + entry_size <= (uintptr_t)tag_end) {
                        struct mb2_mmap_entry *e = (struct mb2_mmap_entry*)entries;
                        if (e->type == MB2_MMAP_AVAILABLE) {
                            uint64_t end_addr = e->base_addr + e->length;
                            if (end_addr > total_ram && e->base_addr < 256ULL * 1024 * 1024)
                                total_ram = (end_addr > 256ULL * 1024 * 1024)
                                            ? 256ULL * 1024 * 1024 : end_addr;
                        }
                        entries += entry_size;
                    }
                }

                uint32_t tag_size = tag->size;
                if (tag_size < 8) tag_size = 8;
                tag_size = (tag_size + 7) & ~7U;
                tag = (struct mb2_tag *)((uint8_t*)tag + tag_size);
            }
        }
    }

    /* Fallback: assume 64 MiB */
    if (total_ram == 0) total_ram = 64ULL * 1024 * 1024;

    phys_mem_end = total_ram;
    total_phys_pages = total_ram / PAGE_SIZE;

    log_printf(LOG_LEVEL_INFO, "phys_mem: total RAM = %d MiB, %d pages\n",
               (int)(total_ram / (1024*1024)),
               (int)total_phys_pages);

    /* Reserve kernel + heap regions BEFORE marking pages available */
    uint64_t kernel_reserved_pages = (2 * 1024 * 1024) / PAGE_SIZE;
    buddy_mark_reserved(0, kernel_reserved_pages);

    uint64_t heap_start_pfn = (8 * 1024 * 1024) / PAGE_SIZE;
    uint64_t heap_end_pfn   = (32 * 1024 * 1024) / PAGE_SIZE;
    if (heap_end_pfn > total_phys_pages) heap_end_pfn = total_phys_pages;
    buddy_mark_reserved(heap_start_pfn, heap_end_pfn);

    /* Mark remaining pages as available.
     * buddy_mark_available skips RESERVED pages, so kernel+heap
     * pages won't be added to free lists. */
    buddy_mark_available(kernel_reserved_pages, total_phys_pages);

    log_printf(LOG_LEVEL_INFO, "phys_mem: buddy system initialized, free pages = %d\n",
               (int)stat_total_pages);
}

/*
 * phys_mem_init_uefi: Initialize physical memory from UEFI memory map.
 * @bi_raw: pointer to struct uefi_boot_info from the UEFI bootloader.
 *
 * Parses the UEFI memory map entries, marks available conventional memory
 * as free, and marks reserved/ACPI/MMIO/other regions as reserved.
 */
void phys_mem_init_uefi(void *bi_raw) {
    struct uefi_boot_info *bi = (struct uefi_boot_info *)bi_raw;

    /* Initialize page array */
    memset(page_array, 0, sizeof(page_array));
    for (uint32_t o = 0; o <= MAX_ORDER; ++o) free_area[o] = NULL;

    uint64_t total_ram = 0;
    uint64_t highest_available = 0;

    /* First pass: find the highest available memory address */
    for (uint32_t i = 0; i < bi->mmap_num_entries; i++) {
        uint64_t end_addr = bi->mmap[i].phys_start +
                            bi->mmap[i].num_pages * PAGE_SIZE;
        if (end_addr > highest_available) {
            highest_available = end_addr;
        }
    }

    /* Cap at 256 MiB (matching the existing buddy allocator limit) */
    if (highest_available > 256ULL * 1024 * 1024) {
        highest_available = 256ULL * 1024 * 1024;
    }

    total_ram = highest_available;
    phys_mem_end = total_ram;
    total_phys_pages = total_ram / PAGE_SIZE;

    log_printf(LOG_LEVEL_INFO, "phys_mem_uefi: total RAM = %d MiB, %d pages\n",
               (int)(total_ram / (1024*1024)),
               (int)total_phys_pages);

    /* Second pass: mark each entry as reserved or available */
    for (uint32_t i = 0; i < bi->mmap_num_entries; i++) {
        uint64_t start = bi->mmap[i].phys_start;
        uint64_t end   = start + bi->mmap[i].num_pages * PAGE_SIZE;

        /* Clamp to our managed range */
        if (start >= total_ram) continue;
        if (end > total_ram) end = total_ram;

        uint64_t start_pfn = phys_to_pfn(start);
        uint64_t end_pfn   = phys_to_pfn(end);

        if (start_pfn >= BUDDY_MAX_PAGES) continue;
        if (end_pfn > BUDDY_MAX_PAGES) end_pfn = BUDDY_MAX_PAGES;

        uint32_t type = bi->mmap[i].type;

        if (type == UEFI_MMAP_CONVENTIONAL) {
            /* Available memory: mark as free */
            /* But reserve kernel and heap regions first */
            /* (We'll do that after the pass) */
        } else {
            /* Reserved, ACPI, MMIO, etc.: mark as reserved */
            buddy_mark_reserved(start_pfn, end_pfn);
        }
    }

    /* Reserve kernel + heap regions BEFORE marking pages available */
    uint64_t kernel_reserved_pages = (2 * 1024 * 1024) / PAGE_SIZE;
    buddy_mark_reserved(0, kernel_reserved_pages);

    uint64_t heap_start_pfn = (8 * 1024 * 1024) / PAGE_SIZE;
    uint64_t heap_end_pfn   = (32 * 1024 * 1024) / PAGE_SIZE;
    if (heap_end_pfn > total_phys_pages) heap_end_pfn = total_phys_pages;
    buddy_mark_reserved(heap_start_pfn, heap_end_pfn);

    /* Mark conventional memory regions as available based on the
     * UEFI memory map. We do NOT do a blanket mark_available first,
     * because that would incorrectly add non-Conventional pages
     * (ACPI, MMIO, reserved) to the free lists, causing double-counting
     * and potential buddy list corruption. */
    for (uint32_t i = 0; i < bi->mmap_num_entries; i++) {
        uint64_t start = bi->mmap[i].phys_start;
        uint64_t end   = start + bi->mmap[i].num_pages * PAGE_SIZE;

        if (start >= total_ram) continue;
        if (end > total_ram) end = total_ram;

        uint64_t start_pfn = phys_to_pfn(start);
        uint64_t end_pfn   = phys_to_pfn(end);

        if (start_pfn >= BUDDY_MAX_PAGES) continue;
        if (end_pfn > BUDDY_MAX_PAGES) end_pfn = BUDDY_MAX_PAGES;

        uint32_t type = bi->mmap[i].type;

        if (type == UEFI_MMAP_CONVENTIONAL) {
            buddy_mark_available(start_pfn, end_pfn);
        }
    }

    log_printf(LOG_LEVEL_INFO, "phys_mem_uefi: buddy system initialized, free pages = %d\n",
               (int)stat_total_pages);
}

void *alloc_pages(uint32_t order) {
    if (order > MAX_ORDER) return NULL;

    buddy_lock();

    /* Find the smallest available order >= requested */
    uint32_t current_order = order;
    while (current_order <= MAX_ORDER && !free_area[current_order])
        current_order++;

    if (current_order > MAX_ORDER) {
        /*
         * No single block large enough.
         * Fallback: allocate 2^order individual pages.
         * They won't be contiguous, but callers of alloc_pages(N)
         * should handle this. For order 0 this is a no-op.
         */
        if (order == 0) {
            buddy_unlock();
            log_printf(LOG_LEVEL_WARN, "alloc_pages: out of memory (order=%d)\n", (int)order);
            return NULL;
        }
        /* For order > 0: just warn and return NULL */
        buddy_unlock();
        log_printf(LOG_LEVEL_WARN, "alloc_pages: out of memory (order=%d)\n", (int)order);
        return NULL;
    }

    /* Split down to the requested order */
    while (current_order > order) {
        buddy_split(current_order);
        current_order--;
    }

    /* Take the first page from free_area[order] */
    struct page *p = free_area[order];
    if (!p) {
        buddy_unlock();
        log_printf(LOG_LEVEL_WARN, "alloc_pages: ASSERT: free_area[%d] is NULL after split\n", (int)order);
        return NULL;
    }
    list_del(&free_area[order], p);

    p->flags &= ~PAGE_FLAG_FREE;
    p->order = order;
    p->ref_count = 1;

    stat_used_pages += (1ULL << order);

    buddy_unlock();

    /* Zero the page(s) for security */
    uint64_t pa = p->phys_addr;
    void *va = (void*)(uintptr_t)pa;
    memset(va, 0, (size_t)(PAGE_SIZE << order));

    log_printf(LOG_LEVEL_DEBUG, "alloc_pages: order=%d pa=%p\n", (int)order, (void*)(uintptr_t)pa);
    return (void*)(uintptr_t)pa;
}

void free_pages(void *ptr, uint32_t order) {
    if (!ptr) return;
    if (order > MAX_ORDER) return;

    uint64_t pa = (uint64_t)(uintptr_t)ptr;
    if (pa < phys_mem_base) return;

    uint64_t pfn = phys_to_pfn(pa);
    if (pfn == (uint64_t)-1 || pfn >= total_phys_pages) return;

    buddy_lock();

    struct page *p = &page_array[pfn];
    if (p->flags & PAGE_FLAG_RESERVED) {
        buddy_unlock();
        return; /* cannot free reserved pages */
    }

    p->flags |= PAGE_FLAG_FREE;
    p->order = order;
    p->ref_count = 0;

    list_add(&free_area[order], p);
    stat_used_pages -= (1ULL << order);

    /* Try to merge with buddy */
    buddy_merge(p);

    buddy_unlock();
}

void *alloc_page(void) {
    return alloc_pages(0);
}

void free_page(void *page) {
    free_pages(page, 0);
}

struct page *get_page_struct(uint64_t phys_addr) {
    uint64_t pfn = phys_to_pfn(phys_addr);
    if (pfn == (uint64_t)-1 || pfn >= total_phys_pages) return NULL;
    return &page_array[pfn];
}

/* ================================================================
 * Slab allocator
 * ================================================================ */

static struct slab_cache slab_caches[SLAB_NUM_CACHES];

/* Get slab cache index for a given size */
static inline int slab_size_to_index(size_t size) {
    if (size <= 32)   return 0;
    if (size <= 64)   return 1;
    if (size <= 128)  return 2;
    if (size <= 256)  return 3;
    if (size <= 512)  return 4;
    if (size <= 1024) return 5;
    if (size <= 2048) return 6;
    return 7; /* 4096 */
}

static inline size_t slab_index_to_size(int idx) {
    return (size_t)(32 << idx);  /* 32, 64, 128, 256, 512, 1024, 2048, 4096 */
}

/* Allocate a new slab page for a cache and carve it into objects */
static int slab_grow(struct slab_cache *cache) {
    void *page = alloc_page();
    if (!page) return -1;

    uint8_t *base = (uint8_t*)page;
    size_t obj_size = cache->obj_size;

    /* Align obj_size to at least sizeof(void*) for intrusive free list */
    if (obj_size < sizeof(void*)) obj_size = sizeof(void*);

    /* Carve page into objects, link them into free list */
    for (size_t i = 0; i + obj_size <= PAGE_SIZE; i += obj_size) {
        void *obj = base + i;
        *(void**)obj = cache->free_list;
        cache->free_list = obj;
    }

    /* Register this page in the page descriptor */
    struct page *pg = get_page_struct((uint64_t)(uintptr_t)page);
    if (pg) {
        pg->flags |= PAGE_FLAG_SLAB;
        pg->slab_cache = cache;
    }

    return 0;
}

void slab_init(void) {
    for (int i = 0; i < SLAB_NUM_CACHES; ++i) {
        slab_caches[i].obj_size = slab_index_to_size(i);
        slab_caches[i].objs_per_page = PAGE_SIZE / slab_caches[i].obj_size;
        slab_caches[i].partial = NULL;
        slab_caches[i].full = NULL;
        slab_caches[i].free_list = NULL;
    }
    log_printf(LOG_LEVEL_INFO, "slab: initialized %d caches (32..4096 bytes)\n", SLAB_NUM_CACHES);
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* Performance counter: kmalloc call */
    perf_inc(PERF_MALLOC_COUNT);

    if (size <= PAGE_SIZE) {
        /* Slab allocation */
        int idx = slab_size_to_index(size);
        struct slab_cache *cache = &slab_caches[idx];

        slab_lock();

        /* If no free objects, release lock, grow cache, then retry.
         * This avoids lock ordering violation: slab_grow calls alloc_page
         * which acquires buddy_lock, and buddy_lock must be acquired
         * BEFORE slab_lock per the lock ordering rules. */
        if (!cache->free_list) {
            slab_unlock();
            if (slab_grow(cache) != 0) {
                log_printf(LOG_LEVEL_WARN, "kmalloc: slab grow failed (size=%d)\n", (int)size);
                return NULL;
            }
            /* Re-acquire and retry — free_list should be non-NULL now */
            slab_lock();
            if (!cache->free_list) {
                slab_unlock();
                return NULL;
            }
        }

        /* Pop from free list */
        void *obj = cache->free_list;
        cache->free_list = *(void**)obj;

        /* Zero the memory for security (prevent info leak) */
        memset(obj, 0, size);

        slab_unlock();
        return obj;
    }

    /* Large allocation: use page allocator directly */
    /* Align size to page boundary, calculate order */
    size_t pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t order = 0;
    while ((1ULL << order) < pages_needed && order <= MAX_ORDER) order++;
    if (order > MAX_ORDER) return NULL;

    return alloc_pages(order);
}

void kfree(void *ptr) {
    if (!ptr) return;

    /* Performance counter: kfree call */
    perf_inc(PERF_FREE_COUNT);

    uint64_t pa = (uint64_t)(uintptr_t)ptr;
    struct page *pg = get_page_struct(pa);

    if (pg && (pg->flags & PAGE_FLAG_SLAB)) {
        /* Slab object: return to free list */
        struct slab_cache *cache = (struct slab_cache*)pg->slab_cache;
        if (!cache) {
            /* Page was allocated but cache pointer lost, free as page */
            free_page(ptr);
            return;
        }

        slab_lock();

        /* Zero memory before returning to free list (prevent info leak) */
        memset(ptr, 0, cache->obj_size);

        *(void**)ptr = cache->free_list;
        cache->free_list = ptr;

        slab_unlock();
        return;
    }

    /* Not a slab page: treat as page allocation.  alloc_pages() stores the
     * order in the page descriptor, so we can free it correctly. */
    uint32_t order = pg ? pg->order : 0;
    free_pages(ptr, order);
}

/* ================================================================
 * Statistics
 * ================================================================ */

void mem_get_stats(uint64_t *total, uint64_t *free, uint64_t *used) {
    *total = stat_total_pages * PAGE_SIZE;
    *used  = stat_used_pages  * PAGE_SIZE;
    *free  = *total - *used;
}

void slab_get_stats(size_t *total_alloc, size_t *total_free) {
    *total_alloc = 0;
    *total_free  = 0;
    /* Simplified: count objects in free lists across all caches */
    for (int i = 0; i < SLAB_NUM_CACHES; ++i) {
        void *obj = slab_caches[i].free_list;
        while (obj) {
            *total_free += slab_caches[i].obj_size;
            obj = *(void**)obj;
        }
    }
}
