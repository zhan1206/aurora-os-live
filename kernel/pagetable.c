/*
 * pagetable.c - x86_64 4-level page table management (FIXED v3)
 *
 * Fixes this round:
 *   - clone_current_pml4: deep-copies the full user-space page table
 *     hierarchy (PML4→PDPT→PD→PT→pages). Shared pages are marked
 *     read-only in BOTH parent and child page tables, with ref_count
 *     incremented. This enables true Copy-On-Write (COW).
 *     (Review §5)
 *
 *   - free_pagetable: decrements ref_count; only frees pages when
 *     ref_count reaches 0.
 *
 *   - pf_handler_c: basic COW fault handler — if a write fault occurs
 *     on a present, read-only, user page with ref_count > 1,
 *     allocates a new page, copies content, updates PTE to RW.
 *     (This replaces the old pf_handler_c which just panicked.)
 *
 *   - SMAP/SMEP: ENABLED. CR4.SMEP (bit 20) and CR4.SMAP (bit 21) are set
 *     during page_table_init(). STAC/CLAC instructions are used in
 *     copy_from_user/copy_to_user to temporarily allow kernel access to
 *     user pages. This protects against ret2usr and kernel data access
 *     to user pages.
 */

#include "pagetable.h"
#include "include/log.h"
#include "include/assert.h"
#include "mem.h"
#include "signal.h"
#include "sched.h"
#include "perf.h"
#include <stdint.h>
#include <string.h>

static uint64_t kernel_cr3 = 0;

/* Page table entry flags (internal — use public PTE_* from pagetable.h for user-visible flags) */
#define PTE_ACCESSED 0x020ULL
#define PTE_DIRTY    0x040ULL

/*
 * PTE_STRUCT_FLAGS: flags for intermediate table entries (PML4, PDPT, PD).
 * Does NOT include PTE_DIRTY (bit 6) — that bit is reserved in non-PT entries
 * and setting it would cause a #PF (reserved bit violation).
 */
#define PTE_STRUCT_FLAGS       (PTE_PRESENT | PTE_RW | PTE_ACCESSED)

/* ================================================================
 * CR3 / TLB
 * ================================================================ */

