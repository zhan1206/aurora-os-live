/*
 * panic.c - Kernel panic with styled visual output
 *
 * Uses theme.h design tokens and layout.h utilities.
 * Follows AuroraOS Visual Aesthetics Design Specification:
 *   - Full red background (VGA_RED) with white text
 *   - ASCII art skull centered
 *   - Structured information hierarchy
 */
#include "include/log.h"
#include "include/theme.h"
#include "include/kstdio.h"
#include "include/print.h"
#include "layout.h"
#include "console.h"
#include <stdarg.h>
#include <stdint.h>

/* ================================================================
 * Skull ASCII art
 * ================================================================ */
static const char *skull_alt[] = {
    "      .-''''''-.      ",
    "    .'          '.    ",
    "   /   O      O   \\   ",
    "  :           `    :  ",
    "  |                |  ",
    "  :    .------.    :  ",
    "   \\  '        '  /   ",
    "    '.          .'    ",
    "      '-......-'      ",
    NULL
};

/* ================================================================
 * Register capture
 * ================================================================ */
struct reg_state {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags, cr0, cr2, cr3, cr4;
};

static void capture_regs(struct reg_state *r) {
    asm volatile (
        "mov %%rax, 0(%0)\n\t" "mov %%rbx, 8(%0)\n\t"
        "mov %%rcx, 16(%0)\n\t" "mov %%rdx, 24(%0)\n\t"
        "mov %%rsi, 32(%0)\n\t" "mov %%rdi, 40(%0)\n\t"
        "mov %%rbp, 48(%0)\n\t" "mov %%rsp, 56(%0)\n\t"
        "mov %%r8,  64(%0)\n\t" "mov %%r9,  72(%0)\n\t"
        "mov %%r10, 80(%0)\n\t" "mov %%r11, 88(%0)\n\t"
        "mov %%r12, 96(%0)\n\t" "mov %%r13, 104(%0)\n\t"
        "mov %%r14, 112(%0)\n\t" "mov %%r15, 120(%0)\n\t"
        : : "r"(r) : "memory"
    );
    uint64_t ripv; asm volatile ("lea (%%rip), %0" : "=r"(ripv)); r->rip = ripv;
    asm volatile ("pushfq\n\tpop %0" : "=r"(r->rflags));
    asm volatile ("mov %%cr0, %0" : "=r"(r->cr0));
    asm volatile ("mov %%cr2, %0" : "=r"(r->cr2));
    asm volatile ("mov %%cr3, %0" : "=r"(r->cr3));
    asm volatile ("mov %%cr4, %0" : "=r"(r->cr4));
}

static void reg_line(const char *name, uint64_t val) {
    console_write_ansi(PANIC_LABEL_FG);
    console_write(PANIC_LABEL_PREFIX);
    console_write_ansi(FG_WHITE);
    console_write(name);
    console_write(": ");
    console_write_ansi(PANIC_VALUE_FG);
    console_write("0x");
    char hex[17];
    uitoa_hex(val, hex, sizeof(hex));
    console_write(hex);
}

/* ================================================================
 * panic() — Emergency display
 * ================================================================ */
