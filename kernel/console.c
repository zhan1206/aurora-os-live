/*
 * console.c - VGA text mode console with ANSI escape support
 *
 * Uses theme.h design tokens for default visual attributes.
 */
#include "console.h"
#include "include/log.h"
#include "include/portio.h"
#include "include/theme.h"
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
 * Hardware cursor
 * ================================================================ */
static void update_hw_cursor(void) {
    uint16_t pos = (uint16_t)(cursor_row * COLS + cursor_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* ================================================================
 * Cell operations
 * ================================================================ */
static void put_cell_raw(int r, int c, char ch, uint8_t attr) {
    int idx = r * COLS + c;
    VGA_BUF[idx] = (uint16_t)ch | ((uint16_t)attr << 8);
}

void console_putc_attr(int row, int col, char c, uint8_t attr) {
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return;
    put_cell_raw(row, col, c, attr);
}

static void scroll_up(void) {
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
    if (row < 0) row = 0;
    if (row >= ROWS) row = ROWS - 1;
    if (col < 0) col = 0;
    if (col >= COLS) col = COLS - 1;
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
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

void console_show_cursor(void) {
    cursor_visible = 1;
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00);
    update_hw_cursor();
}

/* ================================================================
 * Screen control
 * ================================================================ */
void console_clear(void) {
    /* Optimized: use bulk memset-like fill instead of nested loops */
    uint16_t fill = (uint16_t)' ' | ((uint16_t)current_attr << 8);
    for (int i = 0; i < ROWS * COLS; ++i)
        VGA_BUF[i] = fill;
    cursor_row = 0; cursor_col = 0;
    update_hw_cursor();
}

void console_clear_to_end(void) {
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
    if (c == '\n') {
        cursor_col = 0; cursor_row++;
        if (cursor_row >= ROWS) { scroll_up(); cursor_row = ROWS-1; }
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
    if (cursor_col >= COLS) {
        cursor_col = 0; cursor_row++;
        if (cursor_row >= ROWS) { scroll_up(); cursor_row = ROWS-1; }
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
                    if (cursor_row >= ROWS) cursor_row = ROWS - 1;
                    if (cursor_visible) update_hw_cursor();
                    break;

                case 'C': /* cursor forward */
                    cursor_col += (params[0] > 0 ? params[0] : 1);
                    if (cursor_col >= COLS) cursor_col = COLS - 1;
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
                    if (cursor_row >= ROWS) cursor_row = ROWS - 1;
                    if (cursor_col >= COLS) cursor_col = COLS - 1;
                    if (cursor_visible) update_hw_cursor();
                    break;

                case 'J': /* erase display */
                    if (params[0] == 2) console_clear();
                    break;

                case 'K': /* erase line to end */
                    for (int c = cursor_col; c < COLS; ++c)
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
        if (cur_col >= COLS) { cur_col = 0; cur_row++; }
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

    /* Clear current line visually: move cursor to beginning, clear to end */
    /* Move cursor back to start of line */
    int row, col;
    console_get_cursor(&row, &col);
    int steps = inbuf_cursor;
    for (int i = 0; i < steps; i++) {
        if (col > 0) col--;
        else if (row > 0) { row--; col = COLS - 1; }
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
    /* --- Escape sequence state machine --- */
    if (esc_state == ESC_STATE_NORMAL && c == '\x1b') {
        esc_state = ESC_STATE_ESC;
        return;
    }
    if (esc_state == ESC_STATE_ESC) {
        if (c == '[') {
            esc_state = ESC_STATE_CSI;
            esc_params[0] = esc_params[1] = esc_params[2] = 0;
            esc_param_idx = 0;
            return;
        }
        esc_state = ESC_STATE_NORMAL;
        return;
    }
    if (esc_state == ESC_STATE_CSI) {
        if (c >= '0' && c <= '9') {
            esc_params[esc_param_idx] = esc_params[esc_param_idx] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (esc_param_idx < 2) esc_param_idx++;
            return;
        }
        /* Final character of CSI sequence */
        int p0 = esc_params[0];
        esc_state = ESC_STATE_NORMAL;

        switch (c) {
            case 'A': /* Up arrow — history */
                if (line_history_cb) line_history_cb(-1);
                return;
            case 'B': /* Down arrow — history */
                if (line_history_cb) line_history_cb(1);
                return;
            case 'C': /* Right arrow */
                if (inbuf_cursor < inbuf_len) {
                    inbuf_cursor++;
                    int row, col;
                    console_get_cursor(&row, &col);
                    col++;
                    if (col >= COLS) { col = 0; row++; }
                    console_set_cursor(row, col);
                }
                return;
            case 'D': /* Left arrow */
                if (inbuf_cursor > 0) {
                    inbuf_cursor--;
                    int row, col;
                    console_get_cursor(&row, &col);
                    if (col > 0) col--;
                    else if (row > 0) { row--; col = COLS - 1; }
                    console_set_cursor(row, col);
                }
                return;
            case 'H': /* Home */
                if (inbuf_cursor > 0) {
                    /* Move cursor back by inbuf_cursor positions */
                    int row, col;
                    console_get_cursor(&row, &col);
                    int steps = inbuf_cursor;
                    for (int i = 0; i < steps; i++) {
                        if (col > 0) col--;
                        else if (row > 0) { row--; col = COLS - 1; }
                    }
                    console_set_cursor(row, col);
                    inbuf_cursor = 0;
                }
                return;
            case 'F': /* End */
                if (inbuf_cursor < inbuf_len) {
                    int row, col;
                    console_get_cursor(&row, &col);
                    int steps = inbuf_len - inbuf_cursor;
                    for (int i = 0; i < steps; i++) {
                        col++;
                        if (col >= COLS) { col = 0; row++; }
                    }
                    console_set_cursor(row, col);
                    inbuf_cursor = inbuf_len;
                }
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
                return;
            default:
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
        return;
    }
    /* Ignore other control characters */
    if (c < ' ') return;

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
}

int console_getline(char *buf, size_t buflen) {
    if (!line_ready) return 0;
    size_t tocopy = (size_t)saved_line_len < buflen - 1 ? (size_t)saved_line_len : buflen - 1;
    if (buf && buflen > 0) {
        memcpy(buf, inbuf, tocopy);
        buf[tocopy] = '\0';
    }
    line_ready = 0;
    return (int)tocopy;
}