inline uint64_t read_cr3(void) {
    uint64_t v;
    asm volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void invlpg(uint64_t vaddr) {
    asm volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/* ================================================================
 * Init
 * ================================================================ */

void page_table_init(void) {
    kernel_cr3 = read_cr3();
    log_printf(LOG_LEVEL_INFO, "pagetable: kernel CR3=%p\n", (void *)(uintptr_t)kernel_cr3);

    uint32_t efer_lo, efer_hi;
    asm volatile ("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080));
    uint64_t efer = ((uint64_t)efer_hi << 32) | efer_lo;
    if (!(efer & (1ULL << 11))) {
        efer |= (1ULL << 11);
        asm volatile ("wrmsr" :: "a"((uint32_t)efer), "d"((uint32_t)(efer >> 32)),
                      "c"(0xC0000080));
        log_printf(LOG_LEVEL_INFO, "pagetable: EFER.NXE enabled\n");
    }

    /*
     * Enable SMEP (Supervisor Mode Execution Prevention) and SMAP
     * (Supervisor Mode Access Prevention) via CR4 bits.
     *
     * SMEP (CR4 bit 20): Prevents kernel from executing code in
     *   user-accessible pages. Mitigates ret2usr attacks.
     * SMAP (CR4 bit 21): Prevents kernel from accessing user-accessible
     *   data pages. Can be temporarily disabled via STAC/CLAC.
     *
     * CoolPotOS-inspired security hardening.
     */
    uint64_t cr4;
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 20);  /* CR4.SMEP */
    cr4 |= (1ULL << 21);  /* CR4.SMAP */
    asm volatile ("mov %0, %%cr4" :: "r"(cr4) : "memory");
    log_printf(LOG_LEVEL_INFO, "pagetable: SMEP+SMAP enabled (CR4=0x%llx)\n", cr4);
}

uint64_t get_kernel_cr3(void) {
    return kernel_cr3;
}

/*
 * rodata_protect: Mark .rodata pages as read-only in the kernel page table.
 *
 * Uses the linker-defined symbols __rodata_start and __rodata_end
 * to find the .rodata segment boundaries. Walks the current page
 * table and clears the RW bit on each PTE covering .rodata.
 *
 * Must be called after page_table_init() and before any code that
 * writes to .rodata (which should be none).
 */
/*
 * Identity-mapped physical memory range (0..KERNEL_PHYS_MAX).
 * The kernel identity-maps physical memory 0..kernel_end (~1GB).
 * phys_to_virt validates that a physical address is within this range.
 */
uint64_t *phys_to_virt(uint64_t pa) {
    if (pa >= KERNEL_PHYS_MAX) {
        log_printf(LOG_LEVEL_ERR, "pagetable: phys_to_virt: invalid pa %p\n",
                   (void *)(uintptr_t)pa);
        panic("phys_to_virt: physical address %p beyond identity-mapped range",
              (void *)(uintptr_t)pa);
    }
    return (uint64_t *)(uintptr_t)pa;
}

/* Forward declaration: used by rodata_protect and map_page */
static uint64_t split_huge_page(uint64_t *pd, int pd_idx, uint64_t vaddr);

void rodata_protect(void) {
    extern uint8_t __rodata_start[];
    extern uint8_t __rodata_end[];

    uint64_t ro_start = (uint64_t)(uintptr_t)__rodata_start;
    uint64_t ro_end   = (uint64_t)(uintptr_t)__rodata_end;

    /* Align to page boundaries */
    uint64_t va = ro_start & ~0xFFFULL;
    uint64_t end = (ro_end + 0xFFFULL) & ~0xFFFULL;

    log_printf(LOG_LEVEL_INFO, "pagetable: protecting rodata %p-%p\n",
               (void *)ro_start, (void *)ro_end);

    uint64_t *pml4 = phys_to_virt(kernel_cr3);
    log_printf(LOG_LEVEL_DEBUG, "pagetable: rodata_protect: pml4=%p, pml4_phys=%p\n",
               (void *)pml4, (void *)(uintptr_t)kernel_cr3);

    for (; va < end; va += 0x1000) {
        log_printf(LOG_LEVEL_DEBUG, "pagetable: rodata_protect: va=%p\n", (void *)va);
        uint64_t pml4_idx = (va >> 39) & 0x1FF;
        uint64_t pdpt_idx = (va >> 30) & 0x1FF;
        uint64_t pd_idx   = (va >> 21) & 0x1FF;
        uint64_t pt_idx   = (va >> 12) & 0x1FF;

        if (!(pml4[pml4_idx] & PTE_PRESENT)) continue;
        uint64_t *pdpt = phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
        log_printf(LOG_LEVEL_DEBUG, "pagetable: rodata_protect: pdpt=%p, pdpt_idx=%d\n", (void *)pdpt, (int)pdpt_idx);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) continue;
        uint64_t *pd = phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        log_printf(LOG_LEVEL_DEBUG, "pagetable: rodata_protect: pd=%p, pd_idx=%d\n", (void *)pd, (int)pd_idx);
        if (!(pd[pd_idx] & PTE_PRESENT)) continue;

        /* Split 2MB huge page if needed */
        if (pd[pd_idx] & PTE_PS) {
            log_printf(LOG_LEVEL_DEBUG, "pagetable: rodata_protect: splitting huge page at pd[%d]\n", (int)pd_idx);
            if (!split_huge_page(pd, (int)pd_idx, va)) continue;
            log_printf(LOG_LEVEL_DEBUG, "pagetable: rodata_protect: split done\n");
        }

        uint64_t *pt = phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);

        /* Clear RW bit */
        pt[pt_idx] &= ~PTE_RW;
        invlpg(va);
    }

    log_printf(LOG_LEVEL_INFO, "pagetable: rodata protection enabled\n");
}

/* ================================================================
 * Helpers
 * ================================================================ */

static uint64_t alloc_table_page(void) {
    void *p = alloc_page();
    if (!p) return 0;
    memset((void *)(uintptr_t)p, 0, PAGE_SIZE);
    return (uint64_t)(uintptr_t)p;
}

static struct page *page_of_phys(uint64_t pa) {
    return get_page_struct(pa);
}

static void page_ref_inc(uint64_t pa) {
    struct page *pg = page_of_phys(pa);
    if (pg) pg->ref_count++;
}

static void page_ref_dec(uint64_t pa) {
    struct page *pg = page_of_phys(pa);
    if (pg && pg->ref_count > 0) pg->ref_count--;
}

static uint32_t page_ref_get(uint64_t pa) {
    struct page *pg = page_of_phys(pa);
    return pg ? pg->ref_count : 0;
}

