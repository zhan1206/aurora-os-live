/*
 * pagetable.h - Page table management interface
 *
 * Architecture:
 *   - 4-level x86_64 paging: PML4 → PDPT → PD → PT → Page (4KB).
 *   - Kernel identity-maps 0–1GB via 2MB huge pages (PML4[0]).
 *   - User space: PML4[1..255], kernel-half PML4[256..511] shared.
 *   - Copy-On-Write (COW): clone_current_pml4() deep-copies user page
 *     tables, marking leaf pages read-only and incrementing ref_count.
 *     pf_handler_c() handles write faults by copying pages.
 *   - 2MB huge pages in kernel mapping: split on-demand via split_huge_page().
 *   - map_page() handles PTE overwrite: decrements old page's ref_count,
 *     frees it if ref_count reaches 0, then maps the new page.
 *
 * Memory model:
 *   - Physical addresses 0–1GB are identity-mapped (kernel accessible).
 *   - phys_to_virt(pa) = (uint64_t *)(uintptr_t)pa for identity-mapped range.
 *   - Page table pages are allocated via alloc_page() from buddy allocator.
 *   - ref_count is tracked via page descriptors in mem.c.
 */
#ifndef PAGETABLE_H
#define PAGETABLE_H

#include <stdint.h>

/* Page table entry flags (usable by callers) */
#define PTE_PRESENT    0x001ULL
#define PTE_RW         0x002ULL
#define PTE_USER       0x004ULL
#define PTE_PS         0x080ULL  /* Page Size (2MB in PD, 1GB in PDPT) */
#define PTE_NX         (1ULL << 63)

/* Page size (may already be defined in mem.h) */
#ifndef PAGE_SIZE
#define PAGE_SIZE      4096ULL
#endif
#define PAGE_SHIFT     12
#define PAGE_MASK      0xFFFFFFFFFFFFF000ULL

/* Identity-mapped physical memory range (0..KERNEL_PHYS_MAX).
 * The kernel identity-maps physical memory 0..kernel_end (~1GB).
 * phys_to_virt validates that a physical address is within this range. */
#define KERNEL_PHYS_MAX  (0x40000000ULL)  /* 1GB identity-mapped */

/* Convert physical address to kernel virtual address.
 * Physical addresses in the identity-mapped range (0..1GB) are
 * directly accessible as virtual addresses. */
uint64_t *phys_to_virt(uint64_t pa);

/* Physical address mask for PTE (bits 12-51) */
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

void page_table_init(void);
uint64_t get_kernel_cr3(void);

/*
 * clone_current_pml4: COW-aware deep clone of user-space page tables.
 * Allocates new intermediate tables (PDPT/PD/PT) and marks leaf pages
 * read-only with incremented ref_count in both parent and child.
 * Returns: physical address of new PML4.
 */
uint64_t clone_current_pml4(void);
uint64_t clone_kernel_pml4(void);

/* Map a single page. Intermediate tables are always kernel-only. */
int map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags);

/* Convenience: map a user-accessible page. */
int map_user_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags);

/* Unmap a single page without freeing the physical page. */
void unmap_page(uint64_t pml4_phys, uint64_t vaddr);

/* Map a contiguous range. */
int map_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags);

/*
 * free_pagetable: COW-aware recursive free.
 * Decrements ref_count for each leaf page; frees physical pages
 * only when ref_count reaches 0.
 */
void free_pagetable(uint64_t pml4_phys);

/*
 * pf_handler_c: Page fault handler with COW support.
 * @error_code: CPU error code from #PF exception.
 * Called from arch/x86_64/pf_handler.S.
 */
void pf_handler_c(uint64_t error_code);

/*
 * exec_elf: Load an ELF executable and create a new process.
 * @path: VFS path to the ELF binary.
 * Returns: new PID on success, negative on error.
 */
int exec_elf(const char *path);

/*
 * rodata_protect: Mark the kernel's read-only data segment as
 * read-only in the page tables. Called during boot.
 */
void rodata_protect(void);

/*
 * kernel_selftest: Run the kernel self-test suite.
 */
void kernel_selftest(void);

#endif
