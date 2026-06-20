/*
 * main.c - Kernel entry point with styled boot sequence
 *
 * Uses theme.h design tokens for all visual elements.
 * Layout follows AuroraOS Visual Aesthetics Design Specification §6.1.
 */
#include "include/print.h"
#include "include/kstdio.h"
#include "include/theme.h"
#include "layout.h"
#include "mem.h"
#include "sched.h"
#include "include/pit.h"
#include "include/idt.h"
#include "include/log.h"
#include "console.h"
#include "shell.h"
#include "pagetable.h"
#include "syscall.h"
#include "fs.h"
#include "smp.h"
#include "block_dev.h"
#include "perf.h"
#include "sysctl.h"
#include "procfs.h"
#include "stack_protect.h"
#include "aslr.h"
#include "module.h"
#include "rtc.h"
#include "../boot/boot_info.h"

/* ================================================================
 * Global Theme State (defined in theme.h)
 * ================================================================ */
int g_theme_mode      = THEME_DARK;
int g_reduced_motion  = 0;
int g_anim_enabled    = 1;

#define AURORA_VERSION  "AuroraOS v3.2.0"
#define AURORA_BUILD    "2026-06-20 12:00"
#define AURORA_COPY     "(c) 2026 AuroraOS Contributors — MIT License"

/* ================================================================
 * Boot banner — Aurora (northern lights) themed ASCII logo
 * ================================================================ */
static const char *logo[] = {
    "   .  *  ~  .  *  ~  .  *  ~  .  *  ~  .  *  ~  .",
    "  *                                               *",
    " ~         A   U   R   O   R   A   O  S          ~",
    "  *                                               *",
    "   ~  *  .  ~  *  .  ~  *  .  ~  *  .  ~  *  .  ~",
    NULL
};

static const char *logo_title[] = {
    "",
    "    A Self-Built x86_64 Operating System",
    NULL
};