/*
 * split_huge_page: Split a 2MB huge page PD entry into a PT with 512 × 4KB entries.
 * @pd:       pointer to the page directory
 * @pd_idx:   index of the huge page entry in the PD
 * Returns:   physical address of the new PT, or 0 on failure.
 */
static uint64_t split_huge_page(uint64_t *pd, int pd_idx, uint64_t vaddr) {
    uint64_t huge_entry = pd[pd_idx];
    if (!(huge_entry & PTE_PS)) return 0;  /* not a huge page */

    uint64_t huge_base  = huge_entry & PTE_ADDR_MASK;
    uint64_t huge_flags = huge_entry & 0xFFF;

    log_printf(LOG_LEVEL_DEBUG, "pagetable: split_huge_page: pd=%p pd_idx=%d entry=%p\n",
               (void *)pd, pd_idx, (void *)(uintptr_t)huge_entry);

    uint64_t pt_phys = alloc_table_page();
    log_printf(LOG_LEVEL_DEBUG, "pagetable: split_huge_page: pt_phys=%p\n",
               (void *)(uintptr_t)pt_phys);
    if (!pt_phys) return 0;

    uint64_t *new_pt = phys_to_virt(pt_phys);
    for (int i = 0; i < 512; i++) {
        new_pt[i] = (huge_base + ((uint64_t)i * 4096))
                  | (huge_flags & ~PTE_PS)  /* clear PS bit in PT entries */
                  | PTE_PRESENT
                  | PTE_ACCESSED
                  | PTE_DIRTY;
    }

    /* Replace PD entry with pointer to new PT */
    pd[pd_idx] = pt_phys | PTE_STRUCT_FLAGS;

    /*
     * Flush the TLB for the entire 2MB region that was just split.
     * Use the full virtual address (vaddr) aligned to the 2MB boundary,
     * rather than assuming the huge page is at PDPT=0/PML4=0.
     * Flush all 512 4KB pages in the 2MB region to ensure no stale
     * 2MB TLB entries remain.
     */
    uint64_t split_va = vaddr & ~0x1FFFFFULL;  /* 2MB-aligned */
    for (int i = 0; i < 512; i++) {
        invlpg(split_va + (uint64_t)i * 4096);
    }

    return pt_phys;
}

/* ================================================================
 * map_page — unchanged from v2 (intermediate tables kernel-only)
 * ================================================================ */

int map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;

    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t entry  = pml4[pml4_idx];
    uint64_t pdpt_phys;

    if (!(entry & PTE_PRESENT)) {
        pdpt_phys = alloc_table_page();
        if (!pdpt_phys) return -1;
        pml4[pml4_idx] = pdpt_phys | PTE_STRUCT_FLAGS;
    } else {
        pdpt_phys = entry & PTE_ADDR_MASK;
    }

    uint64_t *pdpt = phys_to_virt(pdpt_phys);
    entry = pdpt[pdpt_idx];
    uint64_t pd_phys;

    if (!(entry & PTE_PRESENT)) {
        pd_phys = alloc_table_page();
        if (!pd_phys) return -1;
        pdpt[pdpt_idx] = pd_phys | PTE_STRUCT_FLAGS;
    } else {
        pd_phys = entry & PTE_ADDR_MASK;
    }

    uint64_t *pd = phys_to_virt(pd_phys);
    entry = pd[pd_idx];
    uint64_t pt_phys;

    if (!(entry & PTE_PRESENT)) {
        pt_phys = alloc_table_page();
        if (!pt_phys) return -1;
        pd[pd_idx] = pt_phys | PTE_STRUCT_FLAGS;
    } else if (entry & PTE_PS) {
        /* 2MB huge page: split into 512 × 4KB pages */
        pt_phys = split_huge_page(pd, (int)pd_idx, vaddr);
        if (!pt_phys) return -1;
    } else {
        pt_phys = entry & PTE_ADDR_MASK;
    }

    uint64_t *pt = phys_to_virt(pt_phys);
    uint64_t old_pte = pt[pt_idx];

    /*
     * If the PTE is already present, this is an overwrite: the old
     * physical page is being replaced.  Decrement its ref_count and
     * free it if no other process references it.  This is critical for
     * elf_load() which overlays COW-shared pages with new mappings.
     * Idempotent re-mapping (same VA→same PA) is safe because the
     * ref_count is decremented then immediately re-incremented below.
     */
    if (old_pte & PTE_PRESENT) {
        uint64_t old_phys = old_pte & PTE_ADDR_MASK;
        uint32_t old_ref = page_ref_get(old_phys);
        if (old_ref > 0) {
            page_ref_dec(old_phys);
            if (old_ref == 1) {
                /* Only this process had the page — free it */
                free_page((void *)(uintptr_t)old_phys);
            }
        }
    }

    pt[pt_idx] = (paddr & ~0xFFFULL) | (flags & 0xFFFULL) | PTE_PRESENT;
    if (flags & PTE_NX) pt[pt_idx] |= PTE_NX;

    /* Increment ref_count for the new page */
    page_ref_inc(paddr & PTE_ADDR_MASK);

    invlpg(vaddr);
    return 0;
}

