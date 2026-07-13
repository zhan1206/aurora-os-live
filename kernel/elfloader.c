#include "elf.h"
#include "vfs.h"
#include "include/log.h"
#include "include/userspace.h"
#include "mem.h"
#include "pagetable.h"
#include "user.h"
#include "aslr.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* PIE default base address (used if ASLR is unavailable) */
#define PIE_DEFAULT_BASE    0x555555554000ULL
#define USER_STACK_PAGES    16
#define USER_STACK_SIZE     (USER_STACK_PAGES * PAGE_SIZE)

/* Internal structure for .dynamic parsing results */
struct elf_dyn_info {
    uint64_t rela_addr;
    uint64_t rela_size;
    uint64_t rela_ent;
    uint64_t symtab;
    uint64_t strtab;
    int      has_rela;
};

/*
 * elf_resolve_va: Walk a 4-level page table to translate a virtual
 * address to its physical address. Returns 0 if not mapped.
 */
static uint64_t elf_resolve_va(uint64_t pml4_phys, uint64_t va) {
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t idx = (va >> 39) & 0x1FF;
    if (!(pml4[idx] & PTE_PRESENT)) return 0;

    uint64_t *pdpt = phys_to_virt(pml4[idx] & PTE_ADDR_MASK);
    idx = (va >> 30) & 0x1FF;
    if (!(pdpt[idx] & PTE_PRESENT)) return 0;

    uint64_t *pd = phys_to_virt(pdpt[idx] & PTE_ADDR_MASK);
    idx = (va >> 21) & 0x1FF;
    if (!(pd[idx] & PTE_PRESENT)) return 0;

    uint64_t *pt = phys_to_virt(pd[idx] & PTE_ADDR_MASK);
    idx = (va >> 12) & 0x1FF;
    if (!(pt[idx] & PTE_PRESENT)) return 0;

    return (pt[idx] & PTE_ADDR_MASK) | (va & 0xFFF);
}

/*
 * elf_poke64: Write a 64-bit value to a virtual address in a given
 * page table. Returns 0 on success, -1 if the address is not mapped.
 */
static int elf_poke64(uint64_t pml4_phys, uint64_t va, uint64_t val) {
    uint64_t phys = elf_resolve_va(pml4_phys, va);
    if (!phys) return -1;
    *(uint64_t *)(uintptr_t)phys = val;
    return 0;
}

/*
 * elf_poke32: Write a 32-bit value to a virtual address in a given
 * page table. Returns 0 on success, -1 if the address is not mapped.
 */
static int elf_poke32(uint64_t pml4_phys, uint64_t va, uint32_t val) {
    uint64_t phys = elf_resolve_va(pml4_phys, va);
    if (!phys) return -1;
    *(uint32_t *)(uintptr_t)phys = val;
    return 0;
}

/*
 * elf_load_dynamic: Parse the .dynamic section of an ELF to find
 * the RELA table, symbol table, and string table.
 *
 * @f:       open file handle
 * @phdrs:   array of program headers
 * @phnum:   number of program headers
 * @base:    load base address (0 for ET_EXEC, ASLR base for ET_DYN)
 * @dyn:     output — filled with parsed .dynamic info
 * Returns:  0 on success, -1 on failure.
 */
