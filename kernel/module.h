/*
 * module.h - Kernel module loader interface
 *
 * Provides runtime loading/unloading of kernel modules from ELF
 * relocatable object files.  Modules can call exported kernel
 * symbols and register their own symbols for use by other modules.
 */
#ifndef MODULE_H
#define MODULE_H

#include <stddef.h>
#include <stdint.h>

/* Module states */
enum {
    MODULE_UNLOADED  = 0,
    MODULE_LOADING   = 1,
    MODULE_LIVE      = 2,
    MODULE_UNLOADING = 3
};

/* Exported kernel symbol */
struct module_symbol {
    const char *name;
    void       *addr;
    struct module_symbol *next;
};

/* Kernel module descriptor */
struct kernel_module {
    char          name[64];
    void         *base;        /* base address of module memory */
    size_t         size;       /* total allocated size */
    int            state;      /* MODULE_* */

    void         (*init)(void);  /* constructor */
    void         (*exit)(void);  /* destructor */

    struct kernel_module *next;  /* linked list of all modules */

    /* Dependencies */
    struct kernel_module **deps;
    int                    num_deps;

    /* Module-local symbol table (exported symbols) */
    struct module_symbol   *syms;
    int                     num_syms;
};

/* ================================================================
 * Public API
 * ================================================================ */

/* Initialize the module subsystem. Called once during boot. */
void module_init(void);

/* Load a kernel module from a VFS path. */
int module_load(const char *path);

/* Unload a kernel module by name. */
int module_unload(const char *name);

/* Find a loaded module by name. */
struct kernel_module *module_find(const char *name);

/* Print all loaded modules to the console. */
void module_list(void);

/* Register a kernel symbol so modules can reference it. */
int module_register_symbol(const char *name, void *addr);

/* Look up a symbol in the kernel + module symbol tables. */
void *module_lookup_symbol(const char *name);

#endif /* MODULE_H */