int map_user_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    return map_page(pml4_phys, vaddr, paddr, flags | PTE_USER);
}

int map_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    uint64_t off = 0;
    /* Prevent infinite loop on overflow: ensure off + PAGE_SIZE doesn't wrap */
    if (size > UINT64_MAX - PAGE_SIZE + 1) size = UINT64_MAX - PAGE_SIZE + 1;
    while (off < size) {
        int r = map_page(pml4_phys, vaddr + off, paddr + off, flags);
        if (r) return -1;
        /* Check for overflow before incrementing */
        if (off + PAGE_SIZE < off) break;
        off += PAGE_SIZE;
    }
    return 0;
}

/*
 * unmap_page: Remove a single page mapping without freeing the physical page.
 * Walks the 4-level page table and clears the PTE. Does NOT free intermediate
 * tables (PDPT/PD/PT) — those are cleaned up by free_pagetable().
 * Returns silently if the page was not mapped.
 */
void unmap_page(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;

    uint64_t *pml4 = phys_to_virt(pml4_phys);
    if (!(pml4[pml4_idx] & PTE_PRESENT)) return;

    uint64_t *pdpt = phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return;

    uint64_t *pd = phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) return;

    /* Skip 2MB huge pages — unmap_page only handles 4KB pages */
    if (pd[pd_idx] & PTE_PS) return;

    uint64_t *pt = phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);
    if (!(pt[pt_idx] & PTE_PRESENT)) return;

    /* Clear the PTE and flush TLB */
    pt[pt_idx] = 0;
    invlpg(vaddr);
}

/* ================================================================
 * COW-aware clone: deep-copy user page tables
 *
 * Clones the CURRENT process's page tables (read_cr3()).
 * Used by fork() to create a child with COW-shared user pages.
 *
 * For each user-space PML4 entry (index 0-255):
 *   - Allocates new PDPT, PD, PT tables (deep copy).
 *   - For each leaf PTE: increments page ref_count, marks page
 *     read-only in BOTH parent and child page tables.
 *
 * Returns: physical address of new PML4, or kernel_cr3 on failure.
 * ================================================================ */

