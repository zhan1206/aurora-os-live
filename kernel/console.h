/*
 * console.h - VGA text mode console with ANSI escape support
 *
 * Also supports framebuffer-based console when booted via UEFI GOP.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#define ROWS 25
#define COLS 80

/* VGA 16-color palette */
enum vga_color {
    VGA_BLACK        = 0,
    VGA_BLUE         = 1,
    VGA_GREEN        = 2,
    VGA_CYAN         = 3,
    VGA_RED          = 4,
    VGA_MAGENTA      = 5,
    VGA_BROWN        = 6,
    VGA_LIGHT_GREY   = 7,
    VGA_DARK_GREY    = 8,
    VGA_LIGHT_BLUE   = 9,
    VGA_LIGHT_GREEN  = 10,
    VGA_LIGHT_CYAN   = 11,
    VGA_LIGHT_RED    = 12,
    VGA_LIGHT_MAGENTA= 13,
    VGA_YELLOW       = 14,
    VGA_WHITE        = 15,
};

/* ANSI→VGA color mapping for 16 standard colors */
#define ANSI_BLACK   0
#define ANSI_RED     1
#define ANSI_GREEN   2
#define ANSI_YELLOW  3
#define ANSI_BLUE    4
#define ANSI_MAGENTA 5
#define ANSI_CYAN    6
#define ANSI_WHITE   7

/* Initialize VGA text mode console */
void console_init(void);

/* Initialize framebuffer-based console (for UEFI GOP boot) */
void console_init_fb(uint64_t fb_addr, uint32_t width, uint32_t height,
                     uint32_t pitch, uint32_t bpp);

void console_putc(char c);
void console_write(const char *s);

/* Raw cell write with explicit VGA attribute */
void console_putc_attr(int row, int col, char c, uint8_t attr);

/* Write string with ANSI escape sequence parsing */
void console_write_ansi(const char *s);

/* Cursor control */
void console_set_cursor(int row, int col);
void console_get_cursor(int *row, int *col);
void console_hide_cursor(void);
void console_show_cursor(void);

/* Screen control */
void console_clear(void);
void console_clear_to_end(void);
void console_set_fg(uint8_t color);
void console_set_bg(uint8_t color);
void console_reset_attr(void);

/* Get current VGA attribute */
uint8_t console_get_attr(void);

/* Input */
void console_input_char(char c);
int  console_getline(char *buf, size_t buflen);
void console_set_history_callback(void (*cb)(int direction));
void console_set_tab_complete_callback(void (*cb)(void));
void console_replace_line(const char *new_text);
const char *console_get_current_line(void);

#endif