static void boot_print_logo(void) {
    /* Top decorative line */
    console_write_ansi(BOOT_LOGO_SHADOW);
    console_draw_hr(SEP_DOT);
    console_write_ansi(SGR_RESET);
    console_vpad(1);

    /* Logo */
    for (int i = 0; logo[i]; ++i) {
        console_write_ansi(BOOT_LOGO_COLOR);
        console_write_centered(logo[i]);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }

    /* Subtitle */
    for (int i = 0; logo_title[i]; ++i) {
        console_write_ansi(CLR_MUTED);
        console_write_centered(logo_title[i]);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }

    console_vpad(1);

    /* Version / Build info in a compact box */
    console_write_ansi(BOOT_VERSION_COLOR);
    console_write_centered(AURORA_VERSION);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_write_ansi(BOOT_BUILD_COLOR);
    console_write_centered(AURORA_BUILD);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_write_ansi(CLR_MUTED);
    console_write_centered(AURORA_COPY);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_vpad(1);
    /* Bottom decorative line */
    console_write_ansi(BOOT_LOGO_SHADOW);
    console_draw_hr(SEP_DOT);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* ================================================================
 * Demo tasks
 * ================================================================ */
static void task_fn1(void) {
    for (int i = 0; i < 5; ++i) {
        log_printf(LOG_LEVEL_INFO, "task1 running iter=%d\n", i);
        yield();
    }
    do_exit_current(0);
}

static void task_fn2(void) {
    for (int i = 0; i < 5; ++i) {
        log_printf(LOG_LEVEL_INFO, "task2 running iter=%d\n", i);
        yield();
    }
    do_exit_current(0);
}

/* ================================================================
 * kernel_main — Boot sequence with visual status reporting
 *
 * @magic:    Multiboot magic (0x2BADB002 for MB1, 0x36d76289 for MB2)
 * @mb_info:  Multiboot info structure pointer (physical address)
 * ================================================================ */
void kernel_main(uint32_t magic, void *mb_info) {
    /* Detect UEFI boot vs Multiboot boot */
    int is_uefi = (magic == UEFI_BOOT_MAGIC);
    struct uefi_boot_info *uefi_bi = (void *)0;
    if (is_uefi) {
        uefi_bi = (struct uefi_boot_info *)mb_info;
    }

    /* Early serial init — must be first for panic messages */
    serial_init();
    log_set_level(LOG_LEVEL_DEBUG);  /* Enable debug logging during boot */

    /* Stack protector must be initialized before any C code with stack arrays */
    stack_protect_init();

    /* === Phase 1: Boot Splash Screen === */
    if (is_uefi && uefi_bi->fb_valid) {
        console_init_fb(uefi_bi->fb_addr, uefi_bi->fb_width, uefi_bi->fb_height,
                        uefi_bi->fb_pitch, uefi_bi->fb_bpp);
    } else {
        console_init();
    }
    console_clear();
    console_hide_cursor();

    /* Center logo vertically: 5 logo + 2 subtitle + 1 top-pad + 1 top-sep + 3 version + 1 bot-pad + 1 bot-sep = 15 */
    console_vcenter(15);
    boot_print_logo();

    /* Mirror logo to serial for remote verification */
    printk("\n");
    for (int i = 0; logo[i]; ++i) {
        printk(logo[i]);
        printk("\n");
    }
    printk(AURORA_VERSION);
    printk("\n\n");

    console_show_cursor();

    /* === Phase 2: Hardware Initialization (with progress) === */
    console_write_ansi(BOOT_STAGE_LABEL);
    console_write_centered("Initializing system...");
    console_write_ansi(SGR_RESET);
    console_vpad(1);

    int boot_step = 0, boot_total = 16;
    #define BOOT_STEP() do { \
        boot_step++; \
        console_write_ansi(BOOT_PROGRESS_FILL); \
        console_draw_progress_bar_styled(boot_step * 100 / boot_total, 40, \
            BOOT_PROGRESS_FILL, BOOT_PROGRESS_BG, CLR_MUTED, BOOT_PROGRESS_FILL); \
        console_write_ansi(SGR_RESET); \
        console_putc('\n'); \
    } while(0)

    console_status_ok("Serial port (COM1 115200 8N1)");
    BOOT_STEP();

    /* Physical memory — auto-detects Multiboot1 or Multiboot2 */
    uint64_t mem_mb = 64;
    if (is_uefi) {
        phys_mem_init_uefi(uefi_bi);
    } else {
        phys_mem_init(mb_info);
    }
    {
        uint64_t total, free, used;
        mem_get_stats(&total, &free, &used);
        mem_mb = total / (1024 * 1024);
    }
    console_write_ansi(BOOT_OK_FG);
    console_write(STATUS_OK_STR);
    console_write_ansi(SGR_RESET);
    console_write(" Physical memory: ");
    {
        char buf[16];
        uitoa(mem_mb, buf, sizeof(buf));
        console_write(buf);
        console_write(" MiB\n");
    }
    BOOT_STEP();

    slab_init();
    console_status_ok("Slab allocator (8 size classes)");
    BOOT_STEP();

    aslr_init();
    console_status_ok("ASLR initialized (xorshift64 PRNG)");
    BOOT_STEP();

    page_table_init();
    console_status_ok("Page tables (4-level, NX enabled)");
    BOOT_STEP();

    printk_console_ready();

    /* === Phase 3: Kernel Subsystems === */
    printk("scheduler_init...\n");
    scheduler_init();
    printk("scheduler_init done\n");
    console_status_ok("Scheduler (RR + idle task + PID bitmap)");
    BOOT_STEP();

    printk("syscall_init...\n");
    syscall_init();
    printk("syscall_init done\n");
    console_status_ok("SYSCALL/SYSRET MSRs configured");
    BOOT_STEP();

    printk("irq_init...\n");
    irq_init();
    printk("irq_init done\n");
    console_status_ok("IDT + PIC remap + keyboard driver");
    BOOT_STEP();

    /*
     * Initialize SMP after IRQ system is ready.
     * This parses ACPI MADT, detects CPUs, and starts APs.
     * Falls back to single-CPU mode if no ACPI/MADT found.
     */
    printk("smp_init...\n");
    smp_init(mb_info);
    printk("smp_init done\n");
    console_status_ok("SMP (detected CPUs)");
    BOOT_STEP();

    /*
     * rodata_protect must be called AFTER irq_init() so that the IDT
     * is set up to handle any page faults that may occur during page
     * table modification (e.g., TLB shootdown, split_huge_page).
     * Without the IDT, a page fault during split_huge_page would
     * cause a triple fault and silent hang.
     */
    printk("rodata_protect...\n");
    rodata_protect();
    printk("rodata_protect done\n");
    console_status_ok("Read-only data segment");
    BOOT_STEP();

    pit_init(100);
    console_status_ok("PIT timer (100 Hz)");
    BOOT_STEP();

    rtc_init();
    console_status_ok("CMOS RTC driver");
    BOOT_STEP();

    perf_init();
    console_status_ok("Performance counters");
    BOOT_STEP();

    sysctl_init();
    console_status_ok("Sysctl interface");
    BOOT_STEP();

    /* === Phase 3.5: File System === */
    ramdisk_init(0);  /* 16 MiB RAM disk for testing */
    fs_init();
    console_status_ok("VFS + RamFS mounted");
    BOOT_STEP();

    module_init();
    console_status_ok("Module loader (kernel symbol table)");
    BOOT_STEP();

    #undef BOOT_STEP

    /* Boot complete indicator */
    console_vpad(1);
    console_write_ansi(BOOT_OK_FG);
    console_write_centered("[ System Ready ]");
    console_write_ansi(SGR_RESET);
    console_vpad(2);

    /* Separator */
    console_draw_hr(SEP_LINE);
    console_putc('\n');

    /* === Phase 4: Self-test & Launch === */
    kernel_selftest();

    /* Demo tasks */
    create_task(task_fn1);
    create_task(task_fn2);
    create_task(shell_main);

    /* Scheduling loop */
    while (1) yield();
}
