/*
 * console.c - VGA text mode console with ANSI escape support
 *
 * Uses theme.h design tokens for default visual attributes.
 * When booted via UEFI GOP, uses framebuffer-based console.
 */
#include "console.h"
#include "include/log.h"
#include "include/portio.h"
#include "include/theme.h"
#include "smp.h"
#include <stdint.h>
#include <string.h>

#define VGA_BUF ((volatile uint16_t *)0xB8000)

/* Default VGA attribute: light grey text on black background */
#define DEFAULT_VGA_ATTR  ((uint8_t)((VGA_BLACK << 4) | VGA_LIGHT_GREY))

static int cursor_row = 0;
static int cursor_col = 0;
static int cursor_visible = 1;
static uint8_t current_attr = DEFAULT_VGA_ATTR;

/* ================================================================
 * Console output redirection buffer
 *
 * When active, all console_write/console_putc output is captured
 * in a kernel buffer instead of being written to the screen.
 * Used by the shell for implementing > and >> redirection with
 * built-in commands.  Maximum capture size is 64 KiB.
 * ================================================================ */
#define CONSOLE_REDIRECT_BUF_SIZE 65536
static char  *g_redirect_buf = NULL;
static size_t g_redirect_len = 0;
static int    g_redirect_active = 0;

void console_redirect_begin(void) {
    if (!g_redirect_buf) {
        extern void *kmalloc(size_t size);
        g_redirect_buf = (char *)kmalloc(CONSOLE_REDIRECT_BUF_SIZE);
    }
    if (g_redirect_buf) {
        g_redirect_len = 0;
        g_redirect_active = 1;
    }
}

void console_redirect_end(char *buf, size_t bufsize, size_t *out_len) {
    g_redirect_active = 0;
    if (out_len) *out_len = g_redirect_len;
    if (buf && bufsize > 0 && g_redirect_buf && g_redirect_len > 0) {
        size_t copy = g_redirect_len;
        if (copy >= bufsize) copy = bufsize - 1;
        for (size_t i = 0; i < copy; i++) buf[i] = g_redirect_buf[i];
        buf[copy] = '\0';
    }
}

int console_redirect_active(void) {
    return g_redirect_active;
}

/* ================================================================
 * Framebuffer state (for UEFI GOP boot)
 * ================================================================ */
static int      g_use_fb = 0;           /* 1 = framebuffer, 0 = VGA */
static uint32_t *g_fb_addr = (void *)0;  /* framebuffer base (virtual) */
static uint32_t  g_fb_width = 0;         /* pixels */
static uint32_t  g_fb_height = 0;        /* pixels */
static uint32_t  g_fb_pitch = 0;         /* bytes per scanline */
static uint32_t  g_fb_bpp = 0;           /* bits per pixel */
static uint32_t  g_fb_cols = 80;         /* text columns */
static uint32_t  g_fb_rows = 25;         /* text rows */

/* Font dimensions */
#define FB_FONT_W  8
#define FB_FONT_H  16

/* ================================================================
 * 8x16 bitmap font (ASCII 32-126, 95 characters)
 *
 * Each character is 16 bytes, each byte is one row of 8 pixels.
 * Bit 7 (MSB) = leftmost pixel, bit 0 = rightmost pixel.
 * ================================================================ */