static int elf_load_dynamic(struct file *f, Elf64_Phdr *phdrs, int phnum,
                             uint64_t base, struct elf_dyn_info *dyn) {
    (void)base;
    memset(dyn, 0, sizeof(*dyn));

    /* Find the PT_DYNAMIC segment */
    Elf64_Phdr *dyn_phdr = NULL;
    for (int i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_phdr = &phdrs[i];
            break;
        }
    }
    if (!dyn_phdr) {
        log_printf(LOG_LEVEL_WARN, "elf_load_dynamic: no PT_DYNAMIC segment\n");
        return -1;
    }

    /* Read the .dynamic section from the file */
    uint64_t dyn_size = dyn_phdr->p_filesz;
    if (dyn_size == 0 || dyn_size > 65536) {
        log_printf(LOG_LEVEL_ERR, "elf_load_dynamic: bad .dynamic size %lu\n",
                   (unsigned long)dyn_size);
        return -1;
    }

    size_t num_entries = (size_t)(dyn_size / sizeof(Elf64_Dyn));
    Elf64_Dyn *dyn_entries = (Elf64_Dyn *)kmalloc((size_t)dyn_size);
    if (!dyn_entries) {
        log_printf(LOG_LEVEL_ERR, "elf_load_dynamic: kmalloc failed\n");
        return -1;
    }

    f->offset = dyn_phdr->p_offset;
    ssize_t r = vfs_read(f, dyn_entries, (size_t)dyn_size);
    if (r != (ssize_t)dyn_size) {
        log_printf(LOG_LEVEL_ERR, "elf_load_dynamic: read .dynamic failed\n");
        kfree(dyn_entries);
        return -1;
    }

    /* Parse the dynamic entries */
    for (size_t i = 0; i < num_entries; ++i) {
        int64_t tag = dyn_entries[i].d_tag;
        uint64_t val = dyn_entries[i].d_val;
        switch (tag) {
        case DT_NULL:
            goto done;
        case DT_RELA:
            dyn->rela_addr = val;
            dyn->has_rela = 1;
            break;
        case DT_RELASZ:
            dyn->rela_size = val;
            break;
        case DT_RELAENT:
            dyn->rela_ent = val;
            break;
        case DT_SYMTAB:
            dyn->symtab = val;
            break;
        case DT_STRTAB:
            dyn->strtab = val;
            break;
        default:
            break;
        }
    }

done:
    kfree(dyn_entries);
    log_printf(LOG_LEVEL_INFO,
               "elf_load_dynamic: RELA at vaddr=%p size=%lu symtab=%p strtab=%p\n",
               (void *)(uintptr_t)(base + dyn->rela_addr),
               (unsigned long)dyn->rela_size,
               (void *)(uintptr_t)(base + dyn->symtab),
               (void *)(uintptr_t)(base + dyn->strtab));
    return 0;
}

/*
 * elf_apply_relocations: Apply ELF RELA relocations for a PIE executable.
 *
 * Handles: R_X86_64_RELATIVE, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT,
 *          R_X86_64_64, R_X86_64_PC32, R_X86_64_IRELATIVE
 *
 * For symbol-based relocations, reads the symbol table from the
 * already-loaded segments via the page table.
 */
