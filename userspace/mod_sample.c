/*
 * mod_sample.c - Sample AuroraOS kernel module
 *
 * Demonstrates the kernel module interface:
 *  - init() is called when the module is loaded
 *  - exit() is called when the module is unloaded
 *
 * Uses kernel-exported symbols (console_write, log_printf, etc.)
 * which are resolved by the module loader at load time.
 *
 * Build: make modules
 * Load:  mod load /mod_sample.ko   (from the shell)
 */

/* External kernel symbols — resolved by the module loader */
extern void console_write(const char *s);
extern void console_putc(char c);
extern void log_printf(int level, const char *fmt, ...);
extern void *kmalloc(unsigned long size);
extern void kfree(void *ptr);

/* Module state — simple counter to demonstrate persistent state */
static int call_count = 0;

/*
 * init: Module constructor, called by the kernel when this module is loaded.
 */
void init(void) {
    console_write("[mod_sample] init: Hello from the sample kernel module!\n");
    log_printf(2, "mod_sample: module loaded successfully\n");  /* LOG_LEVEL_INFO = 2 */
    call_count = 0;
}

/*
 * exit: Module destructor, called by the kernel when this module is unloaded.
 */
void exit(void) {
    console_write("[mod_sample] exit: Sample module unloaded. Call count was ");
    /* Simple itoa for the counter */
    if (call_count == 0) {
        console_write("0");
    } else {
        char buf[16];
        int n = 0;
        int v = call_count;
        char tmp[16];
        int tn = 0;
        while (v > 0 && tn < 15) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        for (int i = tn - 1; i >= 0 && n < 15; i--) buf[n++] = tmp[i];
        buf[n] = '\0';
        console_write(buf);
    }
    console_write(".\n");
    log_printf(2, "mod_sample: module unloaded\n");
}

/*
 * sample_hello: A public function that can be called by other modules
 * or the kernel once the module is loaded.
 */
void sample_hello(void) {
    call_count++;
    console_write("[mod_sample] Hello #");
    /* Print call_count */
    if (call_count <= 0) {
        console_write("0");
    } else {
        char buf[16];
        int n = 0;
        int v = call_count;
        char tmp[16];
        int tn = 0;
        while (v > 0 && tn < 15) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        for (int i = tn - 1; i >= 0 && n < 15; i--) buf[n++] = tmp[i];
        buf[n] = '\0';
        console_write(buf);
    }
    console_write(" from sample module!\n");
}