static const uint8_t font_8x16[95][16] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 34 '"' */ {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00},
    /* 36 '$' */ {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00,0x00},
    /* 37 '%' */ {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    /* 38 '&' */ {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 39 ''' */ {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    /* 41 ')' */ {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 42 '*' */ {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 43 '+' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    /* 45 '-' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 47 '/' */ {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    /* 48 '0' */ {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xD6,0xD6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    /* 49 '1' */ {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    /* 50 '2' */ {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 51 '3' */ {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 52 '4' */ {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    /* 53 '5' */ {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 54 '6' */ {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 55 '7' */ {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    /* 56 '8' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 57 '9' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    /* 58 ':' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 59 ';' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 60 '<' */ {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    /* 61 '=' */ {0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 62 '>' */ {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    /* 63 '?' */ {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 64 '@' */ {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00},
    /* 65 'A' */ {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 66 'B' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    /* 67 'C' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* 68 'D' */ {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    /* 69 'E' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 70 'F' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 71 'G' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    /* 72 'H' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 73 'I' */ {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 74 'J' */ {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* 75 'K' */ {0x00,0x00,0xE6,0x66,0x6C,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 76 'L' */ {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 77 'M' */ {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 78 'N' */ {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 79 'O' */ {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    /* 80 'P' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 81 'Q' */ {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    /* 82 'R' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 83 'S' */ {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 84 'T' */ {0x00,0x00,0x7E,0x7E,0x5A,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 85 'U' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 86 'V' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00},
    /* 87 'W' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00},
    /* 88 'X' */ {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 89 'Y' */ {0x00,0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 90 'Z' */ {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 91 '[' */ {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    /* 92 '\' */ {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00},
    /* 93 ']' */ {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    /* 94 '^' */ {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00},
    /* 96 '`' */ {0x00,0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 98 'b' */ {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* 99 'c' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 100 'd' */ {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 101 'e' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 102 'f' */ {0x00,0x00,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 103 'g' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00},
    /* 104 'h' */ {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 105 'i' */ {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 106 'j' */ {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},
    /* 107 'k' */ {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 108 'l' */ {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 109 'm' */ {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00},
    /* 110 'n' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* 111 'o' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 112 'p' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    /* 113 'q' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00},
    /* 114 'r' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 115 's' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 116 't' */ {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00},
    /* 117 'u' */ {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 118 'v' */ {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00},
    /* 119 'w' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00},
    /* 120 'x' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    /* 121 'y' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00},
    /* 122 'z' */ {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 123 '{' */ {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    /* 124 '|' */ {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 125 '}' */ {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    /* 126 '~' */ {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* ================================================================
 * Framebuffer helper: draw a single pixel
 * ================================================================ */
static void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_fb_width || y >= g_fb_height) return;
    uint32_t *pixel = (uint32_t *)((uint8_t *)g_fb_addr + y * g_fb_pitch + x * 4);
    *pixel = color;
}

/* ================================================================
 * Framebuffer helper: draw a filled rectangle
 * ================================================================ */
static void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x >= g_fb_width || y >= g_fb_height) return;
    if (x + w > g_fb_width) w = g_fb_width - x;
    if (y + h > g_fb_height) h = g_fb_height - y;
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++) {
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

/* ================================================================
 * Framebuffer helper: draw a character at cell position
 * ================================================================ */
static void fb_draw_char(int col, int row, char c, uint32_t fg, uint32_t bg) {
    int px = col * FB_FONT_W;
    int py = row * FB_FONT_H;

    /* Draw background */
    fb_fill_rect((uint32_t)px, (uint32_t)py, FB_FONT_W, FB_FONT_H, bg);

    if (c < 32 || c > 126) return;

    const uint8_t *glyph = font_8x16[c - 32];
    for (int gy = 0; gy < FB_FONT_H; gy++) {
        uint8_t row_bits = glyph[gy];
        for (int gx = 0; gx < FB_FONT_W; gx++) {
            if (row_bits & (0x80 >> gx)) {
                fb_put_pixel((uint32_t)(px + gx), (uint32_t)(py + gy), fg);
            }
        }
    }
}

/* ================================================================
 * Framebuffer helper: scroll up one line
 * ================================================================ */
static void fb_scroll_up(void) {
    uint32_t line_h = FB_FONT_H;

    /* Copy all lines up by one */
    for (uint32_t row = 0; row < g_fb_rows - 1; row++) {
        uint8_t *dst = (uint8_t *)g_fb_addr + row * line_h * g_fb_pitch;
        uint8_t *src = (uint8_t *)g_fb_addr + (row + 1) * line_h * g_fb_pitch;
        for (uint32_t y = 0; y < line_h; y++) {
            for (uint32_t x = 0; x < g_fb_cols * FB_FONT_W; x++) {
                uint32_t *dp = (uint32_t *)(dst + y * g_fb_pitch + x * 4);
                uint32_t *sp = (uint32_t *)(src + y * g_fb_pitch + x * 4);
                *dp = *sp;
            }
        }
    }

    /* Clear the last line */
    fb_fill_rect(0, (g_fb_rows - 1) * FB_FONT_H, g_fb_cols * FB_FONT_W, FB_FONT_H, 0x00000000);
}

/* ================================================================
 * Framebuffer: update hardware cursor (no-op for framebuffer)
 * ================================================================ */
static void fb_update_cursor(void) {
    /* Framebuffer cursor is drawn as part of the character cell.
     * We could draw a blinking underline, but for now this is a no-op. */
}

/* ================================================================
 * Framebuffer: initialize console
 * ================================================================ */
void console_init_fb(uint64_t fb_addr, uint32_t width, uint32_t height,
                     uint32_t pitch, uint32_t bpp) {
    (void)bpp;  /* assume 32-bit BGRA */

    g_use_fb = 1;
    g_fb_addr = (uint32_t *)(uintptr_t)fb_addr;
    g_fb_width = width;
    g_fb_height = height;
    g_fb_pitch = pitch;
    g_fb_bpp = 32;

    /* Calculate text dimensions */
    g_fb_cols = width / FB_FONT_W;
    g_fb_rows = height / FB_FONT_H;
    if (g_fb_cols < 80) g_fb_cols = 80;
    if (g_fb_rows < 25) g_fb_rows = 25;

    /* Clear screen to black */
    fb_fill_rect(0, 0, width, height, 0x00000000);

    cursor_row = 0;
    cursor_col = 0;
    current_attr = 0x07;
    cursor_visible = 1;

    log_printf(LOG_LEVEL_INFO, "console: framebuffer %dx%d, %dx%d text cells\n",
               (int)width, (int)height, (int)g_fb_cols, (int)g_fb_rows);
}

/* ================================================================
 * Hardware cursor
 * ================================================================ */
static void update_hw_cursor(void) {
    if (g_use_fb) {
        fb_update_cursor();
        return;
    }
    uint16_t pos = (uint16_t)(cursor_row * COLS + cursor_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* ================================================================
 * VGA/framebuffer output spinlock
 *
 * Protects all writes to the VGA buffer and framebuffer from
 * concurrent access on multi-CPU systems. Without this lock,
 * simultaneous console output from multiple cores can cause
 * screen corruption.
 * ================================================================ */
typedef struct {
    volatile uint32_t locked;
} spinlock_t;

static spinlock_t console_out_lock = {0};

static inline void console_out_lock_acquire(void) {
    while (1) {
        uint32_t old = 0, new = 1;
        asm volatile (
            "lock cmpxchgl %2, %1"
            : "=a"(old), "+m"(console_out_lock.locked)
            : "r"(new), "0"(old)
            : "memory"
        );
        if (old == 0) break;
        asm volatile ("pause" ::: "memory");
    }
}

static inline void console_out_lock_release(void) {
    asm volatile ("movl $0, %0" : "=m"(console_out_lock.locked) : : "memory");
}

/* ================================================================
 * Cell operations
 * ================================================================ */
static void put_cell_raw(int r, int c, char ch, uint8_t attr) {
    console_out_lock_acquire();
    if (g_use_fb) {
        /* Convert VGA attribute to framebuffer colors */
        uint8_t fg_idx = attr & 0x0F;
        uint8_t bg_idx = (attr >> 4) & 0x0F;

        /* VGA 16-color palette to RGB */
        static const uint32_t vga_palette[16] = {
            0x00000000, /* 0: Black */
            0x000000AA, /* 1: Blue */
            0x0000AA00, /* 2: Green */
            0x0000AAAA, /* 3: Cyan */
            0x00AA0000, /* 4: Red */
            0x00AA00AA, /* 5: Magenta */
            0x00AA5500, /* 6: Brown */
            0x00AAAAAA, /* 7: Light Grey */
            0x00555555, /* 8: Dark Grey */
            0x005555FF, /* 9: Light Blue */
            0x0055FF55, /* 10: Light Green */
            0x0055FFFF, /* 11: Light Cyan */
            0x00FF5555, /* 12: Light Red */
            0x00FF55FF, /* 13: Light Magenta */
            0x00FFFF55, /* 14: Yellow */
            0x00FFFFFF, /* 15: White */
        };

        uint32_t fg = vga_palette[fg_idx];
        uint32_t bg = vga_palette[bg_idx];

        fb_draw_char(c, r, ch, fg, bg);
        return;
    }

    int idx = r * COLS + c;
    VGA_BUF[idx] = (uint16_t)ch | ((uint16_t)attr << 8);
}

void console_putc_attr(int row, int col, char c, uint8_t attr) {
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return;
    put_cell_raw(row, col, c, attr);
}

static void scroll_up(void) {
    if (g_use_fb) {
        fb_scroll_up();
        return;
    }

    /* Use memcpy for bulk row move (160 bytes per row) */
    for (int r = 1; r < ROWS; ++r) {
        memcpy((uint16_t *)&VGA_BUF[(r-1)*COLS],
               (const uint16_t *)&VGA_BUF[r*COLS],
               COLS * sizeof(uint16_t));
    }
    /* Clear last row */
    uint16_t blank = (uint16_t)' ' | ((uint16_t)current_attr << 8);
    for (int c = 0; c < COLS; ++c) {
        VGA_BUF[(ROWS-1)*COLS + c] = blank;
    }
}

/* ================================================================
 * Cursor control
 * ================================================================ */
void console_set_cursor(int row, int col) {
    int max_rows = g_use_fb ? (int)g_fb_rows : ROWS;
    int max_cols = g_use_fb ? (int)g_fb_cols : COLS;
    if (row < 0) row = 0;
    if (row >= max_rows) row = max_rows - 1;
    if (col < 0) col = 0;
    if (col >= max_cols) col = max_cols - 1;
    cursor_row = row;
    cursor_col = col;
    if (cursor_visible) update_hw_cursor();
}

void console_get_cursor(int *row, int *col) {
    if (row) *row = cursor_row;
    if (col) *col = cursor_col;
}

void console_hide_cursor(void) {
    cursor_visible = 0;
    if (g_use_fb) return;
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

void console_show_cursor(void) {
    cursor_visible = 1;
    if (g_use_fb) return;
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00);
    update_hw_cursor();
}

/* ================================================================
 * Screen control
 * ================================================================ */
void console_clear(void) {
    if (g_use_fb) {
        fb_fill_rect(0, 0, g_fb_width, g_fb_height, 0x00000000);
        cursor_row = 0; cursor_col = 0;
        return;
    }
    /* Optimized: use bulk memset-like fill instead of nested loops */
    uint16_t fill = (uint16_t)' ' | ((uint16_t)current_attr << 8);
    for (int i = 0; i < ROWS * COLS; ++i)
        VGA_BUF[i] = fill;
    cursor_row = 0; cursor_col = 0;
    update_hw_cursor();
}

void console_clear_to_end(void) {
    if (g_use_fb) {
        int max_cols = (int)g_fb_cols;
        for (int c = cursor_col; c < max_cols; c++)
            put_cell_raw(cursor_row, c, ' ', current_attr);
        for (int r = cursor_row + 1; r < (int)g_fb_rows; r++)
            for (int c = 0; c < max_cols; c++)
                put_cell_raw(r, c, ' ', current_attr);
        return;
    }
    uint16_t fill = (uint16_t)' ' | ((uint16_t)current_attr << 8);
    int r = cursor_row, c = cursor_col;

    /* Clear rest of current row */
    for (; c < COLS; ++c)
        put_cell_raw(r, c, ' ', current_attr);

    /* Bulk-fill remaining rows */
    for (r = cursor_row + 1; r < ROWS; ++r) {
        for (c = 0; c < COLS; ++c)
            VGA_BUF[r * COLS + c] = fill;
    }
}

void console_set_fg(uint8_t color) {
    current_attr = (current_attr & 0xF0) | (color & 0x0F);
}

void console_set_bg(uint8_t color) {
    current_attr = (current_attr & 0x0F) | ((color & 0x0F) << 4);
}

void console_reset_attr(void) {
    current_attr = 0x07;
}

uint8_t console_get_attr(void) {
    return current_attr;
}

/* ================================================================
 * Character output
 * ================================================================ */
void console_putc(char c) {
    /* If redirection is active, capture output to buffer instead of screen */
    if (g_redirect_active && g_redirect_buf) {
        if (g_redirect_len < CONSOLE_REDIRECT_BUF_SIZE - 1) {
            g_redirect_buf[g_redirect_len++] = c;
        }
        return;
    }

    int max_cols = g_use_fb ? (int)g_fb_cols : COLS;
    int max_rows = g_use_fb ? (int)g_fb_rows : ROWS;

    if (c == '\n') {
        cursor_col = 0; cursor_row++;
        if (cursor_row >= max_rows) { scroll_up(); cursor_row = max_rows - 1; }
        if (cursor_visible) update_hw_cursor();
        return;
    }
    if (c == '\r') { cursor_col = 0; if (cursor_visible) update_hw_cursor(); return; }
    if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            put_cell_raw(cursor_row, cursor_col, ' ', current_attr);
        }
        if (cursor_visible) update_hw_cursor();
        return;
    }
    if (c == '\t') {
        int spaces = 8 - (cursor_col % 8);
        for (int i = 0; i < spaces; ++i) console_putc(' ');
        return;
    }
    put_cell_raw(cursor_row, cursor_col, c, current_attr);
    cursor_col++;
    if (cursor_col >= max_cols) {
        cursor_col = 0; cursor_row++;
        if (cursor_row >= max_rows) { scroll_up(); cursor_row = max_rows - 1; }
    }
    if (cursor_visible) update_hw_cursor();
}

void console_write(const char *s) {
    for (size_t i = 0; s && s[i]; ++i) console_putc(s[i]);
}

/* ================================================================
 * ANSI escape sequence parser
 *
 * Supported sequences:
 *   ESC [ n m        — SGR (Select Graphic Rendition)
 *   ESC [ n A/B/C/D  — Cursor up/down/forward/back
 *   ESC [ n ; m H/f  — Cursor position
 *   ESC [ 2 J        — Clear screen
 *   ESC [ K          — Clear to end of line
 *   ESC [ ? 25 l/h   — Hide/show cursor
 *   ESC [ s/u        — Save/restore cursor position
 * ================================================================ */
void console_write_ansi(const char *s) {
    if (!s) return;
    int saved_row = 0, saved_col = 0;
    int max_rows = g_use_fb ? (int)g_fb_rows : ROWS;
    int max_cols = g_use_fb ? (int)g_fb_cols : COLS;

    while (*s) {
        if (*s == '\x1b' && s[1] == '[') {
            s += 2; /* skip ESC [ */
            /* Parse numeric parameters */
            int params[4] = {0, 0, 0, 0};
            int np = 0;
            int has_qmark = 0;

            if (*s == '?') { has_qmark = 1; s++; }

            while (*s >= '0' && *s <= '9') {
                params[np] = params[np] * 10 + (*s - '0');
                s++;
            }
            while (*s == ';' && np < 3) {
                np++; s++;
                while (*s >= '0' && *s <= '9') {
                    params[np] = params[np] * 10 + (*s - '0');
                    s++;
                }
            }

            char cmd = *s;
            if (cmd) s++;

            switch (cmd) {
                case 'm': /* SGR */
                    for (int i = 0; i <= np; ++i) {
                        int p = params[i];
                        if (p == 0) console_reset_attr();
                        else if (p == 1) current_attr |= 0x08; /* bold→bright */
                        else if (p >= 30 && p <= 37) {
                            current_attr = (current_attr & 0xF0) | (uint8_t)(p - 30);
                        }
                        else if (p >= 40 && p <= 47) {
                            current_attr = (current_attr & 0x0F) | (uint8_t)((p - 40) << 4);
                        }
                        else if (p >= 90 && p <= 97) {
                            current_attr = (current_attr & 0xF0) | (uint8_t)(p - 90 + 8);
                        }
                        else if (p >= 100 && p <= 107) {
                            current_attr = (current_attr & 0x0F) | (uint8_t)((p - 100 + 8) << 4);
                        }
                    }
                    break;

                case 'A': /* cursor up */
                    cursor_row -= (params[0] > 0 ? params[0] : 1);
                    if (cursor_row < 0) cursor_row = 0;
                    if (cursor_visible) update_hw_cursor();
                    break;

                case 'B': /* cursor down */
                    cursor_row += (params[0] > 0 ? params[0] : 1);
                    if (cursor_row >= max_rows) cursor_row = max_rows - 1;
                    if (cursor_visible) update_hw_cursor();
                    break;

                case 'C': /* cursor forward */
                    cursor_col += (params[0] > 0 ? params[0] : 1);
                    if (cursor_col >= max_cols) cursor_col = max_cols - 1;
                    if (cursor_visible) update_hw_cursor();
                    break;

                case 'D': /* cursor back */
                    cursor_col -= (params[0] > 0 ? params[0] : 1);
                    if (cursor_col < 0) cursor_col = 0;
                    if (cursor_visible) update_hw_cursor();
                    break;

                case 'H': case 'f': /* cursor position (1-based) */
                    cursor_row = (params[0] > 0 ? params[0] - 1 : 0);
                    cursor_col = (np >= 1 && params[1] > 0 ? params[1] - 1 : 0);
                    if (cursor_row >= max_rows) cursor_row = max_rows - 1;
                    if (cursor_col >= max_cols) cursor_col = max_cols - 1;
                    if (cursor_visible) update_hw_cursor();
                    break;

                case 'J': /* erase display */
                    if (params[0] == 2) console_clear();
                    break;

                case 'K': /* erase line to end */
                    for (int c = cursor_col; c < max_cols; ++c)
                        put_cell_raw(cursor_row, c, ' ', current_attr);
                    break;

                case 's': /* save cursor */
                    saved_row = cursor_row;
                    saved_col = cursor_col;
                    break;

                case 'u': /* restore cursor */
                    cursor_row = saved_row;
                    cursor_col = saved_col;
                    if (cursor_visible) update_hw_cursor();
                    break;

                case 'h': /* set mode */
                    if (has_qmark && params[0] == 25) console_show_cursor();
                    break;

                case 'l': /* reset mode */
                    if (has_qmark && params[0] == 25) console_hide_cursor();
                    break;

                default:
                    break;
            }
        } else {
            console_putc(*s++);
        }
    }
}

/* ================================================================
 * Init
 * ================================================================ */
void console_init(void) {
    uint16_t fill = (uint16_t)' ' | ((uint16_t)0x07 << 8);
    for (int i = 0; i < ROWS * COLS; ++i) VGA_BUF[i] = fill;
    cursor_row = 0; cursor_col = 0;
    current_attr = 0x07;
    spin_init(&inbuf_lock);
    update_hw_cursor();
}

/* ================================================================
 * Input with line editing (escape sequence support)
 *
 * Supports:
 *   - Left/Right arrows, Home/End for cursor movement
 *   - Backspace / Delete for character removal
 *   - Insert character at cursor position
 *   - Escape sequence parsing via simple state machine
 *
 * Escape sequences recognized:
 *   ESC [ A     — Up arrow (history)
 *   ESC [ B     — Down arrow (history)
 *   ESC [ C     — Right arrow
 *   ESC [ D     — Left arrow
 *   ESC [ H     — Home
 *   ESC [ F     — End
 *   ESC [ 3 ~   — Delete
 *   ESC [ 2 ~   — Insert (toggle)
 * ================================================================ */
#define INBUF_SIZE 256
static char inbuf[INBUF_SIZE];
static int  inbuf_len    = 0;   /* current length of line */
static int  inbuf_cursor = 0;   /* cursor position within line (0..inbuf_len) */
static int  line_ready   = 0;
static int  saved_line_len = 0;  /* saved length when line becomes ready */
static int  insert_mode  = 1;   /* 1 = insert, 0 = overwrite */
static spinlock_t inbuf_lock;     /* protects inbuf from SMP concurrent access */

/* Escape sequence state machine */
#define ESC_STATE_NORMAL   0
#define ESC_STATE_ESC      1
#define ESC_STATE_CSI      2
#define ESC_STATE_CSI_PARAM 3
#define ESC_STATE_CSI_TILDE 4
static int  esc_state = ESC_STATE_NORMAL;
static int  esc_params[3];
static int  esc_param_idx = 0;

/* Forward declaration for line history callback */
static void (*line_history_cb)(int direction) = 0;

/* Tab completion callback */
static void (*tab_complete_cb)(void) = 0;

/* Redraw the line from cursor to end, clearing trailing chars */
static void line_redraw_from_cursor(int old_len) {
    int saved_row, saved_col;
    console_get_cursor(&saved_row, &saved_col);
    int max_cols = g_use_fb ? (int)g_fb_cols : COLS;

    /* Write from cursor position to end of current buffer */
    for (int i = inbuf_cursor; i < inbuf_len; i++) {
        console_putc(inbuf[i]);
    }
    /* Clear any leftover characters from the old (longer) line */
    for (int i = inbuf_len; i < old_len + 1; i++) {
        console_putc(' ');
    }
    /* Restore cursor to correct position */
    int cur_row = saved_row, cur_col = saved_col;
    for (int i = inbuf_cursor; i < inbuf_len; i++) {
        cur_col++;
        if (cur_col >= max_cols) { cur_col = 0; cur_row++; }
    }
    console_set_cursor(cur_row, cur_col);
}

void console_set_history_callback(void (*cb)(int direction)) {
    line_history_cb = cb;
}

void console_set_tab_complete_callback(void (*cb)(void)) {
    tab_complete_cb = cb;
}

/*
 * Replace the entire current input line with new text.
 * Used by history navigation and tab completion.
 */
void console_replace_line(const char *new_text) {
    if (!new_text) return;

    int max_cols = g_use_fb ? (int)g_fb_cols : COLS;

    /* Clear current line visually: move cursor to beginning, clear to end */
    /* Move cursor back to start of line */
    int row, col;
    console_get_cursor(&row, &col);
    int steps = inbuf_cursor;
    for (int i = 0; i < steps; i++) {
        if (col > 0) col--;
        else if (row > 0) { row--; col = max_cols - 1; }
    }
    console_set_cursor(row, col);

    /* Clear the line visually */
    int old_len = inbuf_len;
    for (int i = 0; i < old_len + 1; i++) console_putc(' ');

    /* Reset cursor to start of line */
    console_set_cursor(row, col);

    /* Copy new text to buffer */
    inbuf_len = 0;
    for (; *new_text && inbuf_len < INBUF_SIZE - 1; new_text++) {
        inbuf[inbuf_len++] = *new_text;
        console_putc(*new_text);
    }
    inbuf[inbuf_len] = '\0';
    inbuf_cursor = inbuf_len;
}

/* Get the current input line content (for history save/restore) */
const char *console_get_current_line(void) {
    inbuf[inbuf_len] = '\0';
    return inbuf;
}

void console_input_char(char c) {
    int max_cols = g_use_fb ? (int)g_fb_cols : COLS;

    spin_lock(&inbuf_lock);

    /* --- Escape sequence state machine --- */
    if (esc_state == ESC_STATE_NORMAL && c == '\x1b') {
        esc_state = ESC_STATE_ESC;
        spin_unlock(&inbuf_lock);
        return;
    }
    if (esc_state == ESC_STATE_ESC) {
        if (c == '[') {
            esc_state = ESC_STATE_CSI;
            esc_params[0] = esc_params[1] = esc_params[2] = 0;
            esc_param_idx = 0;
            spin_unlock(&inbuf_lock);
            return;
        }
        esc_state = ESC_STATE_NORMAL;
        spin_unlock(&inbuf_lock);
        return;
    }
    if (esc_state == ESC_STATE_CSI) {
        if (c >= '0' && c <= '9') {
            esc_params[esc_param_idx] = esc_params[esc_param_idx] * 10 + (c - '0');
            spin_unlock(&inbuf_lock);
            return;
        }
        if (c == ';') {
            if (esc_param_idx < 2) esc_param_idx++;
            spin_unlock(&inbuf_lock);
            return;
        }
        /* Final character of CSI sequence */
        int p0 = esc_params[0];
        esc_state = ESC_STATE_NORMAL;

        switch (c) {
            case 'A': /* Up arrow — history */
                if (line_history_cb) line_history_cb(-1);
                spin_unlock(&inbuf_lock);
                return;
            case 'B': /* Down arrow — history */
                if (line_history_cb) line_history_cb(1);
                spin_unlock(&inbuf_lock);
                return;
            case 'C': /* Right arrow */
                if (inbuf_cursor < inbuf_len) {
                    inbuf_cursor++;
                    int row, col;
                    console_get_cursor(&row, &col);
                    col++;
                    if (col >= max_cols) { col = 0; row++; }
                    console_set_cursor(row, col);
                }
                spin_unlock(&inbuf_lock);
                return;
            case 'D': /* Left arrow */
                if (inbuf_cursor > 0) {
                    inbuf_cursor--;
                    int row, col;
                    console_get_cursor(&row, &col);
                    if (col > 0) col--;
                    else if (row > 0) { row--; col = max_cols - 1; }
                    console_set_cursor(row, col);
                }
                spin_unlock(&inbuf_lock);
                return;
            case 'H': /* Home */
                if (inbuf_cursor > 0) {
                    /* Move cursor back by inbuf_cursor positions */
                    int row, col;
                    console_get_cursor(&row, &col);
                    int steps = inbuf_cursor;
                    for (int i = 0; i < steps; i++) {
                        if (col > 0) col--;
                        else if (row > 0) { row--; col = max_cols - 1; }
                    }
                    console_set_cursor(row, col);
                    inbuf_cursor = 0;
                }
                spin_unlock(&inbuf_lock);
                return;
            case 'F': /* End */
                if (inbuf_cursor < inbuf_len) {
                    int row, col;
                    console_get_cursor(&row, &col);
                    int steps = inbuf_len - inbuf_cursor;
                    for (int i = 0; i < steps; i++) {
                        col++;
                        if (col >= max_cols) { col = 0; row++; }
                    }
                    console_set_cursor(row, col);
                    inbuf_cursor = inbuf_len;
                }
                spin_unlock(&inbuf_lock);
                return;
            case '~':
                if (p0 == 3) { /* Delete key */
                    if (inbuf_cursor < inbuf_len) {
                        int old_len = inbuf_len;
                        for (int i = inbuf_cursor; i < inbuf_len - 1; i++)
                            inbuf[i] = inbuf[i + 1];
                        inbuf_len--;
                        line_redraw_from_cursor(old_len);
                    }
                }
                spin_unlock(&inbuf_lock);
                return;
            default:
                spin_unlock(&inbuf_lock);
                return;
        }
    }

    /* --- Normal character processing --- */
    if (c == '\r' || c == '\n') {
        console_putc('\n');
        inbuf[inbuf_len] = '\0';
        saved_line_len = inbuf_len;
        line_ready = 1;
        inbuf_len = 0;
        inbuf_cursor = 0;
        spin_unlock(&inbuf_lock);
        return;
    }
    if (c == '\b') {
        if (inbuf_cursor > 0) {
            int old_len = inbuf_len;
            /* Shift characters left */
            for (int i = inbuf_cursor - 1; i < inbuf_len - 1; i++)
                inbuf[i] = inbuf[i + 1];
            inbuf_len--;
            inbuf_cursor--;
            /* Move cursor back one */
            int row, col;
            console_get_cursor(&row, &col);
            if (col > 0) col--;
            else if (row > 0) { row--; col = COLS - 1; }
            console_set_cursor(row, col);
            line_redraw_from_cursor(old_len);
        }
        spin_unlock(&inbuf_lock);
        return;
    }
    /* Tab completion: invoke callback if registered, else expand to spaces */
    if (c == '\t') {
        if (tab_complete_cb) {
            tab_complete_cb();
        } else {
            /* Default: expand tab to spaces */
            int spaces = 8 - (inbuf_cursor % 8);
            for (int i = 0; i < spaces && inbuf_len < INBUF_SIZE - 1; i++) {
                if (insert_mode && inbuf_cursor < inbuf_len) {
                    int old_len = inbuf_len;
                    for (int j = inbuf_len; j > inbuf_cursor; j--)
                        inbuf[j] = inbuf[j - 1];
                    inbuf[inbuf_cursor] = ' ';
                    inbuf_len++;
                    inbuf_cursor++;
                    console_putc(' ');
                    line_redraw_from_cursor(old_len);
                } else {
                    if (inbuf_cursor == inbuf_len) {
                        inbuf[inbuf_len++] = ' ';
                        inbuf_cursor = inbuf_len;
                        console_putc(' ');
                    } else {
                        inbuf[inbuf_cursor] = ' ';
                        inbuf_cursor++;
                        console_putc(' ');
                    }
                }
            }
        }
        spin_unlock(&inbuf_lock);
        return;
    }
    /* Ignore other control characters */
    if (c < ' ') {
        spin_unlock(&inbuf_lock);
        return;
    }

    if (inbuf_len < INBUF_SIZE - 1) {
        if (insert_mode && inbuf_cursor < inbuf_len) {
            /* Insert mode: shift right, then insert */
            int old_len = inbuf_len;
            for (int i = inbuf_len; i > inbuf_cursor; i--)
                inbuf[i] = inbuf[i - 1];
            inbuf[inbuf_cursor] = c;
            inbuf_len++;
            inbuf_cursor++;
            console_putc(c);
            line_redraw_from_cursor(old_len);
        } else {
            /* Overwrite mode or at end of line */
            if (inbuf_cursor == inbuf_len) {
                inbuf[inbuf_len++] = c;
                inbuf_cursor = inbuf_len;
                console_putc(c);
            } else {
                inbuf[inbuf_cursor] = c;
                inbuf_cursor++;
                console_putc(c);
            }
        }
    }
    spin_unlock(&inbuf_lock);
}

int console_getline(char *buf, size_t buflen) {
    spin_lock(&inbuf_lock);
    if (!line_ready) {
        spin_unlock(&inbuf_lock);
        return 0;
    }
    size_t tocopy = (size_t)saved_line_len < buflen - 1 ? (size_t)saved_line_len : buflen - 1;
    if (buf && buflen > 0) {
        memcpy(buf, inbuf, tocopy);
        buf[tocopy] = '\0';
    }
    line_ready = 0;
    spin_unlock(&inbuf_lock);
    return (int)tocopy;
}

int console_has_input(void) {
    return line_ready;
}
