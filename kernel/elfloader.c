#include "elf.h"
#include "vfs.h"
#include "include/log.h"
#include "include/userspace.h"
#include "mem.h"
#include "pagetable.h"
#include "user.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * elf_load: Load an ELF executable into a new page table.
 * @path:      path to the ELF file in VFS
 * @pml4_out:  output — physical address of the new PML4 table
 * Returns:    user virtual entry point, or NULL on failure.
 *
 * On failure, all allocated resources (pages, page tables) are freed,
 * so the caller does not need to call free_pagetable().
 */
void *elf_load(const char *path, uint64_t *pml4_out) {
    struct file *f = vfs_open(path, 0);
    if (!f) { log_printf(LOG_LEVEL_ERR, "elf_load: open failed %s\n", path); return NULL; }

    Elf64_Ehdr ehdr;
    f->offset = 0;
    ssize_t r = vfs_read(f, &ehdr, sizeof(ehdr));
    if (r != (ssize_t)sizeof(ehdr)) { vfs_close(f); log_printf(LOG_LEVEL_ERR, "elf_load: read ehdr failed\n"); return NULL; }
    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        vfs_close(f); log_printf(LOG_LEVEL_ERR, "elf_load: bad magic\n"); return NULL;
    }
    if (ehdr.e_machine != 0x3E) { vfs_close(f); log_printf(LOG_LEVEL_ERR, "elf_load: not x86-64\n"); return NULL; }

    /* Validate program header table bounds */
    if (ehdr.e_phnum > 128) { vfs_close(f); log_printf(LOG_LEVEL_ERR, "elf_load: too many program headers %u\n", ehdr.e_phnum); return NULL; }
    if (ehdr.e_phentsize < sizeof(Elf64_Phdr)) { vfs_close(f); log_printf(LOG_LEVEL_ERR, "elf_load: bad phentsize %u\n", ehdr.e_phentsize); return NULL; }

    /* first pass: find loadable segments */
    Elf64_Phdr phdr;
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    for (int i = 0; i < ehdr.e_phnum; ++i) {
        f->offset = ehdr.e_phoff + i * ehdr.e_phentsize;
        if (vfs_read(f, &phdr, sizeof(phdr)) != (ssize_t)sizeof(phdr)) {
            log_printf(LOG_LEVEL_WARN, "elf_load: failed to read phdr %d\n", i);
            continue;
        }
        if (phdr.p_type != PT_LOAD) continue;
        if (phdr.p_vaddr < min_vaddr) min_vaddr = phdr.p_vaddr;
        if (phdr.p_vaddr + phdr.p_memsz > max_vaddr) max_vaddr = phdr.p_vaddr + phdr.p_memsz;
    }
    if (min_vaddr == UINT64_MAX) { vfs_close(f); log_printf(LOG_LEVEL_ERR, "elf_load: no loadable segments\n"); return NULL; }

    /* create a new PML4 for this program (kernel-only, fresh user space) */
    uint64_t new_pml4 = clone_kernel_pml4();
    if (!new_pml4 || new_pml4 == get_kernel_cr3()) {
        vfs_close(f);
        log_printf(LOG_LEVEL_ERR, "elf_load: failed to clone kernel pml4\n");
        return NULL;
    }

    /* load segments page-by-page and map into new_pml4 */
    for (int i = 0; i < ehdr.e_phnum; ++i) {
        f->offset = ehdr.e_phoff + i * ehdr.e_phentsize;
        if (vfs_read(f, &phdr, sizeof(phdr)) != (ssize_t)sizeof(phdr)) continue;
        if (phdr.p_type != PT_LOAD) continue;

        uint64_t seg_vstart = phdr.p_vaddr;
        uint64_t seg_filesz = phdr.p_filesz;
        uint64_t seg_memsz = phdr.p_memsz;
        uint64_t file_offset = phdr.p_offset;

        /* Security: Validate segment bounds are within user address space */
        if (seg_vstart > USER_ADDR_MAX || seg_vstart + seg_memsz < seg_vstart ||
            seg_vstart + seg_memsz > USER_ADDR_MAX) {
            vfs_close(f);
            free_pagetable(new_pml4);
            log_printf(LOG_LEVEL_ERR, "elf_load: segment %d vaddr=%p out of bounds\n",
                       i, (void *)(uintptr_t)seg_vstart);
            return NULL;
        }

        uint64_t page_base = seg_vstart & ~0xFFFULL;
        uint64_t page_offset = seg_vstart & 0xFFFULL;
        uint64_t to_alloc = (page_offset + seg_memsz + 4095) & ~0xFFFULL;
        uint64_t pages = to_alloc / 4096;

        /* permissions: user pages with NX for non-executable segments */
        uint64_t flags = PTE_USER;
        if (phdr.p_flags & PF_W) flags |= PTE_RW;
        if (!(phdr.p_flags & 1)) flags |= PTE_NX; /* PF_X=0 → set NX */

        for (uint64_t p = 0; p < pages; ++p) {
            void *phys = alloc_page();
            if (!phys) {
                vfs_close(f);
                free_pagetable(new_pml4);
                log_printf(LOG_LEVEL_ERR, "elf_load: out of phys pages\n");
                return NULL;
            }
            memset(phys, 0, 4096);
            uint64_t va = page_base + p * 4096;
            if (map_page(new_pml4, va, (uint64_t)(uintptr_t)phys, flags) != 0) {
                vfs_close(f);
                free_page(phys);
                free_pagetable(new_pml4);
                log_printf(LOG_LEVEL_ERR, "elf_load: map_page failed\n");
                return NULL;
            }
        }

        /* now read file contents into the mapped physical pages */
        uint64_t remaining = seg_filesz;
        uint64_t read_off = 0;
        while (remaining > 0) {
            uint64_t va = seg_vstart + read_off;
            uint64_t page_vbase = va & ~0xFFFULL;
            uint64_t page_inner = va & 0xFFFULL;
            uint64_t toread = 4096 - page_inner;
            if (toread > remaining) toread = remaining;

            /* Walk page table to find physical page (with PRESENT checks) */
            uint64_t *pml4 = (uint64_t*)phys_to_virt(new_pml4);
            uint64_t pml4_idx = (page_vbase >> 39) & 0x1FF;
            if (!(pml4[pml4_idx] & PTE_PRESENT)) break;
            uint64_t pdpt_phys = pml4[pml4_idx] & PTE_ADDR_MASK;
            uint64_t *pdpt = (uint64_t*)(uintptr_t)pdpt_phys;
            uint64_t pdpt_idx = (page_vbase >> 30) & 0x1FF;
            if (!(pdpt[pdpt_idx] & PTE_PRESENT)) break;
            uint64_t pd_phys = pdpt[pdpt_idx] & PTE_ADDR_MASK;
            uint64_t *pd = (uint64_t*)(uintptr_t)pd_phys;
            uint64_t pd_idx = (page_vbase >> 21) & 0x1FF;
            if (!(pd[pd_idx] & PTE_PRESENT)) break;
            uint64_t pt_phys = pd[pd_idx] & PTE_ADDR_MASK;
            uint64_t *pt = (uint64_t*)(uintptr_t)pt_phys;
            uint64_t pt_idx = (page_vbase >> 12) & 0x1FF;
            if (!(pt[pt_idx] & PTE_PRESENT)) break;
            uint64_t phys_page = pt[pt_idx] & PTE_ADDR_MASK;

            void *dst = (void*)(uintptr_t)(phys_page + page_inner);
            f->offset = file_offset + read_off;
            ssize_t got = vfs_read(f, dst, toread);
            if (got <= 0) break;
            read_off += (uint64_t)got;
            remaining -= (uint64_t)got;
        }

        /* zero BSS beyond filesz up to memsz (already zeroed by memset on pages) */
    }

    void *entry = (void*)(uintptr_t)ehdr.e_entry; /* user virtual entry address */

    /* Security: Validate entry point is within mapped user space */
    if ((uint64_t)(uintptr_t)entry < min_vaddr ||
        (uint64_t)(uintptr_t)entry >= max_vaddr) {
        vfs_close(f);
        free_pagetable(new_pml4);
        log_printf(LOG_LEVEL_ERR, "elf_load: entry point %p outside mapped range [%p, %p)\n",
                   entry, (void *)min_vaddr, (void *)max_vaddr);
        return NULL;
    }

    *pml4_out = new_pml4;
    vfs_close(f);
    log_printf(LOG_LEVEL_INFO, "elf_load: loaded %s entry=%p pml4=%p\n", path, entry, (void*)(uintptr_t)new_pml4);
    return entry;
}

/* helper to create task from ELF path */
int exec_elf(const char *path) {
    uint64_t pml4 = 0;
    void *entry = elf_load(path, &pml4);
    if (!entry) return -1;
    int pid = create_user_task_from_entry((void(*)(void))entry, pml4);
    return pid;
}