static int elf_apply_relocations(uint64_t pml4, uint64_t base,
                                  struct elf_dyn_info *dyn,
                                  Elf64_Phdr *phdrs, int phnum,
                                  struct file *f) {
    if (!dyn->has_rela || dyn->rela_size == 0) {
        log_printf(LOG_LEVEL_INFO, "elf_apply_relocations: no RELA, skipping\n");
        return 0;
    }
    if (dyn->rela_ent < sizeof(Elf64_Rela)) {
        log_printf(LOG_LEVEL_ERR, "elf_apply_relocations: bad RELA entry size %lu\n",
                   (unsigned long)dyn->rela_ent);
        return -1;
    }

    /* Find the file offset of the RELA table */
    uint64_t rela_file_off = 0;
    int found_seg = 0;
    for (int i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t seg_start = phdrs[i].p_vaddr;
        uint64_t seg_end   = seg_start + phdrs[i].p_memsz;
        if (dyn->rela_addr >= seg_start && dyn->rela_addr < seg_end) {
            rela_file_off = dyn->rela_addr - seg_start + phdrs[i].p_offset;
            found_seg = 1;
            break;
        }
    }
    if (!found_seg) {
        log_printf(LOG_LEVEL_ERR, "elf_apply_relocations: RELA not in any segment\n");
        return -1;
    }

    /* Read the entire RELA table from the file */
    size_t num_rela = (size_t)(dyn->rela_size / sizeof(Elf64_Rela));
    Elf64_Rela *rela = (Elf64_Rela *)kmalloc((size_t)dyn->rela_size);
    if (!rela) {
        log_printf(LOG_LEVEL_ERR, "elf_apply_relocations: kmalloc failed\n");
        return -1;
    }

    f->offset = rela_file_off;
    ssize_t r = vfs_read(f, rela, (size_t)dyn->rela_size);
    if (r != (ssize_t)dyn->rela_size) {
        log_printf(LOG_LEVEL_ERR, "elf_apply_relocations: read RELA failed\n");
        kfree(rela);
        return -1;
    }

    /* Read the symbol table from the file for symbol-based relocations.
     * Estimate the number of entries: the symbol table usually ends at
     * the string table. Use a reasonable upper bound. */
    Elf64_Sym *symtab = NULL;
    size_t sym_count = 0;
    if (dyn->symtab != 0 && dyn->strtab > dyn->symtab) {
        uint64_t sym_size = dyn->strtab - dyn->symtab;
        sym_count = (size_t)(sym_size / sizeof(Elf64_Sym));
        if (sym_count > 16384) sym_count = 16384; /* safety cap */

        /* Find the file offset of the symbol table */
        uint64_t sym_file_off = 0;
        int sym_found = 0;
        for (int i = 0; i < phnum; ++i) {
            if (phdrs[i].p_type != PT_LOAD) continue;
            if (dyn->symtab >= phdrs[i].p_vaddr &&
                dyn->symtab < phdrs[i].p_vaddr + phdrs[i].p_memsz) {
                sym_file_off = dyn->symtab - phdrs[i].p_vaddr + phdrs[i].p_offset;
                sym_found = 1;
                break;
            }
        }
        if (sym_found && sym_count > 0) {
            symtab = (Elf64_Sym *)kmalloc(sym_count * sizeof(Elf64_Sym));
            if (symtab) {
                f->offset = sym_file_off;
                r = vfs_read(f, symtab, sym_count * sizeof(Elf64_Sym));
                if (r <= 0) {
                    kfree(symtab);
                    symtab = NULL;
                    sym_count = 0;
                }
            }
        }
    }

    /* Process each relocation */
    int errors = 0;
    for (size_t i = 0; i < num_rela; ++i) {
        uint64_t r_offset = rela[i].r_offset;
        uint64_t r_info   = rela[i].r_info;
        int64_t  r_addend = rela[i].r_addend;
        uint32_t r_type   = ELF64_R_TYPE(r_info);
        uint32_t r_sym    = ELF64_R_SYM(r_info);

        uint64_t target_va = base + r_offset;
        uint64_t sym_val   = 0;

        /* Resolve symbol value if needed */
        if (r_type == R_X86_64_GLOB_DAT || r_type == R_X86_64_JUMP_SLOT ||
            r_type == R_X86_64_64) {
            if (symtab && r_sym < sym_count) {
                sym_val = base + symtab[r_sym].st_value;
            }
        }

        switch (r_type) {
        case R_X86_64_RELATIVE:
            /* *loc = base + addend */
            if (elf_poke64(pml4, target_va, base + (uint64_t)r_addend) != 0) {
                log_printf(LOG_LEVEL_WARN,
                           "elf_apply_relocations: R_RELATIVE poke failed at %p\n",
                           (void *)(uintptr_t)target_va);
                ++errors;
            }
            break;

        case R_X86_64_64:
            /* *loc = sym_val + addend */
            if (elf_poke64(pml4, target_va, sym_val + (uint64_t)r_addend) != 0) {
                log_printf(LOG_LEVEL_WARN,
                           "elf_apply_relocations: R_64 poke failed at %p\n",
                           (void *)(uintptr_t)target_va);
                ++errors;
            }
            break;

        case R_X86_64_PC32: {
            /* *(uint32_t*)loc = sym_val + addend - loc */
            uint64_t result = sym_val + (uint64_t)r_addend - target_va;
            if (elf_poke32(pml4, target_va, (uint32_t)result) != 0) {
                log_printf(LOG_LEVEL_WARN,
                           "elf_apply_relocations: R_PC32 poke failed at %p\n",
                           (void *)(uintptr_t)target_va);
                ++errors;
            }
            break;
        }

        case R_X86_64_GLOB_DAT:
            /* *loc = sym_val + addend */
            if (elf_poke64(pml4, target_va, sym_val + (uint64_t)r_addend) != 0) {
                log_printf(LOG_LEVEL_WARN,
                           "elf_apply_relocations: R_GLOB_DAT poke failed at %p\n",
                           (void *)(uintptr_t)target_va);
                ++errors;
            }
            break;

        case R_X86_64_JUMP_SLOT:
            /* *loc = sym_val (PLT slots ignore addend) */
            if (elf_poke64(pml4, target_va, sym_val) != 0) {
                log_printf(LOG_LEVEL_WARN,
                           "elf_apply_relocations: R_JUMP_SLOT poke failed at %p\n",
                           (void *)(uintptr_t)target_va);
                ++errors;
            }
            break;

        case R_X86_64_IRELATIVE: {
            /* *loc = ((uint64_t(*)()) (base + addend))() */
            uint64_t resolver_addr = base + (uint64_t)r_addend;
            /* The resolver function is already loaded in memory.
             * We call it via the page table to get the resolved value. */
            uint64_t resolver_phys = elf_resolve_va(pml4, resolver_addr);
            if (!resolver_phys) {
                log_printf(LOG_LEVEL_WARN,
                           "elf_apply_relocations: R_IRELATIVE resolver not mapped at %p\n",
                           (void *)(uintptr_t)resolver_addr);
                ++errors;
                break;
            }
            uint64_t (*resolver)(void) = (uint64_t (*)(void))(uintptr_t)resolver_phys;
            uint64_t resolved = resolver();
            if (elf_poke64(pml4, target_va, resolved) != 0) {
                log_printf(LOG_LEVEL_WARN,
                           "elf_apply_relocations: R_IRELATIVE poke failed at %p\n",
                           (void *)(uintptr_t)target_va);
                ++errors;
            }
            break;
        }

        case R_X86_64_32:
        case R_X86_64_32S:
            /* These are uncommon for x86_64 PIE; skip with a warning */
            log_printf(LOG_LEVEL_WARN,
                       "elf_apply_relocations: unsupported reloc type %u at %p\n",
                       r_type, (void *)(uintptr_t)target_va);
            break;

        default:
            /* Unknown relocation type — non-fatal for now */
            break;
        }
    }

    if (symtab) kfree(symtab);
    kfree(rela);

    if (errors > 0) {
        log_printf(LOG_LEVEL_WARN, "elf_apply_relocations: %d errors\n", errors);
    } else {
        log_printf(LOG_LEVEL_INFO, "elf_apply_relocations: %zu relocations applied\n",
                   num_rela);
    }
    return 0;
}