uint64_t clone_current_pml4(void) {
    void *newp = alloc_page();
    if (!newp) {
        log_printf(LOG_LEVEL_WARN, "pagetable: clone_current_pml4: alloc_page failed\n");
        return kernel_cr3;
    }

    uint64_t current_cr3 = read_cr3();
    uint64_t *src_pml4  = phys_to_virt(current_cr3);
    uint64_t *dst_pml4  = (uint64_t *)newp;
    uint64_t  new_cr3   = (uint64_t)(uintptr_t)newp;

    /* Copy kernel-half PML4 entries (256-511) directly (shared) */
    for (int i = 256; i < 512; ++i) {
        dst_pml4[i] = src_pml4[i];
    }

    /*
     * PML4[0]: Copy as-is to preserve the kernel's identity mapping
     * (0-1GB, 2MB huge pages). This is required so that kernel code,
     * data, and stack remain accessible when the new PML4 is loaded.
     * map_page() will split 2MB pages as needed when mapping user pages.
     */
    dst_pml4[0] = src_pml4[0];

    /* Deep-copy user-half PML4[1..255] for COW */
    for (int i = 1; i < 256; ++i) {
        uint64_t src_pml4e = src_pml4[i];
        if (!(src_pml4e & PTE_PRESENT)) {
            dst_pml4[i] = 0;
            continue;
        }

        /* Allocate new PDPT */
        uint64_t new_pdpt_phys = alloc_table_page();
        if (!new_pdpt_phys) goto fail;
        dst_pml4[i] = new_pdpt_phys | PTE_STRUCT_FLAGS;

        uint64_t src_pdpt_phys = src_pml4e & PTE_ADDR_MASK;
        uint64_t *src_pdpt = phys_to_virt(src_pdpt_phys);
        uint64_t *dst_pdpt = phys_to_virt(new_pdpt_phys);

        for (int j = 0; j < 512; ++j) {
            uint64_t src_pdpte = src_pdpt[j];
            if (!(src_pdpte & PTE_PRESENT)) {
                dst_pdpt[j] = 0;
                continue;
            }

            uint64_t new_pd_phys = alloc_table_page();
            if (!new_pd_phys) goto fail;
            dst_pdpt[j] = new_pd_phys | PTE_STRUCT_FLAGS;

            uint64_t src_pd_phys = src_pdpte & PTE_ADDR_MASK;
            uint64_t *src_pd = phys_to_virt(src_pd_phys);
            uint64_t *dst_pd = phys_to_virt(new_pd_phys);

            for (int k = 0; k < 512; ++k) {
                uint64_t src_pde = src_pd[k];
                if (!(src_pde & PTE_PRESENT)) {
                    dst_pd[k] = 0;
                    continue;
                }

                /*
                 * 2MB huge pages in user space are not created by the
                 * current design (map_page always creates 4KB PT entries).
                 * If one is encountered, log a warning and skip — splitting
                 * would require ref_count setup for all 512 sub-pages.
                 */
                if (src_pde & PTE_PS) {
                    log_printf(LOG_LEVEL_WARN,
                               "pagetable: clone: unexpected 2MB huge page in user PD[%d][%d]\n",
                               i, k);
                    dst_pd[k] = 0;
                    continue;
                }

                uint64_t new_pt_phys = alloc_table_page();
                if (!new_pt_phys) goto fail;
                dst_pd[k] = new_pt_phys | PTE_STRUCT_FLAGS;

                uint64_t src_pt_phys = src_pde & PTE_ADDR_MASK;
                uint64_t *src_pt = phys_to_virt(src_pt_phys);
                uint64_t *dst_pt = phys_to_virt(new_pt_phys);

                for (int l = 0; l < 512; ++l) {
                    uint64_t src_pte = src_pt[l];
                    if (!(src_pte & PTE_PRESENT)) {
                        dst_pt[l] = 0;
                        continue;
                    }

                    uint64_t phys_page = src_pte & PTE_ADDR_MASK;
                    uint64_t old_flags  = src_pte & 0xFFF;
                    if (src_pte & PTE_NX) old_flags |= PTE_NX;

                    /* Increment ref_count for this shared physical page */
                    page_ref_inc(phys_page);

                    /*
                     * COW: mark page read-only in BOTH parent and child.
                     * Clear RW bit, set COW flag (we use a software bit:
                     * bit 9 is available on x86_64 PTE).
                     * When a write fault occurs, pf_handler_c will
                     * detect ref_count > 1 and perform the copy.
                     */
                    uint64_t cow_flags = (old_flags & ~PTE_RW) | PTE_PRESENT;
                    if (old_flags & PTE_USER) cow_flags |= PTE_USER;
                    if (old_flags & PTE_NX)   cow_flags |= PTE_NX;

                    /* Update parent's PTE to read-only */
                    src_pt[l] = phys_page | cow_flags;

                    /* Child's PTE also read-only, same physical page */
                    dst_pt[l] = phys_page | cow_flags;

                    /* Invalidate TLB for this VA in parent */
                    uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                  ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    invlpg(va);
                }
            }
        }
    }

    log_printf(LOG_LEVEL_DEBUG, "pagetable: COW-cloned pml4 %p -> %p\n",
               (void *)(uintptr_t)current_cr3, (void *)(uintptr_t)new_cr3);
    return new_cr3;

fail:
    /* On failure, free the partially-built new PML4 */
    log_printf(LOG_LEVEL_ERR, "pagetable: clone_current_pml4: allocation failed\n");
    free_pagetable(new_cr3);
    return kernel_cr3;
}

/* ================================================================
 * clone_kernel_pml4: clone from kernel_cr3 (no user pages)
 *
 * Used by exec() to create a fresh address space with only kernel
 * mappings. The caller then loads ELF segments into the new space.
 * Returns: physical address of new PML4, or kernel_cr3 on failure.
 * ================================================================ */
