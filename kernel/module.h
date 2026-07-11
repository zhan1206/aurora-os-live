/*
 * module.h - Kernel module loader interface
 *
 * Provides runtime loading/unloading of kernel modules from ELF
 * relocatable object files (.ko and .km formats).  Modules can call
 * exported kernel symbols and register their own symbols for use
 * by other modules.
 *
 * .km format: Same as .ko (ELF relocatable) but with an additional
 * .modinfo section containing version metadata for runtime checks.
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

/* Module version structure */
struct module_version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
};

/* Module metadata info (for module_export_info) */
struct module_info {
    char        name[64];
    struct module_version ver;
    char        author[64];
    char        description[128];
    char        license[32];
    int         state;
    int         refcount;
    int         num_deps;
    void       *base;
    size_t      size;
    struct module_info *next;
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
    int            refcount;   /* reference count (prevents premature unload) */

    /* Module version info */
    struct module_version version;

    /* Module metadata */
    char          author[64];
    char          description[128];
    char          license[32];

    void         (*init)(void);  /* constructor */
    void         (*exit)(void);  /* destructor */

    struct kernel_module *next;  /* linked list of all modules */

    /* Dependencies */
    struct kernel_module **deps;
    int                    num_deps;
    char                 **dep_names;  /* dependency names for version check */

    /* Module-local symbol table (exported symbols) */
    struct module_symbol   *syms;
    int                     num_syms;
};

/* ================================================================
 * Module version macros (for use in module source code)
 * ================================================================ */

/* MODULE_VERSION: Embed module version for runtime compatibility check.
 * Usage: MODULE_VERSION(1, 0, 0) */
#define MODULE_VERSION(major, minor, patch) \
    __attribute__((used, section(".modinfo.version"))) \
    static const struct module_version __module_version = { \
        (uint16_t)(major), (uint16_t)(minor), (uint16_t)(patch) \
    }

/* MODULE_AUTHOR: Embed module author info.
 * Usage: MODULE_AUTHOR("John Doe") */
#define MODULE_AUTHOR(name) \
    __attribute__((used, section(".modinfo.author"))) \
    static const char __module_author[] = name

/* MODULE_DESCRIPTION: Embed module description.
 * Usage: MODULE_DESCRIPTION("My kernel module") */
#define MODULE_DESCRIPTION(desc) \
    __attribute__((used, section(".modinfo.desc"))) \
    static const char __module_desc[] = desc

/* MODULE_LICENSE: Embed module license.
 * Usage: MODULE_LICENSE("MIT") */
#define MODULE_LICENSE(license) \
    __attribute__((used, section(".modinfo.license"))) \
    static const char __module_license[] = license

/* Kernel version for module compatibility check */
#define KERNEL_VERSION(major, minor, patch) \
    (((major) << 16) | ((minor) << 8) | (patch))

/* Current kernel version (encoded as 24-bit) */
#define KERNEL_VERSION_CURRENT \
    KERNEL_VERSION(AURORAOS_MAJOR, AURORAOS_MINOR, AURORAOS_PATCH)

/* ================================================================
 * Public API
 * ================================================================ */

/* Initialize the module subsystem. Called once during boot. */
void module_init(void);

/* Load a kernel module from a VFS path (.ko or .km). */
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

/* Increment module reference count (prevents unload). */
void module_get(struct kernel_module *mod);

/* Decrement module reference count. */
void module_put(struct kernel_module *mod);

/* Check if a module's version is compatible with the running kernel.
 * Returns 0 if compatible, -1 if incompatible. */
int module_version_check(struct kernel_module *mod);

/* Check if a module's dependencies are satisfied.
 * Returns 0 if all deps are met, -1 if any are missing. */
int module_dep_check(struct kernel_module *mod);

/* Export module info: return a linked list of all loaded module info.
 * Caller must call module_info_free() to release the list. */
struct module_info *module_export_info(void);

/* Free the module info list returned by module_export_info(). */
void module_info_free(struct module_info *list);

#endif /* MODULE_H */