/*
 * elf_setup_user_stack: Create a user-mode stack with argc, argv, envp,
 * and auxiliary vector. Allocates and maps physical pages, then writes
 * the initial stack layout.
 *
 * Returns the initial RSP value (user virtual address), or 0 on failure.
 */
static uint64_t elf_setup_user_stack(uint64_t pml4, int argc,
                                      char *const argv[],
                                      char *const envp[],
                                      uint64_t entry,
                                      uint64_t phdr_addr,
                                      uint16_t phnum,
                                      uint16_t phentsize) {
    /* Count envp entries */
    int envc = 0;
    if (envp) {
        while (envp[envc]) ++envc;
    }

    /* Calculate total string data size */
    size_t strings_size = 0;
    for (int i = 0; i < argc; ++i) {
        if (argv[i]) strings_size += strlen(argv[i]) + 1;
    }
    for (int i = 0; i < envc; ++i) {
        if (envp[i]) strings_size += strlen(envp[i]) + 1;
    }

    /* Calculate metadata size:
     *   auxv:  6 entries × 16 bytes = 96
     *   envp:  (envc + 1) × 8 bytes
     *   argv:  (argc + 1) × 8 bytes
     *   argc:  8 bytes
     */
    size_t metadata_size = 96 + (size_t)(envc + 1) * 8 +
                           (size_t)(argc + 1) * 8 + 8;
    size_t total_needed = strings_size + metadata_size + 16; /* +16 for alignment slack */

    if (total_needed > USER_STACK_SIZE) {
        log_printf(LOG_LEVEL_ERR,
                   "elf_setup_user_stack: data too large (%lu > %lu)\n",
                   (unsigned long)total_needed, (unsigned long)USER_STACK_SIZE);
        return 0;
    }

    /* Get a randomized stack top */
    uint64_t stack_top = aslr_randomize_stack();

    /* Allocate and map stack pages */
    uint64_t stack_bottom = stack_top - USER_STACK_SIZE;
    uint64_t va;
    void *stack_pages[USER_STACK_PAGES];
    int num_stack_pages = 0;
    for (va = stack_bottom; va < stack_top; va += PAGE_SIZE) {
        void *phys = alloc_page();
        if (!phys) {
            log_printf(LOG_LEVEL_ERR, "elf_setup_user_stack: out of memory\n");
            goto free_stack;
        }
        stack_pages[num_stack_pages++] = phys;
        memset(phys, 0, PAGE_SIZE);
        if (map_page(pml4, va, (uint64_t)(uintptr_t)phys,
                     PTE_USER | PTE_RW | PTE_NX) != 0) {
            log_printf(LOG_LEVEL_ERR, "elf_setup_user_stack: map_page failed\n");
            goto free_stack;
        }
    }

    /*
     * Stack layout (high to low):
     *   [envp strings] [argv strings]
     *   [alignment padding to 16 bytes]
     *   [Elf64_auxv_t AT_NULL]
     *   [Elf64_auxv_t entries ...]
     *   [NULL envp terminator]
     *   [envp pointers ...]
     *   [NULL argv terminator]
     *   [argv pointers ...]
     *   [argc (8 bytes)]
     *   <- RSP
     */

    /* Write position: start at stack_top - 1 and go down */
    uint64_t write_pos = stack_top;

    /* Helper: write bytes to the stack at a given offset from stack_top.
     * offset is how far below stack_top to write. */
    /* We'll write sequentially from high to low using phys_to_virt. */

    /* --- Step 1: Copy envp and argv strings to the top of the stack --- */
    /* We'll collect string pointers as we go */
    uint64_t *str_ptrs = (uint64_t *)kmalloc(
        (size_t)(argc + envc + 1) * sizeof(uint64_t));
    if (!str_ptrs) {
        log_printf(LOG_LEVEL_ERR, "elf_setup_user_stack: kmalloc failed\n");
        goto free_stack;
    }

    int str_idx = 0;
    /* Copy envp strings first (they end up at higher addresses) */
    for (int i = 0; i < envc; ++i) {
        if (!envp[i]) continue;
        size_t len = strlen(envp[i]) + 1;
        write_pos -= len;
        uint64_t phys = elf_resolve_va(pml4, write_pos);
        if (!phys) { goto free_stack_and_data; }
        memcpy((void *)(uintptr_t)phys, envp[i], len);
        str_ptrs[str_idx++] = write_pos;
    }
    int envp_start_idx = 0;       /* first envp string in str_ptrs */
    int argv_start_idx = str_idx; /* first argv string in str_ptrs */

    /* Copy argv strings */
    for (int i = 0; i < argc; ++i) {
        if (!argv[i]) continue;
        size_t len = strlen(argv[i]) + 1;
        write_pos -= len;
        uint64_t phys = elf_resolve_va(pml4, write_pos);
        if (!phys) { goto free_stack_and_data; }
        memcpy((void *)(uintptr_t)phys, argv[i], len);
        str_ptrs[str_idx++] = write_pos;
    }

    /* --- Step 2: Align to 16 bytes --- */
    write_pos &= ~0xFULL;

    /* --- Step 3: Write auxv entries --- */
    write_pos -= 16;
    if (elf_poke64(pml4, write_pos, AT_NULL) != 0 ||
        elf_poke64(pml4, write_pos + 8, 0) != 0) {
        goto free_stack_and_data;
    }

    write_pos -= 16;
    if (elf_poke64(pml4, write_pos, AT_PAGESZ) != 0 ||
        elf_poke64(pml4, write_pos + 8, PAGE_SIZE) != 0) {
        goto free_stack_and_data;
    }

    write_pos -= 16;
    if (elf_poke64(pml4, write_pos, AT_ENTRY) != 0 ||
        elf_poke64(pml4, write_pos + 8, entry) != 0) {
        goto free_stack_and_data;
    }

    write_pos -= 16;
    if (elf_poke64(pml4, write_pos, AT_PHNUM) != 0 ||
        elf_poke64(pml4, write_pos + 8, phnum) != 0) {
        goto free_stack_and_data;
    }

    write_pos -= 16;
    if (elf_poke64(pml4, write_pos, AT_PHENT) != 0 ||
        elf_poke64(pml4, write_pos + 8, phentsize) != 0) {
        goto free_stack_and_data;
    }

    write_pos -= 16;
    if (elf_poke64(pml4, write_pos, AT_PHDR) != 0 ||
        elf_poke64(pml4, write_pos + 8, phdr_addr) != 0) {
        goto free_stack_and_data;
    }

    /* --- Step 4: Write envp pointer array (NULL-terminated) --- */
    write_pos -= 8;
    if (elf_poke64(pml4, write_pos, 0) != 0) { goto free_stack_and_data; }
    for (int i = envc - 1; i >= 0; --i) {
        write_pos -= 8;
        if (elf_poke64(pml4, write_pos, str_ptrs[envp_start_idx + i]) != 0) {
            goto free_stack_and_data;
        }
    }

    /* --- Step 5: Write argv pointer array (NULL-terminated) --- */
    write_pos -= 8;
    if (elf_poke64(pml4, write_pos, 0) != 0) { goto free_stack_and_data; }
    for (int i = argc - 1; i >= 0; --i) {
        write_pos -= 8;
        if (elf_poke64(pml4, write_pos, str_ptrs[argv_start_idx + i]) != 0) {
            goto free_stack_and_data;
        }
    }

    /* --- Step 6: Write argc --- */
    write_pos -= 8;
    if (elf_poke64(pml4, write_pos, (uint64_t)argc) != 0) {
        goto free_stack_and_data;
    }

    /* --- Step 7: Ensure 16-byte stack alignment --- */
    if (write_pos & 0xF) {
        write_pos -= 8;
        if (elf_poke64(pml4, write_pos, 0) != 0) {
            goto free_stack_and_data;
        }
    }

    kfree(str_ptrs);

    log_printf(LOG_LEVEL_INFO,
               "elf_setup_user_stack: argc=%d envc=%d rsp=%p stack_top=%p\n",
               argc, envc, (void *)(uintptr_t)write_pos,
               (void *)(uintptr_t)stack_top);
    return write_pos;

free_stack_and_data:
    kfree(str_ptrs);
free_stack:
    /* Unmap and free all stack pages allocated so far */
    for (uint64_t v = stack_bottom, idx = 0; idx < (uint64_t)num_stack_pages; v += PAGE_SIZE, idx++) {
        unmap_page(pml4, v);
        free_page(stack_pages[idx]);
    }
    return 0;
}

