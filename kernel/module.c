/*
 * module.c - Kernel module loader
 *
 * Loads relocatable ELF object files (.ko) into the kernel address
 * space, resolves symbols against the kernel export table, applies
 * x86_64 relocations, and calls the module's init function.
 *
 * The kernel is identity-mapped, so physical addresses returned by
 * kmalloc() are directly usable as virtual addresses.
 */
#include "module.h"
#include "elf.h"
#include "vfs.h"
#include "mem.h"
#include "include/log.h"
#include "include/kstdio.h"
#include "include/print.h"
#include "include/version.h"
#include "console.h"
#include "fs.h"
#include <string.h>

/* ================================================================
 * Kernel symbol table (linked list)
 * ================================================================ */
static struct module_symbol *kernel_syms = NULL;

/* ================================================================
 * Module list (linked list)
 * ================================================================ */
static struct kernel_module *module_head = NULL;

/* ================================================================
 * Internal helpers
 * ================================================================ */

/* Round up to alignment */
static inline size_t align_up(size_t val, size_t align) {
    if (align == 0) return val;
    return (val + align - 1) & ~(align - 1);
}

/* Write a string to the console (thin wrapper) */
static void con_write(const char *s) {
    console_write(s);
}

/* Write an unsigned integer to the console */
static void con_write_uint(uint64_t val) {
    char buf[24];
    uitoa(val, buf, sizeof(buf));
    console_write(buf);
}

/* ================================================================
 * Symbol table management
 * ================================================================ */

int module_register_symbol(const char *name, void *addr) {
    if (!name || !addr) return -1;

    struct module_symbol *sym = (struct module_symbol *)kmalloc(sizeof(*sym));
    if (!sym) return -1;
    sym->name = name;
    sym->addr = addr;
    sym->next = kernel_syms;
    kernel_syms = sym;
    return 0;
}

void *module_lookup_symbol(const char *name) {
    if (!name) return NULL;

    /* Search kernel symbol table */
    for (struct module_symbol *s = kernel_syms; s; s = s->next) {
        if (strcmp(s->name, name) == 0) return s->addr;
    }

    /* Search module-local symbol tables */
    for (struct kernel_module *m = module_head; m; m = m->next) {
        if (m->state != MODULE_LIVE) continue;
        for (int i = 0; i < m->num_syms; i++) {
            if (strcmp(m->syms[i].name, name) == 0)
                return m->syms[i].addr;
        }
    }

    return NULL;
}

/* ================================================================
 * Module reference counting
 * ================================================================ */

void module_get(struct kernel_module *mod) {
    if (mod) mod->refcount++;
}

void module_put(struct kernel_module *mod) {
    if (mod && mod->refcount > 0) mod->refcount--;
}

/* ================================================================
 * Module list management
 * ================================================================ */