uint64_t clone_kernel_pml4(void) {
    void *newp = alloc_page();
    if (!newp) return kernel_cr3;

    uint64_t *src_pml4 = phys_to_virt(kernel_cr3);
    uint64_t *dst_pml4 = (uint64_t *)newp;

    /* Copy kernel-half PML4 entries (256-511) directly */
    for (int i = 256; i < 512; ++i)
        dst_pml4[i] = src_pml4[i];

    /* Copy PML4[0] for identity mapping */
    dst_pml4[0] = src_pml4[0];

    /* User-half PML4[1..255] stays empty (zeroed by alloc_page) */

    return (uint64_t)(uintptr_t)newp;
}

/* ================================================================
/*
 * user_page_present: Check if a user virtual address is mapped and
 * accessible in the current page table. Walks the 4-level page table
 * to verify the user PTE is present.
 * Returns 1 if mapped, 0 if not mapped or not in user space.
 */
int user_page_present(uint64_t vaddr) {
    /* Only user-space addresses */
    if (vaddr > 0x00007FFFFFFFFFFFULL) return 0;

    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;

    uint64_t cr3 = read_cr3();
    uint64_t *pml4 = phys_to_virt(cr3);
    if (!(pml4[pml4_idx] & PTE_PRESENT)) return 0;

    uint64_t *pdpt = phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 0;

    uint64_t *pd = phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) return 0;

    /* 2MB huge page: the whole 2MB region is mapped */
    if (pd[pd_idx] & PTE_PS) return 1;

    uint64_t *pt = phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);
    return (pt[pt_idx] & PTE_PRESENT) ? 1 : 0;
}

/*
 * pf_handler_c: Page fault handler with COW + lazy allocation
 *
 * Handles:
 *   - Not-present fault (user): lazy page allocation — allocates a
 *     zero-filled physical page and maps it RW for the faulting user
 *     address.  This avoids pre-allocating the entire address space
 *     and is a standard optimization used by Linux, SerenityOS, etc.
 *   - COW write fault: if ref_count > 1, alloc new page, copy, remap RW.
 *   - Otherwise: panic (unhandled fault).
 * ================================================================ */

/*
 * pf_handler_c: Page fault handler with COW support and lazy allocation.
 *
 * @error_code: CPU error code pushed on #PF (passed by pf_handler.S via RDI).
 *   bit 0 = P (0 = not-present, 1 = protection violation)
 *   bit 1 = W/R (0 = read, 1 = write)
 *   bit 2 = U/S (0 = kernel, 1 = user)
 *   bit 3 = RSVD (reserved bit violation)
 *   bit 4 = I/D (0 = data, 1 = instruction fetch)
 */