/*
 * elf_load_core: Core ELF loading logic shared by elf_load and elf_load_pie.
 *
 * Loads an ELF (ET_EXEC or ET_DYN) into a new page table, applies
 * relocations for PIE (ET_DYN), and optionally sets up a user stack.
 *
 * @path:       path to the ELF file in VFS
 * @pml4_out:   output — physical address of the new PML4 table
 * @stack_out:  output — user stack pointer (RSP), or NULL if no stack setup
 * @argv:       argv array for stack setup (NULL for no argv)
 * @envp:       envp array for stack setup (NULL for no envp)
 * Returns:     user virtual entry point, or NULL on failure.
 */
static void *elf_load_core(const char *path, uint64_t *pml4_out,
                            uint64_t *stack_out,
                            char *const argv[], char *const envp[]) {
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

    /* Determine ELF type and base address */
    int is_pie = (ehdr.e_type == ET_DYN);
    uint64_t base = 0;
    if (is_pie) {
        base = aslr_randomize_mmap();
        if (base == 0) base = PIE_DEFAULT_BASE;
        log_printf(LOG_LEVEL_INFO, "elf_load: PIE base=%p\n", (void *)(uintptr_t)base);
    } else if (ehdr.e_type != ET_EXEC) {
        vfs_close(f);
        log_printf(LOG_LEVEL_ERR, "elf_load: unsupported ELF type %u\n", ehdr.e_type);
        return NULL;
    }

    /* Read all program headers into an array */
    int phnum = (int)ehdr.e_phnum;
    Elf64_Phdr *phdrs = (Elf64_Phdr *)kmalloc((size_t)phnum * sizeof(Elf64_Phdr));
    if (!phdrs) { vfs_close(f); log_printf(LOG_LEVEL_ERR, "elf_load: kmalloc phdrs failed\n"); return NULL; }
    f->offset = ehdr.e_phoff;
    r = vfs_read(f, phdrs, (size_t)phnum * sizeof(Elf64_Phdr));
    if (r != (ssize_t)(phnum * sizeof(Elf64_Phdr))) {
        kfree(phdrs); vfs_close(f);
        log_printf(LOG_LEVEL_ERR, "elf_load: read phdrs failed\n");
        return NULL;
    }

    /* first pass: find loadable segment bounds */
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    for (int i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t seg_start = base + phdrs[i].p_vaddr;
        uint64_t seg_end   = seg_start + phdrs[i].p_memsz;
        if (seg_start < min_vaddr) min_vaddr = seg_start;
        if (seg_end > max_vaddr) max_vaddr = seg_end;
    }
    if (min_vaddr == UINT64_MAX) {
        kfree(phdrs); vfs_close(f);
        log_printf(LOG_LEVEL_ERR, "elf_load: no loadable segments\n");
        return NULL;
    }

    /* create a new PML4 for this program (kernel-only, fresh user space) */
    uint64_t new_pml4 = clone_kernel_pml4();
    if (!new_pml4 || new_pml4 == get_kernel_cr3()) {
        kfree(phdrs); vfs_close(f);
        log_printf(LOG_LEVEL_ERR, "elf_load: failed to clone kernel pml4\n");
        return NULL;
    }

    /* second pass: load segments page-by-page and map into new_pml4 */
    for (int i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t seg_vstart = base + phdrs[i].p_vaddr;
        uint64_t seg_filesz = phdrs[i].p_filesz;
        uint64_t seg_memsz  = phdrs[i].p_memsz;
        uint64_t file_offset = phdrs[i].p_offset;

        /* Security: Validate segment bounds are within user address space */
        if (seg_vstart > USER_ADDR_MAX || seg_vstart + seg_memsz < seg_vstart ||
            seg_vstart + seg_memsz > USER_ADDR_MAX) {
            kfree(phdrs); vfs_close(f);
            free_pagetable(new_pml4);
            log_printf(LOG_LEVEL_ERR, "elf_load: segment %d vaddr=%p out of bounds\n",
                       i, (void *)(uintptr_t)seg_vstart);
            return NULL;
        }

        uint64_t page_base   = seg_vstart & ~0xFFFULL;
        uint64_t page_offset = seg_vstart & 0xFFFULL;
        uint64_t to_alloc = (page_offset + seg_memsz + 4095) & ~0xFFFULL;
        uint64_t pages = to_alloc / 4096;

        /* permissions: user pages with NX for non-executable segments */
        uint64_t flags = PTE_USER;
        if (phdrs[i].p_flags & PF_W) flags |= PTE_RW;
        if (!(phdrs[i].p_flags & 1)) flags |= PTE_NX; /* PF_X=0 → set NX */

        for (uint64_t p = 0; p < pages; ++p) {
            void *phys = alloc_page();
            if (!phys) {
                kfree(phdrs); vfs_close(f);
                free_pagetable(new_pml4);
                log_printf(LOG_LEVEL_ERR, "elf_load: out of phys pages\n");
                return NULL;
            }
            memset(phys, 0, 4096);
            uint64_t va = page_base + p * 4096;
            if (map_page(new_pml4, va, (uint64_t)(uintptr_t)phys, flags) != 0) {
                kfree(phdrs); vfs_close(f);
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
            uint64_t *pml4 = phys_to_virt(new_pml4);
            uint64_t pml4_idx = (page_vbase >> 39) & 0x1FF;
            if (!(pml4[pml4_idx] & PTE_PRESENT)) break;
            uint64_t pdpt_phys = pml4[pml4_idx] & PTE_ADDR_MASK;
            uint64_t *pdpt = phys_to_virt(pdpt_phys);
            uint64_t pdpt_idx = (page_vbase >> 30) & 0x1FF;
            if (!(pdpt[pdpt_idx] & PTE_PRESENT)) break;
            uint64_t pd_phys = pdpt[pdpt_idx] & PTE_ADDR_MASK;
            uint64_t *pd = phys_to_virt(pd_phys);
            uint64_t pd_idx = (page_vbase >> 21) & 0x1FF;
            if (!(pd[pd_idx] & PTE_PRESENT)) break;
            uint64_t pt_phys = pd[pd_idx] & PTE_ADDR_MASK;
            uint64_t *pt = phys_to_virt(pt_phys);
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

    /* Apply relocations for PIE executables */
    if (is_pie) {
        struct elf_dyn_info dyn;
        if (elf_load_dynamic(f, phdrs, phnum, base, &dyn) == 0) {
            elf_apply_relocations(new_pml4, base, &dyn, phdrs, phnum, f);
        }
    }

    uint64_t entry = base + ehdr.e_entry;

    /* Security: Validate entry point is within mapped user space */
    if (entry < min_vaddr || entry >= max_vaddr) {
        kfree(phdrs); vfs_close(f);
        free_pagetable(new_pml4);
        log_printf(LOG_LEVEL_ERR, "elf_load: entry point %p outside mapped range [%p, %p)\n",
                   (void *)(uintptr_t)entry, (void *)min_vaddr, (void *)max_vaddr);
        return NULL;
    }

    /* Compute AT_PHDR value: virtual address of the program header table.
     * Find the PT_LOAD segment that contains the PHDR file offset. */
    uint64_t at_phdr = 0;
    {
        uint64_t phdr_start = ehdr.e_phoff;
        uint64_t phdr_end   = phdr_start + (uint64_t)phnum * ehdr.e_phentsize;
        for (int i = 0; i < phnum; ++i) {
            if (phdrs[i].p_type != PT_LOAD) continue;
            uint64_t seg_file_start = phdrs[i].p_offset;
            uint64_t seg_file_end   = seg_file_start + phdrs[i].p_filesz;
            if (phdr_start >= seg_file_start && phdr_end <= seg_file_end) {
                at_phdr = base + phdrs[i].p_vaddr +
                          (phdr_start - seg_file_start);
                break;
            }
        }
        /* Fallback: if not found in a segment, use the file offset as-is */
        if (at_phdr == 0) {
            at_phdr = base + ehdr.e_phoff;
        }
    }

    /* Set up user stack */
    if (stack_out) {
        int argc = 0;
        if (argv) { while (argv[argc]) ++argc; }
        uint64_t rsp = elf_setup_user_stack(new_pml4, argc, argv, envp,
                                             entry, at_phdr,
                                             ehdr.e_phnum, ehdr.e_phentsize);
        if (rsp == 0) {
            kfree(phdrs); vfs_close(f);
            free_pagetable(new_pml4);
            log_printf(LOG_LEVEL_ERR, "elf_load: stack setup failed\n");
            return NULL;
        }
        *stack_out = rsp;
    }

    kfree(phdrs);
    *pml4_out = new_pml4;
    vfs_close(f);
    log_printf(LOG_LEVEL_INFO, "elf_load: loaded %s entry=%p pml4=%p base=%p\n",
               path, (void *)(uintptr_t)entry, (void *)(uintptr_t)new_pml4,
               (void *)(uintptr_t)base);
    return (void *)(uintptr_t)entry;
}

/*
 * elf_load: Load an ELF executable into a new page table.
 * @path:      path to the ELF file in VFS
 * @pml4_out:  output — physical address of the new PML4 table
 * Returns:    user virtual entry point, or NULL on failure.
 *
 * Supports both ET_EXEC (fixed-address) and ET_DYN (PIE) executables.
 * Sets up a basic user stack with auxv (no argv/envp).
 *
 * On failure, all allocated resources (pages, page tables) are freed,
 * so the caller does not need to call free_pagetable().
 */
void *elf_load(const char *path, uint64_t *pml4_out) {
    uint64_t stack_unused;
    return elf_load_core(path, pml4_out, &stack_unused, NULL, NULL);
}

/*
 * elf_load_pie: Load a PIE executable and set up a full user stack
 * with argv, envp, and auxiliary vector.
 *
 * @path:       path to the ELF file in VFS
 * @argv:       NULL-terminated argument vector (may be NULL)
 * @envp:       NULL-terminated environment vector (may be NULL)
 * @pml4_out:   output — physical address of the new PML4 table
 * @stack_out:  output — initial user stack pointer (RSP)
 * Returns:     user virtual entry point, or NULL on failure.
 */
void *elf_load_pie(const char *path, char *const argv[], char *const envp[],
                   uint64_t *pml4_out, uint64_t *stack_out) {
    if (!stack_out) return NULL;
    return elf_load_core(path, pml4_out, stack_out, argv, envp);
}

/* helper to create task from ELF path */
int exec_elf(const char *path) {
    uint64_t pml4 = 0;
    uint64_t stack = 0;
    void *entry = elf_load_pie(path, NULL, NULL, &pml4, &stack);
    if (!entry) return -1;
    int pid = create_user_task_from_entry((void(*)(void))entry, pml4, stack);
    return pid;
}