struct kernel_module *module_find(const char *name) {
    for (struct kernel_module *m = module_head; m; m = m->next) {
        if (strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

void module_list(void) {
    if (!module_head) {
        console_write_ansi("\x1b[90m");
        console_write("  No modules loaded.\n");
        console_write_ansi("\x1b[0m");
        return;
    }

    console_write_ansi("\x1b[90m");
    console_write("  Name                    State       Base        Size\n");
    console_write_ansi("\x1b[0m");

    for (struct kernel_module *m = module_head; m; m = m->next) {
        console_write("  ");
        console_write(m->name);
        /* pad name to 24 chars */
        int nlen = 0;
        for (const char *p = m->name; *p; p++) nlen++;
        for (int i = nlen; i < 24; i++) console_putc(' ');

        const char *state_str;
        const char *state_color;
        switch (m->state) {
            case MODULE_LIVE:      state_str = "LIVE";   state_color = "\x1b[32m"; break;
            case MODULE_LOADING:   state_str = "LOADING"; state_color = "\x1b[33m"; break;
            case MODULE_UNLOADING: state_str = "UNLOAD";  state_color = "\x1b[33m"; break;
            default:               state_str = "DEAD";    state_color = "\x1b[31m"; break;
        }
        console_write_ansi(state_color);
        console_write(state_str);
        console_write_ansi("\x1b[0m");
        console_write("  ");

        char hex[24];
        con_write("0x");
        uitoa_hex((uint64_t)(uintptr_t)m->base, hex, sizeof(hex));
        console_write(hex);
        console_write("  ");
        con_write_uint(m->size);
        console_putc('\n');
    }
}

/* ================================================================
 * Section layout helper
 * ================================================================ */

struct section_info {
    uint64_t addr;       /* assigned virtual address */
    uint64_t size;
    uint64_t file_offset;
    uint64_t align;
    uint32_t type;
    int      alloc;      /* SHF_ALLOC */
};

/* ================================================================
 * module_load: Load a relocatable ELF from VFS
 * ================================================================ */
int module_load(const char *path) {
    if (!path) return -1;

    struct file *f = vfs_open(path, 0);
    if (!f) {
        log_printf(LOG_LEVEL_ERR, "module_load: cannot open %s\n", path);
        return -1;
    }

    /* ================================================================
     * Module signature verification (FIXED v4.0.6)
     *
     * When MODULE_SIGN_CHECK is enabled, verify the module's signature
     * before any ELF parsing. This prevents loading of unsigned or
     * tampered modules.
     * ================================================================ */
    if (module_sign_is_enabled()) {
        size_t file_size = f->inode->size;
        if (file_size < MODULE_SIGN_HEADER_MIN_SIZE) {
            log_printf(LOG_LEVEL_ERR,
                "module_load: %s too small for signature (%zu bytes)\n",
                path, file_size);
            vfs_close(f);
            return -1;
        }

        uint8_t *mod_buf = (uint8_t *)kmalloc(file_size);
        if (!mod_buf) {
            log_printf(LOG_LEVEL_ERR, "module_load: OOM reading %s\n", path);
            vfs_close(f);
            return -1;
        }

        f->offset = 0;
        if (vfs_read(f, mod_buf, file_size) != (ssize_t)file_size) {
            log_printf(LOG_LEVEL_ERR, "module_load: read failed for %s\n", path);
            kfree(mod_buf);
            vfs_close(f);
            return -1;
        }

        int sign_result = module_sign_verify(mod_buf, file_size);
        kfree(mod_buf);

        if (sign_result != 0) {
            log_printf(LOG_LEVEL_ERR,
                "module_load: %s signature verification failed\n", path);
            vfs_close(f);
            return -1;
        }

        log_printf(LOG_LEVEL_INFO, "module_load: %s signature verified\n", path);
        /* Reset file offset for subsequent ELF parsing */
        f->offset = 0;
    }

    /* Read ELF header */
    Elf64_Ehdr ehdr;
    f->offset = 0;
    if (vfs_read(f, &ehdr, sizeof(ehdr)) != (ssize_t)sizeof(ehdr)) {
        log_printf(LOG_LEVEL_ERR, "module_load: read ehdr failed\n");
        vfs_close(f);
        return -1;
    }

    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        log_printf(LOG_LEVEL_ERR, "module_load: bad ELF magic\n");
        vfs_close(f);
        return -1;
    }

    if (ehdr.e_machine != 0x3E) {
        log_printf(LOG_LEVEL_ERR, "module_load: not x86-64\n");
        vfs_close(f);
        return -1;
    }

    /* Must be a relocatable object (ET_REL = 1) */
    if (ehdr.e_type != 1) {
        log_printf(LOG_LEVEL_ERR, "module_load: not a relocatable object (type=%d)\n", ehdr.e_type);
        vfs_close(f);
        return -1;
    }

    if (ehdr.e_shnum == 0 || ehdr.e_shoff == 0) {
        log_printf(LOG_LEVEL_ERR, "module_load: no section headers\n");
        vfs_close(f);
        return -1;
    }

    /* Read section headers */
    uint16_t shnum = ehdr.e_shnum;
    Elf64_Shdr *shdrs = (Elf64_Shdr *)kmalloc(shnum * sizeof(Elf64_Shdr));
    if (!shdrs) { vfs_close(f); return -1; }

    f->offset = ehdr.e_shoff;
    if (vfs_read(f, shdrs, shnum * sizeof(Elf64_Shdr)) !=
        (ssize_t)(shnum * sizeof(Elf64_Shdr))) {
        log_printf(LOG_LEVEL_ERR, "module_load: read shdrs failed\n");
        kfree(shdrs);
        vfs_close(f);
        return -1;
    }

    /* Read section header string table */

    /* Bug #8 fix: validate e_shstrndx bounds before using as array index.
     * A malicious ELF could set e_shstrndx to 0xFFFF, causing an out-of-bounds
     * read from shdrs[]. */
    if (ehdr.e_shstrndx >= shnum) {
        log_printf(LOG_LEVEL_ERR, "module_load: invalid e_shstrndx (%u >= %u)\n",
                   ehdr.e_shstrndx, shnum);
        kfree(shdrs);
        vfs_close(f);
        return -1;
    }

    Elf64_Shdr *shstrtab_hdr = &shdrs[ehdr.e_shstrndx];

    /* Bug #9 fix: validate sh_size and sh_offset against file size.
     * A malicious ELF can specify huge sizes causing kmalloc to fail or overflow,
     * or sh_offset + sh_size could exceed the file boundaries. */
    size_t shstrtab_size = shstrtab_hdr->sh_size;
    if (shstrtab_size > 65536) {  /* cap at 64KB, reasonable for a string table */
        log_printf(LOG_LEVEL_ERR, "module_load: shstrtab size too large (%zu)\n",
                   shstrtab_size);
        kfree(shdrs);
        vfs_close(f);
        return -1;
    }

    /* Validate that sh_offset + sh_size doesn't exceed file size, and
     * also check for integer overflow in the addition. */
    {
        size_t file_sz = f->inode->size;
        if (shstrtab_hdr->sh_offset + shstrtab_size > file_sz ||
            shstrtab_hdr->sh_offset + shstrtab_size < shstrtab_hdr->sh_offset) {
            log_printf(LOG_LEVEL_ERR, "module_load: shstrtab offset/size out of file bounds\n");
            kfree(shdrs);
            vfs_close(f);
            return -1;
        }
    }

    char *shstrtab = (char *)kmalloc(shstrtab_size);
    if (!shstrtab) { kfree(shdrs); vfs_close(f); return -1; }
    f->offset = shstrtab_hdr->sh_offset;
    if (vfs_read(f, shstrtab, shstrtab_size) != (ssize_t)shstrtab_size) {
        log_printf(LOG_LEVEL_WARN, "module_load: read shstrtab failed\n");
        /* Continue with empty string table - section names will be unavailable */
        memset(shstrtab, 0, shstrtab_size);
    }

    /* --- First pass: collect allocatable sections and compute total size --- */
    #define MAX_SECTIONS 64
    struct section_info secs[MAX_SECTIONS];
    int nsecs = 0;
    size_t total_size = 0;

    /* Also locate symtab and strtab for later */
    Elf64_Shdr *symtab_hdr = NULL;
    Elf64_Shdr *strtab_hdr = NULL;

    for (int i = 0; i < shnum && nsecs < MAX_SECTIONS; i++) {
        uint64_t sh_flags = shdrs[i].sh_flags;
        int is_alloc = (sh_flags & 0x2) != 0;  /* SHF_ALLOC */

        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab_hdr = &shdrs[i];
        }
        if (shdrs[i].sh_type == SHT_STRTAB && i != ehdr.e_shstrndx) {
            /* The first non-shstrtab STRTAB is the main string table */
            if (!strtab_hdr) strtab_hdr = &shdrs[i];
        }

        if (!is_alloc || shdrs[i].sh_size == 0) continue;

        uint64_t align = shdrs[i].sh_addralign;
        /* Bug #46: attacker-controlled sh_addralign can cause overflow in
         * alignment calculations. Validate it is a power of 2 and within
         * a reasonable range (<= 4096, the page size). */
        if (align == 0 || (align & (align - 1)) != 0 || align > 4096) {
            log_printf(LOG_LEVEL_ERR, "module_load: invalid sh_addralign %llu\n",
                       (unsigned long long)align);
            kfree(shstrtab);
            kfree(shdrs);
            vfs_close(f);
            return -1;
        }
        if (align < 16) align = 16;

        /* Bug #10 fix: check for integer overflow in align_up.
         * total_size could overflow and wrap to a small value, causing
         * kmalloc to allocate a tiny buffer for a large module. */
        size_t old_total = total_size;
        total_size = align_up(total_size, align);
        if (total_size < old_total) {
            /* Integer overflow detected */
            log_printf(LOG_LEVEL_ERR, "module_load: total_size overflow\n");
            kfree(shstrtab);
            kfree(shdrs);
            vfs_close(f);
            return -1;
        }

        secs[nsecs].addr        = 0;  /* will be assigned */
        secs[nsecs].size        = shdrs[i].sh_size;
        secs[nsecs].file_offset = shdrs[i].sh_offset;
        secs[nsecs].align       = align;
        secs[nsecs].type        = shdrs[i].sh_type;
        secs[nsecs].alloc       = 1;
        nsecs++;
    }

    if (nsecs == 0) {
        log_printf(LOG_LEVEL_ERR, "module_load: no allocatable sections\n");
        kfree(shstrtab); kfree(shdrs); vfs_close(f);
        return -1;
    }

    /* Allocate module memory */
    void *mod_base = kmalloc(total_size);
    if (!mod_base) {
        log_printf(LOG_LEVEL_ERR, "module_load: kmalloc failed\n");
        kfree(shstrtab); kfree(shdrs); vfs_close(f);
        return -1;
    }
    memset(mod_base, 0, total_size);

    /* --- Second pass: assign addresses and copy section data --- */
    size_t current_offset = 0;
    for (int i = 0; i < nsecs; i++) {
        current_offset = align_up(current_offset, secs[i].align);
        secs[i].addr = (uint64_t)(uintptr_t)mod_base + current_offset;

        if (secs[i].type == SHT_NOBITS) {
            /* BSS: already zeroed by memset */
        } else {
            /* Copy from file */
            f->offset = secs[i].file_offset;
            ssize_t rd = vfs_read(f, (void *)(uintptr_t)secs[i].addr, secs[i].size);
            if (rd != (ssize_t)secs[i].size) {
                log_printf(LOG_LEVEL_WARN, "module_load: read section %d failed (got %d/%llu)\n",
                           i, (int)rd, (unsigned long long)secs[i].size);
            }
        }
        current_offset += secs[i].size;
    }

    /* --- Resolve symbols and apply relocations --- */
    char *strtab_data = NULL;
    if (strtab_hdr && strtab_hdr->sh_size > 0) {
        /* Bug #11 fix: validate strtab size before kmalloc.
         * An attacker-controlled ELF could specify a huge sh_size causing
         * kmalloc to fail or wrap. Cap at a reasonable maximum (1MB). */
        if (strtab_hdr->sh_size > 1048576) {  /* 1MB cap */
            log_printf(LOG_LEVEL_ERR, "module_load: strtab size too large (%llu)\n",
                       (unsigned long long)strtab_hdr->sh_size);
            kfree(shstrtab);
            kfree(shdrs);
            kfree(mod_base);
            vfs_close(f);
            return -1;
        }
        strtab_data = (char *)kmalloc(strtab_hdr->sh_size);
        if (strtab_data) {
            f->offset = strtab_hdr->sh_offset;
            if (vfs_read(f, strtab_data, strtab_hdr->sh_size) != (ssize_t)strtab_hdr->sh_size) {
                log_printf(LOG_LEVEL_WARN, "module_load: read strtab failed\n");
                memset(strtab_data, 0, strtab_hdr->sh_size);
            }
        }
    }

    Elf64_Sym *symtab_data = NULL;
    uint32_t symtab_count = 0;
    if (symtab_hdr && symtab_hdr->sh_size >= sizeof(Elf64_Sym)) {
        symtab_data = (Elf64_Sym *)kmalloc(symtab_hdr->sh_size);
        if (symtab_data) {
            f->offset = symtab_hdr->sh_offset;
            if (vfs_read(f, symtab_data, symtab_hdr->sh_size) != (ssize_t)symtab_hdr->sh_size) {
                log_printf(LOG_LEVEL_WARN, "module_load: read symtab failed\n");
                memset(symtab_data, 0, symtab_hdr->sh_size);
            }
            symtab_count = symtab_hdr->sh_size / sizeof(Elf64_Sym);
        }
    }

    /* Process all RELA sections */
    int relocate_errors = 0;
    for (int i = 0; i < shnum; i++) {
        if (shdrs[i].sh_type != SHT_RELA) continue;
        if (shdrs[i].sh_size == 0) continue;

        /* The section being relocated */
        uint32_t target_sec = shdrs[i].sh_info;
        uint64_t target_base = 0;

        /* Compute the base address of the target section.
         * Find which sec_info entry corresponds to this section index
         * by matching size and file_offset. */
        /* Bug #12 fix: validate target_sec is a valid section index and
         * that the target section type is one we can relocate (SHT_PROGBITS
         * or SHT_NOBITS). sh_info is attacker-controlled in a malicious ELF. */
        if (target_sec < shnum &&
            (shdrs[target_sec].sh_flags & 0x2) &&
            (shdrs[target_sec].sh_type == SHT_PROGBITS ||
             shdrs[target_sec].sh_type == SHT_NOBITS)) {
            uint64_t tgt_size = shdrs[target_sec].sh_size;
            uint64_t tgt_off  = shdrs[target_sec].sh_offset;
            for (int j = 0; j < nsecs; j++) {
                if (secs[j].size == tgt_size && secs[j].file_offset == tgt_off) {
                    target_base = secs[j].addr;
                    break;
                }
            }
        }

        /* Skip relocation if target section not found (avoid wild writes to address 0) */
        if (target_base == 0) {
            log_printf(LOG_LEVEL_WARN, "module_load: RELA target section %u not found, skipping\n", target_sec);
            continue;
        }

        /* Read RELA entries */
        uint64_t rela_count = shdrs[i].sh_size / sizeof(Elf64_Rela);
        /* Cap RELA section size to 1MB to prevent excessive allocation */
        if (shdrs[i].sh_size > (1ULL << 20)) {
            log_printf(LOG_LEVEL_WARN, "module_load: RELA section %d too large (%llu bytes), skipping\n",
                       i, (unsigned long long)shdrs[i].sh_size);
            continue;
        }
        Elf64_Rela *relas = (Elf64_Rela *)kmalloc(shdrs[i].sh_size);
        if (!relas) continue;
        f->offset = shdrs[i].sh_offset;
        if (vfs_read(f, relas, shdrs[i].sh_size) != (ssize_t)shdrs[i].sh_size) {
            log_printf(LOG_LEVEL_WARN, "module_load: read RELA section %d failed\n", i);
            kfree(relas);
            continue;
        }

        for (uint64_t r = 0; r < rela_count; r++) {
            uint64_t r_offset = relas[r].r_offset;
            uint64_t r_info   = relas[r].r_info;
            int64_t  r_addend = relas[r].r_addend;
            uint32_t r_type   = ELF64_R_TYPE(r_info);
            uint32_t r_sym    = ELF64_R_SYM(r_info);

            /* Validate r_offset is within the module's allocated memory */
            if (r_offset >= total_size) {
                log_printf(LOG_LEVEL_WARN, "module_load: r_offset %llu out of bounds (module size %zu)\n",
                           (unsigned long long)r_offset, total_size);
                relocate_errors++;
                continue;
            }

            uint64_t *patch_addr = (uint64_t *)(uintptr_t)(target_base + r_offset);
            uint64_t S = 0;  /* symbol value */
            const char *sym_name = NULL;

            if (r_sym > 0 && r_sym < symtab_count && symtab_data && strtab_data) {
                if (symtab_data[r_sym].st_name >= strtab_hdr->sh_size) {
                    log_printf(LOG_LEVEL_WARN, "module_load: symbol st_name out of bounds\n");
                    relocate_errors++;
                    continue;
                }
                sym_name = strtab_data + symtab_data[r_sym].st_name;

                if (ELF64_ST_BIND(symtab_data[r_sym].st_info) == STB_GLOBAL ||
                    ELF64_ST_BIND(symtab_data[r_sym].st_info) == STB_WEAK) {
                    /* Look up symbol in kernel + module symbol tables */
                    void *sym_addr = module_lookup_symbol(sym_name);
                    if (sym_addr) {
                        S = (uint64_t)(uintptr_t)sym_addr;
                    } else if (ELF64_ST_BIND(symtab_data[r_sym].st_info) == STB_WEAK) {
                        S = 0;  /* weak undefined → 0 */
                    } else {
                        log_printf(LOG_LEVEL_WARN, "module_load: undefined symbol '%s'\n", sym_name);
                        relocate_errors++;
                    }
                }
            }

            switch (r_type) {
                case R_X86_64_64:
                    /* S + A */
                    *patch_addr = S + (uint64_t)r_addend;
                    break;

                case R_X86_64_PC32: {
                    /* S + A - P (32-bit PC-relative) */
                    uint64_t P = target_base + r_offset;
                    int64_t val = (int64_t)(S + (uint64_t)r_addend - P);
                    int32_t *p32 = (int32_t *)(uintptr_t)patch_addr;
                    *p32 = (int32_t)val;
                    break;
                }

                case R_X86_64_32:
                    /* S + A (32-bit) */
                    *(uint32_t *)(uintptr_t)patch_addr = (uint32_t)(S + (uint64_t)r_addend);
                    break;

                case R_X86_64_32S: {
                    /* S + A (32-bit sign-extended) */
                    int32_t *p32s = (int32_t *)(uintptr_t)patch_addr;
                    *p32s = (int32_t)(S + (uint64_t)r_addend);
                    break;
                }

                case R_X86_64_RELATIVE:
                    /* B + A (base address of the module + addend) */
                    *patch_addr = (uint64_t)(uintptr_t)mod_base + (uint64_t)r_addend;
                    break;

                default:
                    log_printf(LOG_LEVEL_WARN, "module_load: unknown relocation type %d\n", r_type);
                    relocate_errors++;
                    break;
            }
        }
        kfree(relas);
    }

    /* --- Look up module's init/exit symbols BEFORE freeing symtab/strtab ---
     * BUGFIX: The original code freed symtab_data and strtab_data first,
     * then tried to look up init/exit symbols from the freed memory (UAF).
     * The lookup must happen while the data is still valid. */
    void (*module_init_fn)(void) = NULL;
    void (*module_exit_fn)(void) = NULL;
    if (symtab_data && strtab_data && symtab_count > 0) {
        for (uint32_t s = 0; s < symtab_count; s++) {
            if (ELF64_ST_BIND(symtab_data[s].st_info) != STB_GLOBAL) continue;
            const char *sn = strtab_data + symtab_data[s].st_name;

            /* Find the section this symbol is in */
            uint16_t sndx = symtab_data[s].st_shndx;
            if (sndx < shnum) {
                /* Find the corresponding section in our allocated layout */
                uint64_t sec_addr = 0;
                for (int j = 0; j < nsecs; j++) {
                    if (secs[j].file_offset == shdrs[sndx].sh_offset &&
                        secs[j].size == shdrs[sndx].sh_size) {
                        sec_addr = secs[j].addr;
                        break;
                    }
                }
                uint64_t sym_addr = sec_addr + symtab_data[s].st_value;

                if (strcmp(sn, "init") == 0) {
                    module_init_fn = (void (*)(void))(uintptr_t)sym_addr;
                } else if (strcmp(sn, "exit") == 0) {
                    module_exit_fn = (void (*)(void))(uintptr_t)sym_addr;
                }
            }
        }
    }

    if (strtab_data) kfree(strtab_data);
    if (symtab_data) kfree(symtab_data);
    kfree(shstrtab);
    kfree(shdrs);
    vfs_close(f);

    if (relocate_errors > 0) {
        log_printf(LOG_LEVEL_ERR, "module_load: %d relocation errors\n", relocate_errors);
        kfree(mod_base);
        return -1;
    }

    /* --- Allocate module descriptor --- */
    struct kernel_module *mod = (struct kernel_module *)kmalloc(sizeof(*mod));
    if (!mod) { kfree(mod_base); return -1; }
    memset(mod, 0, sizeof(*mod));

    /* Extract module name from path (last component, strip extension) */
    {
        const char *name_start = path;
        for (const char *p = path; *p; p++) {
            if (*p == '/') name_start = p + 1;
        }
        size_t ni = 0;
        for (const char *p = name_start; *p && *p != '.' && ni < 63; p++, ni++)
            mod->name[ni] = *p;
        mod->name[ni] = '\0';
    }

    mod->base  = mod_base;
    mod->size  = total_size;
    mod->state = MODULE_LOADING;
    mod->deps  = NULL;
    mod->num_deps = 0;
    mod->syms  = NULL;
    mod->num_syms = 0;

    /* Insert into module list */
    mod->init = module_init_fn;
    mod->exit = module_exit_fn;
    mod->next = module_head;
    module_head = mod;

    /* Call the init function */
    if (mod->init) {
        mod->init();
    }

    mod->state = MODULE_LIVE;

    log_printf(LOG_LEVEL_INFO, "module_load: %s loaded, size %zu\n",
               mod->name, mod->size);
    return 0;
}

/* ================================================================
 * module_unload: Unload a kernel module
 * ================================================================ */
int module_unload(const char *name) {
    if (!name) return -1;

    struct kernel_module *prev = NULL;
    struct kernel_module *mod = module_head;
    while (mod) {
        if (strcmp(mod->name, name) == 0) break;
        prev = mod;
        mod = mod->next;
    }

    if (!mod) {
        log_printf(LOG_LEVEL_WARN, "module_unload: %s not found\n", name);
        return -1;
    }

    if (mod->state != MODULE_LIVE) {
        log_printf(LOG_LEVEL_WARN, "module_unload: %s not live\n", name);
        return -1;
    }

    /* Check if any other module depends on this one */
    for (struct kernel_module *m = module_head; m; m = m->next) {
        if (m == mod || m->state != MODULE_LIVE) continue;
        for (int i = 0; i < m->num_deps; i++) {
            if (m->deps[i] == mod) {
                log_printf(LOG_LEVEL_WARN, "module_unload: %s is used by %s\n", name, m->name);
                return -1;
            }
        }
    }

    /* Check reference count */
    if (mod->refcount > 0) {
        log_printf(LOG_LEVEL_WARN, "module_unload: %s has refcount %d\n", name, mod->refcount);
        return -1;
    }

    mod->state = MODULE_UNLOADING;

    /* Call exit function */
    if (mod->exit) {
        mod->exit();
    }

    /* Free module-local symbol table */
    if (mod->syms) {
        kfree(mod->syms);
    }

    /* Free dependency array */
    if (mod->deps) {
        kfree(mod->deps);
    }

    /* Free dependency names */
    if (mod->dep_names) {
        for (int i = 0; i < mod->num_deps; i++) {
            if (mod->dep_names[i]) kfree(mod->dep_names[i]);
        }
        kfree(mod->dep_names);
    }

    /*
     * Clean up dependencies: for each module that depends on this module,
     * remove this module from its dependency list.
     */
    for (struct kernel_module *m = module_head; m; m = m->next) {
        if (m == mod || m->state != MODULE_LIVE) continue;
        for (int i = 0; i < m->num_deps; i++) {
            if (m->deps[i] == mod) {
                /* Shift remaining deps */
                for (int j = i; j < m->num_deps - 1; j++) {
                    m->deps[j] = m->deps[j + 1];
                }
                m->num_deps--;
                /* Free the dep name if it exists */
                if (m->dep_names && m->dep_names[i]) {
                    kfree(m->dep_names[i]);
                    for (int j = i; j < m->num_deps; j++) {
                        m->dep_names[j] = m->dep_names[j + 1];
                    }
                }
                break;
            }
        }
    }

    /* Free module memory */
    if (mod->base) {
        kfree(mod->base);
    }

    /* Remove from linked list */
    if (prev) {
        prev->next = mod->next;
    } else {
        module_head = mod->next;
    }

    kfree(mod);

    log_printf(LOG_LEVEL_INFO, "module_unload: %s unloaded\n", name);
    return 0;
}

/* ================================================================
 * module_init: Register core kernel symbols
 * ================================================================ */
void module_init(void) {
    module_register_symbol("kmalloc",        (void *)kmalloc);
    module_register_symbol("kfree",          (void *)kfree);
    module_register_symbol("alloc_page",     (void *)alloc_page);
    module_register_symbol("free_page",      (void *)free_page);
    module_register_symbol("alloc_pages",    (void *)alloc_pages);
    module_register_symbol("free_pages",     (void *)free_pages);
    module_register_symbol("log_printf",     (void *)log_printf);
    module_register_symbol("console_write",  (void *)console_write);
    module_register_symbol("console_putc",   (void *)console_putc);
    module_register_symbol("vfs_open",       (void *)vfs_open);
    module_register_symbol("vfs_read",       (void *)vfs_read);
    module_register_symbol("vfs_write",      (void *)vfs_write);
    module_register_symbol("vfs_close",      (void *)vfs_close);
    module_register_symbol("vfs_lookup",     (void *)vfs_lookup);
    module_register_symbol("memset",         (void *)memset);
    module_register_symbol("memcpy",         (void *)memcpy);
    module_register_symbol("strcmp",         (void *)strcmp);
    module_register_symbol("strlen",         (void *)strlen);
    module_register_symbol("printk",         (void *)printk);
    module_register_symbol("ramfs_add_file", (void *)ramfs_add_file);

    {
        int sym_count = 0;
        for (struct module_symbol *s = kernel_syms; s; s = s->next) sym_count++;
        log_printf(LOG_LEVEL_INFO, "module: subsystem initialized, %d kernel symbols exported\n", sym_count);
    }
}

/* ================================================================
 * module_version_check: Check module version vs kernel version
 *
 * For .km modules, the module's version is embedded in the .modinfo
 * section. For .ko modules, version defaults to 0.0.0 and is always
 * considered compatible.
 *
 * A module is compatible if its major version matches the kernel's
 * major version. Minor/patch mismatches are warnings but not errors.
 * ================================================================ */
int module_version_check(struct kernel_module *mod) {
    if (!mod) return -1;

    /* Version 0.0.0 means unversioned (.ko format) — always compatible */
    if (mod->version.major == 0 && mod->version.minor == 0 &&
        mod->version.patch == 0) {
        return 0;
    }

    /* Major version must match */
    if (mod->version.major != AURORAOS_MAJOR) {
        log_printf(LOG_LEVEL_WARN,
            "module_version_check: %s version %d.%d.%d incompatible with kernel %d.%d.%d\n",
            mod->name,
            mod->version.major, mod->version.minor, mod->version.patch,
            AURORAOS_MAJOR, AURORAOS_MINOR, AURORAOS_PATCH);
        return -1;
    }

    /* Minor version warning */
    if (mod->version.minor > AURORAOS_MINOR) {
        log_printf(LOG_LEVEL_WARN,
            "module_version_check: %s minor version %d > kernel minor %d\n",
            mod->name, mod->version.minor, AURORAOS_MINOR);
    }

    return 0;
}

/* ================================================================
 * module_dep_check: Check module dependencies
 *
 * Verifies that all modules listed in dep_names are loaded and in
 * LIVE state. If any dependency is missing, returns -1.
 * ================================================================ */
int module_dep_check(struct kernel_module *mod) {
    if (!mod) return -1;

    for (int i = 0; i < mod->num_deps; i++) {
        if (!mod->deps[i] || mod->deps[i]->state != MODULE_LIVE) {
            const char *dep_name = (mod->dep_names && mod->dep_names[i])
                                   ? mod->dep_names[i] : "unknown";
            log_printf(LOG_LEVEL_WARN,
                "module_dep_check: %s depends on %s (not loaded)\n",
                mod->name, dep_name);
            return -1;
        }
    }

    return 0;
}

/* ================================================================
 * module_export_info: Export all loaded module metadata
 *
 * Returns a linked list of struct module_info, one per loaded module.
 * Caller must free with module_info_free().
 * ================================================================ */
struct module_info *module_export_info(void) {
    struct module_info *head = NULL;
    struct module_info *tail = NULL;