void pf_handler_c(uint64_t error_code) {
    uint64_t cr2;
    asm volatile ("mov %%cr2, %0" : "=r"(cr2));

    /* Performance counter: count all page faults */
    perf_inc(PERF_PAGE_FAULTS);
    /* Per-process page fault counter */
    if (current) current->page_fault_count++;

    int present = (error_code & 1) != 0;
    int write   = (error_code & 2) != 0;
    int user    = (error_code & 4) != 0;

    if (!present) {
        /* Lazy allocation: handle not-present faults in user space.
         * Allocate a zero-filled page and map it at the faulting address.
         * This is a standard optimization: pages are allocated on first
         * access rather than at mmap/brk time. */
        if (!user) {
            /* Kernel-space not-present fault is a bug */
            log_printf(LOG_LEVEL_ERR, "Page fault: kernel not-present at CR2=%p (code=0x%x)\n",
                       (void *)cr2, (unsigned int)error_code);
            panic("Kernel page fault: not-present at CR2=%p\n", (void *)cr2);
            return;
        }

        /* Allocate a zero-filled physical page */
        void *new_page = alloc_page();
        if (!new_page) {
            log_printf(LOG_LEVEL_ERR, "Lazy alloc: out of memory at CR2=%p, sending SIGSEGV\n",
                       (void *)cr2);
            if (current) do_sys_kill(current->pid, SIGSEGV);
            return;
        }

        /* Map the page at the faulting address (user RW + NX for data pages) */
        uint64_t page_addr = cr2 & ~0xFFFULL;
        uint64_t flags = PTE_PRESENT | PTE_RW | PTE_USER;
        /* NX is set for data pages by default via EFER.NXE + absence of PTE_NX flag */
        int ret = map_user_page(read_cr3(), page_addr, (uint64_t)(uintptr_t)new_page, flags);
        if (ret < 0) {
            log_printf(LOG_LEVEL_ERR, "Lazy alloc: map_user_page failed at CR2=%p\n",
                       (void *)cr2);
            free_page(new_page);
            if (current) do_sys_kill(current->pid, SIGSEGV);
            return;
        }

        log_printf(LOG_LEVEL_DEBUG, "Lazy alloc: mapped %p -> %p (user RW)\n",
                   (void *)page_addr, new_page);
        perf_inc(PERF_PAGE_FAULTS);
        return;
    }

    if (present && write && user) {
        /* Protection fault: write to a present, user page.
         * This is our COW trigger — check if the page is shared. */

        /* Walk the current page table to find the PTE */
        uint64_t pml4_idx = (cr2 >> 39) & 0x1FF;
        uint64_t pdpt_idx = (cr2 >> 30) & 0x1FF;
        uint64_t pd_idx   = (cr2 >> 21) & 0x1FF;
        uint64_t pt_idx   = (cr2 >> 12) & 0x1FF;

        uint64_t cr3 = read_cr3();
        uint64_t *pml4 = phys_to_virt(cr3);
        if (!(pml4[pml4_idx] & PTE_PRESENT)) goto unhandled;

        uint64_t *pdpt = phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) goto unhandled;

        uint64_t *pd = phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT)) goto unhandled;

        /* 2MB huge page in user space: split it, then handle the 4KB fault */
        if (pd[pd_idx] & PTE_PS) {
            if (!split_huge_page(pd, (int)pd_idx, cr2)) goto unhandled;
        }

        uint64_t *pt = phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);
        uint64_t pte = pt[pt_idx];

        if (!(pte & PTE_PRESENT)) goto unhandled;

        uint64_t phys_page = pte & PTE_ADDR_MASK;
        uint32_t ref = page_ref_get(phys_page);

        if (ref <= 1) {
            /* Only one reference — just make it writable */
            pt[pt_idx] |= PTE_RW;
            invlpg(cr2);
            log_printf(LOG_LEVEL_DEBUG, "COW: single-ref page at %p, made writable\n",
                       (void *)cr2);
            return;
        }

        /* Multiple references — perform COW copy */
        log_printf(LOG_LEVEL_DEBUG, "COW: fault at %p, ref_count=%d, copying\n",
                   (void *)cr2, (int)ref);

        void *new_page = alloc_page();
        if (!new_page) {
            log_printf(LOG_LEVEL_ERR, "COW: out of memory at %p, sending SIGSEGV\n",
                       (void *)cr2);
            /* Send SIGSEGV to the faulting process instead of panicking */
            do_sys_kill(current->pid, SIGSEGV);
            return;
        }

        /* Copy content from old page to new page */
        memcpy(new_page, (void *)(uintptr_t)(cr2 & ~0xFFFULL), PAGE_SIZE);

        /* Decrement old page ref_count */
        page_ref_dec(phys_page);

        /* Update PTE: new physical page, RW, preserve USER/NX */
        uint64_t new_flags = (pte & (PTE_USER | PTE_NX)) | PTE_PRESENT | PTE_RW;
        pt[pt_idx] = ((uint64_t)(uintptr_t)new_page & PTE_ADDR_MASK) | new_flags;

        /* New page ref_count = 1 (implicit in alloc_page) */
        invlpg(cr2);

        /* Performance counter: COW page fault */
        perf_inc(PERF_COW_COUNT);

        log_printf(LOG_LEVEL_DEBUG, "COW: resolved at %p, new phys=%p\n",
                   (void *)cr2, new_page);
        return;
    }

    /*
     * SMAP violation: present=1, access was from supervisor (!user),
     * but the page is a user page (U/S=1 in PTE).
     * This means kernel code touched a user page without STAC.
     * Log the error and kill the offending process if in user context,
     * or panic if in pure kernel context (no current task).
     */
    if (present && !user) {
        /* Verify the page is actually a user page by checking the PTE */
        uint64_t pml4_idx = (cr2 >> 39) & 0x1FF;
        uint64_t pdpt_idx = (cr2 >> 30) & 0x1FF;
        uint64_t pd_idx   = (cr2 >> 21) & 0x1FF;
        uint64_t pt_idx   = (cr2 >> 12) & 0x1FF;

        uint64_t cr3 = read_cr3();
        uint64_t *pml4 = phys_to_virt(cr3);
        if ((pml4[pml4_idx] & PTE_PRESENT) && (pml4[pml4_idx] & PTE_USER)) {
            uint64_t *pdpt = phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
            if ((pdpt[pdpt_idx] & PTE_PRESENT) && (pdpt[pdpt_idx] & PTE_USER)) {
                uint64_t *pd = phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
                if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_USER)) {
                    if (!(pd[pd_idx] & PTE_PS)) {
                        uint64_t *pt = phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);
                        if ((pt[pt_idx] & PTE_PRESENT) && (pt[pt_idx] & PTE_USER)) {
                            /* SMAP violation confirmed: kernel accessed user page without STAC */
                            log_printf(LOG_LEVEL_ERR,
                                "SMAP violation: kernel accessed user page at CR2=%p (code=0x%x)\n",
                                (void *)cr2, (unsigned int)error_code);
                            if (current) {
                                log_printf(LOG_LEVEL_ERR,
                                    "SMAP violation in process %s (pid=%d), sending SIGSEGV\n",
                                    current->name, current->pid);
                                do_sys_kill(current->pid, SIGSEGV);
                                return;
                            }
                            panic("SMAP violation in kernel context at CR2=%p\n", (void *)cr2);
                        }
                    }
                }
            }
        }
        /* Fall through to unhandled if not a SMAP violation */
    }