void panic(const char *fmt, ...) {
    va_list ap;
    asm volatile ("cli");

    /* Full red screen — emergency visual takeover */
    console_set_bg(PANIC_BG);
    console_set_fg(PANIC_FG);
    console_clear();

    /* ---- Phase 1: Skull Art ---- */
    int skull_lines = 0;
    while (skull_alt[skull_lines]) skull_lines++;
    console_vcenter(skull_lines + 8);

    for (int i = 0; skull_alt[i]; ++i) {
        console_write_ansi(FG_WHITE);
        console_write_centered(skull_alt[i]);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }

    /* ---- Phase 2: Title ---- */
    console_vpad(1);
    console_write_ansi(PANIC_TITLE_FG);
    console_write_centered("KERNEL PANIC");
    console_write_ansi(SGR_RESET);
    console_vpad(1);

    /* ---- Phase 3: Error Message ---- */
    console_set_fg(PANIC_MSG_COLOR);
    va_start(ap, fmt);
    {
        char buf[256]; int n = 0;
        const char *p = fmt;
        while (*p && n < 250) {
            if (*p == '%') {
                p++;
                /* Handle length modifiers: hh, h, l, ll */
                int long_mod = 0;  /* 0=none, 1=l, 2=ll */
                if (*p == 'l') { p++; long_mod = 1; }
                if (*p == 'l') { p++; long_mod = 2; }
                if (*p == 's') {
                    const char *s = va_arg(ap, const char*);
                    if (!s) s = "(null)";
                    while (*s && n < 250) buf[n++] = *s++;
                } else if (*p == 'p' || *p == 'x') {
                    uint64_t v;
                    if (long_mod == 2)      v = va_arg(ap, uint64_t);
                    else if (long_mod == 1) v = va_arg(ap, unsigned long);
                    else                    v = va_arg(ap, uint64_t);
                    buf[n++] = '0'; buf[n++] = 'x';
                    char hex[17];
                    uitoa_hex(v, hex, sizeof(hex));
                    const char *h = hex; while (*h && n < 250) buf[n++] = *h++;
                } else if (*p == 'u') {
                    uint64_t v;
                    if (long_mod == 2)      v = va_arg(ap, uint64_t);
                    else if (long_mod == 1) v = va_arg(ap, unsigned long);
                    else                    v = va_arg(ap, unsigned int);
                    char tmp[24];
                    int tn = 0;
                    if (v == 0) tmp[tn++] = '0';
                    while (v && tn < 23) { tmp[tn++] = '0' + (v % 10); v /= 10; }
                    for (int i = tn - 1; i >= 0 && n < 250; i--) buf[n++] = tmp[i];
                } else if (*p == 'd') {
                    int64_t v;
                    if (long_mod == 2)      v = va_arg(ap, int64_t);
                    else if (long_mod == 1) v = va_arg(ap, long);
                    else                    v = va_arg(ap, int);
                    char tmp[24];
                    int tn = 0;
                    int neg = v < 0;
                    uint64_t uv = neg ? (uint64_t)(-(v + 1)) + 1ULL : (uint64_t)v;
                    if (uv == 0) tmp[tn++] = '0';
                    while (uv && tn < 23) { tmp[tn++] = '0' + (uv % 10); uv /= 10; }
                    if (neg) tmp[tn++] = '-';
                    for (int i = tn - 1; i >= 0 && n < 250; i--) buf[n++] = tmp[i];
                } else {
                    /* Handle trailing '%' or unknown format specifier:
                     * print the '%' literally, and if *p is not '\0',
                     * print the character after it as well. */
                    buf[n++] = '%';
                    if (*p) buf[n++] = *p;
                }
                if (*p) p++;
            } else {
                buf[n++] = *p++;
            }
        }
        buf[n] = '\0';
        console_write_centered(buf);
    }
    va_end(ap);

    console_vpad(2);

    /* ---- Phase 4: Register Dump (4 columns) ---- */
    struct reg_state regs;
    capture_regs(&regs);
    console_set_fg(PANIC_FG);

    reg_line("RAX", regs.rax); console_write("  ");
    reg_line("RBX", regs.rbx); console_write("  ");
    reg_line("RCX", regs.rcx); console_write("  ");
    reg_line("RDX", regs.rdx); console_putc('\n');
    reg_line("RSI", regs.rsi); console_write("  ");
    reg_line("RDI", regs.rdi); console_write("  ");
    reg_line("RBP", regs.rbp); console_write("  ");
    reg_line("RSP", regs.rsp); console_putc('\n');
    reg_line("R8 ", regs.r8);  console_write("  ");
    reg_line("R9 ", regs.r9);  console_write("  ");
    reg_line("R10", regs.r10); console_write("  ");
    reg_line("R11", regs.r11); console_putc('\n');
    reg_line("R12", regs.r12); console_write("  ");
    reg_line("R13", regs.r13); console_write("  ");
    reg_line("R14", regs.r14); console_write("  ");
    reg_line("R15", regs.r15); console_putc('\n');
    reg_line("RIP", regs.rip); console_write("  ");
    reg_line("FLG", regs.rflags); console_write("  ");
    reg_line("CR2", regs.cr2); console_write("  ");
    reg_line("CR3", regs.cr3); console_putc('\n');

    /* ---- Phase 4.5: Kernel Stack Trace ---- */
    console_vpad(1);
    console_write_ansi(PANIC_TITLE_FG);
    console_write("     Stack Trace (most recent call first):");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    {
        uint64_t *rbp_ptr = (uint64_t *)regs.rbp;
        int depth = 0;
        int max_depth = 12;

        while (rbp_ptr && depth < max_depth) {
            uint64_t *next_rbp = (uint64_t *)*rbp_ptr;
            uint64_t ret_addr = *(rbp_ptr + 1);

            /* Sanity check: kernel code starts at 1MB, no upper bound needed for 64-bit */
            if (!next_rbp || ret_addr < 0x100000) break;

            console_write_ansi(PANIC_LABEL_FG);
            console_write("       #");
            {
                char dbuf[4];
                int n = 0;
                int d = depth;
                if (d == 0) { dbuf[0] = '0'; n = 1; }
                else { while (d > 0 && n < 3) { dbuf[n++] = '0' + (d % 10); d /= 10; } }
                for (int i = n - 1; i >= 0; i--) console_putc(dbuf[i]);
            }
            console_write("  ");
            console_write_ansi(PANIC_VALUE_FG);
            console_write("0x");
            {
                char hex[17];
                uitoa_hex(ret_addr, hex, sizeof(hex));
                console_write(hex);
            }
            console_write_ansi(SGR_RESET);
            console_putc('\n');

            if (next_rbp == rbp_ptr) break; /* prevent infinite loop */
            rbp_ptr = next_rbp;
            depth++;
        }
    }

    /* ---- Phase 5: Bottom Message ---- */
    int cur_row;
    console_get_cursor(&cur_row, NULL);
    int remaining = ROWS - cur_row - 2;
    for (int i = 0; i < remaining; ++i) console_putc('\n');

    console_write_ansi(PANIC_BOTTOM_FG);
    console_write_centered("System halted. Press Ctrl+Alt+Del to restart.");
    console_write_ansi(SGR_RESET);

    for (;;) asm volatile ("hlt");
}