    for (struct kernel_module *m = module_head; m; m = m->next) {
        struct module_info *info = (struct module_info *)kmalloc(sizeof(*info));
        if (!info) continue;
        memset(info, 0, sizeof(*info));

        /* Copy name */
        for (int i = 0; i < 63 && m->name[i]; i++)
            info->name[i] = m->name[i];
        info->name[63] = '\0';

        /* Copy version */
        info->ver.major = m->version.major;
        info->ver.minor = m->version.minor;
        info->ver.patch = m->version.patch;

        /* Copy metadata */
        for (int i = 0; i < 63 && m->author[i]; i++)
            info->author[i] = m->author[i];
        info->author[63] = '\0';

        for (int i = 0; i < 127 && m->description[i]; i++)
            info->description[i] = m->description[i];
        info->description[127] = '\0';

        for (int i = 0; i < 31 && m->license[i]; i++)
            info->license[i] = m->license[i];
        info->license[31] = '\0';

        /* Copy state info */
        info->state    = m->state;
        info->refcount = m->refcount;
        info->num_deps = m->num_deps;
        info->base     = m->base;
        info->size     = m->size;
        info->next     = NULL;

        /* Append to list */
        if (!head) {
            head = tail = info;
        } else {
            tail->next = info;
            tail = info;
        }
    }

    return head;
}

/* ================================================================
 * module_info_free: Free the module info list
 * ================================================================ */
void module_info_free(struct module_info *list) {
    while (list) {
        struct module_info *next = list->next;
        kfree(list);
        list = next;
    }
}