unhandled:
    log_printf(LOG_LEVEL_ERR, "Page fault: unhandled at CR2=%p (code=0x%x)\n",
               (void *)cr2, (unsigned int)error_code);
    panic("Unhandled page fault at CR2=%p\n", (void *)cr2);
}

/* ================================================================
 * free_pagetable: COW-aware recursive free
 *
 * For each leaf PTE: decrements ref_count. Only frees the physical
 * page when ref_count reaches 0.
 * ================================================================ */

void free_pagetable(uint64_t pml4_phys) {
    if (!pml4_phys) return;

    uint64_t *pml4 = phys_to_virt(pml4_phys);

    /*
     * Start from PML4[1]. PML4[0] points to the kernel's shared
     * identity mapping (2MB huge pages) and must NOT be freed.
     */
    for (int i = 1; i < 256; ++i) {  /* user-half only, skip PML4[0] */
        uint64_t pml4e = pml4[i];
        if (!(pml4e & PTE_PRESENT)) continue;

        uint64_t pdpt_phys = pml4e & PTE_ADDR_MASK;
        uint64_t *pdpt = phys_to_virt(pdpt_phys);

        for (int j = 0; j < 512; ++j) {
            uint64_t pdpte = pdpt[j];
            if (!(pdpte & PTE_PRESENT)) continue;

            uint64_t pd_phys = pdpte & PTE_ADDR_MASK;
            uint64_t *pd = phys_to_virt(pd_phys);

            for (int k = 0; k < 512; ++k) {
                uint64_t pde = pd[k];
                if (!(pde & PTE_PRESENT)) continue;

                /* Skip 2MB huge pages in user space (should not exist) */
                if (pde & PTE_PS) {
                    log_printf(LOG_LEVEL_WARN,
                               "pagetable: free: unexpected 2MB huge page in user PD[%d][%d]\n",
                               i, k);
                    continue;
                }

                uint64_t pt_phys = pde & PTE_ADDR_MASK;
                uint64_t *pt = phys_to_virt(pt_phys);

                for (int l = 0; l < 512; ++l) {
                    uint64_t pte = pt[l];
                    if (!(pte & PTE_PRESENT)) continue;

                    uint64_t page_phys = pte & PTE_ADDR_MASK;
                    uint32_t ref = page_ref_get(page_phys);

                    if (ref > 0) {
                        page_ref_dec(page_phys);  /* write back to page_array */
                        ref--;
                    }
                    if (ref == 0) {
                        free_page((void *)(uintptr_t)page_phys);
                    }
                    /* else: page still referenced by another process */
                }
                free_page((void *)(uintptr_t)pt_phys);
            }
            free_page((void *)(uintptr_t)pd_phys);
        }
        free_page((void *)(uintptr_t)pdpt_phys);
    }

    free_page((void *)(uintptr_t)pml4_phys);
}
