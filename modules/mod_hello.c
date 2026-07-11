/*
 * mod_hello.c - AuroraOS kernel module with .km format metadata
 *
 * Demonstrates the module versioning system:
 *  - MODULE_VERSION macro embeds version info
 *  - MODULE_AUTHOR, MODULE_DESCRIPTION, MODULE_LICENSE embed metadata
 *  - init() / exit() are the module constructor/destructor
 *
 * Build:  make -f ../modules/Makefile.template MODULE=mod_hello FORMAT=km
 * Load:   mod load /mod_hello.km
 */

/* Module metadata macros (defined in module.h, redefined here for standalone build) */
#ifndef MODULE_VERSION
#define MODULE_VERSION(major, minor, patch)
#endif
#ifndef MODULE_AUTHOR
#define MODULE_AUTHOR(name)
#endif
#ifndef MODULE_DESCRIPTION
#define MODULE_DESCRIPTION(desc)
#endif
#ifndef MODULE_LICENSE
#define MODULE_LICENSE(license)
#endif

/* Declare module metadata */
MODULE_VERSION(1, 0, 0);
MODULE_AUTHOR("AuroraOS Contributors");
MODULE_DESCRIPTION("Hello World kernel module demonstrating .km format");
MODULE_LICENSE("MIT");

/* Kernel version check (must match at compile time) */
#ifndef KERNEL_VERSION_MAJOR
#define KERNEL_VERSION_MAJOR 3
#define KERNEL_VERSION_MINOR 9
#define KERNEL_VERSION_PATCH 4
#endif

/* External kernel symbols — resolved by the module loader */
extern void console_write(const char *s);
extern void console_putc(char c);
extern void log_printf(int level, const char *fmt, ...);

/* Module state */
static int greet_count = 0;

/*
 * init: Module constructor
 */
void init(void) {
    console_write("[mod_hello] init: Hello from the .km kernel module!\n");
    console_write("[mod_hello] Kernel version: ");
    console_write("v");
    console_putc('0' + KERNEL_VERSION_MAJOR);
    console_putc('.');
    /* Print minor version digits */
    {
        int minor = KERNEL_VERSION_MINOR;
        if (minor >= 10) {
            console_putc('0' + (minor / 10));
            console_putc('0' + (minor % 10));
        } else {
            console_putc('0' + minor);
        }
    }
    console_putc('.');
    {
        int patch = KERNEL_VERSION_PATCH;
        if (patch >= 10) {
            console_putc('0' + (patch / 10));
            console_putc('0' + (patch % 10));
        } else {
            console_putc('0' + patch);
        }
    }
    console_write("\n");
    log_printf(2, "mod_hello: loaded (v1.0.0, MIT License)\n");
    greet_count = 0;
}

/*
 * exit: Module destructor
 */
void exit(void) {
    console_write("[mod_hello] exit: Module unloaded after ");
    /* Print greet_count */
    if (greet_count == 0) {
        console_write("0");
    } else {
        char buf[16];
        int pos = 0;
        int v = greet_count;
        if (v < 0) { console_putc('-'); v = -v; }
        char tmp[16];
        int tn = 0;
        while (v > 0 && tn < 15) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        for (int i = tn - 1; i >= 0 && pos < 15; i--) buf[pos++] = tmp[i];
        buf[pos] = '\0';
        console_write(buf);
    }
    console_write(" greets. Goodbye!\n");
    log_printf(2, "mod_hello: unloaded\n");
}

/*
 * greet: Public function that can be called by other modules
 */
void greet(const char *from) {
    greet_count++;
    console_write("[mod_hello] Greeting #");
    {
        char buf[16];
        int pos = 0;
        int v = greet_count;
        char tmp[16];
        int tn = 0;
        while (v > 0 && tn < 15) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        for (int i = tn - 1; i >= 0 && pos < 15; i--) buf[pos++] = tmp[i];
        buf[pos] = '\0';
        console_write(buf);
    }
    console_write(" from ");
    if (from && *from) {
        console_write(from);
    } else {
        console_write("unknown");
    }
    console_write("!\n");
}