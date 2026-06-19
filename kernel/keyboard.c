/*
 * keyboard.c - PS/2 keyboard driver with modifier keys + multi-byte scancodes (FIXED)
 *
 * Fixes:
 *   - Shift/Ctrl/Alt modifier key state tracking (Report §7.2, issue #1)
 *   - Multi-byte scancode state machine (E0 prefix, E1 prefix) (Report §7.2, issue #2)
 *   - Shift-modified character map (uppercase, symbols)
 *   - Caps Lock tracking
 *   - Ctrl key combinations (Ctrl+C → detected and logged)
 *     (Report §7.2, issue #3)
 */

#include "include/log.h"
#include "include/portio.h"
#include "include/print.h"
#include "console.h"
#include "sched.h"
#include "signal.h"
#include "perf.h"
#include <stdint.h>

/* ================================================================
 * Modifier key state
 * ================================================================ */
#define MOD_LSHIFT   0x01
#define MOD_RSHIFT   0x02
#define MOD_LCTRL    0x04
#define MOD_RCTRL    0x08
#define MOD_LALT     0x10
#define MOD_RALT     0x20
#define MOD_CAPSLOCK 0x40
#define MOD_NUMLOCK  0x80

static uint8_t modifiers = 0;
static uint8_t caps_lock_on = 0;

/* E0/E1 prefix state machine */
static int e0_prefix = 0;  /* 0=normal, 1=E0 seen, 2=E1 seen */

/* E0-prefixed key scancodes (make codes) */
#define SC_E0_UP      0x48
#define SC_E0_DOWN    0x50
#define SC_E0_LEFT    0x4B
#define SC_E0_RIGHT   0x4D
#define SC_E0_HOME    0x47
#define SC_E0_END     0x4F
#define SC_E0_PGUP    0x49
#define SC_E0_PGDN    0x51
#define SC_E0_INSERT  0x52
#define SC_E0_DELETE  0x53

/* Generate ANSI escape sequence for special keys */
static void send_ansi_seq(const char *seq) {
    for (; *seq; seq++) console_input_char(*seq);
}

/* Scancode → base character map (unshifted) */
static const char scancode_base[128] = {
      0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',  0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',  0,'\\','z','x','c','v',
    'b','n','m',',','.','/',  0,'*',  0,' ',  0,
    /* F1-F10 */
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /* numlock, scrolllock */
     0,  0,
    /* keypad 7,8,9,-,4,5,6,+,1,2,3,0,del */
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
     0,  0,  0,
    /* F11, F12 */
     0,  0,
};

/* Shifted character map — must match scancode_base size (128) */
static const char scancode_shifted[] = {
      0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',  0,'A','S',
    'D','F','G','H','J','K','L',':','"','~',  0,'|','Z','X','C','V',
    'B','N','M','<','>','?',  0,'*',  0,' ',  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
      0,  0,  0,
      0,  0,
    /* Pad to 128 entries to match scancode_base */
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

/* ================================================================
 * Modifier scancode ranges
 * ================================================================ */
#define SC_LCTRL   0x1D
#define SC_LSHIFT  0x2A
#define SC_RSHIFT  0x36
#define SC_LALT    0x38
#define SC_CAPSLOCK 0x3A
#define SC_RCTRL_E0 0x1D  /* E0 prefix */
#define SC_RALT_E0  0x38  /* E0 prefix */
#define SC_EXTENDED 0xE0

/* ================================================================
 * Keyboard interrupt handler
 * ================================================================ */

void keyboard_c_handler(void *stack) {
    (void)stack;

    /* Performance counter: count each keyboard interrupt */
    perf_inc(PERF_IRQ_COUNT);

    uint8_t sc = inb(0x60);

    /* --- Multi-byte prefix handling --- */
    if (sc == 0xE0) {
        e0_prefix = 1;
        return;
    }
    if (sc == 0xE1) {
        e0_prefix = 2;  /* Pause/Break sequence, rare */
        return;
    }

    /* --- Break code detection (bit 7 set) --- */
    int is_break = (sc & 0x80) != 0;
    uint8_t make_code = sc & 0x7F;

    /* --- Modifier key handling --- */
    if (!e0_prefix) {
        switch (make_code) {
            case SC_LSHIFT:
                if (is_break) modifiers &= ~MOD_LSHIFT;
                else          modifiers |= MOD_LSHIFT;
                goto done;
            case SC_RSHIFT:
                if (is_break) modifiers &= ~MOD_RSHIFT;
                else          modifiers |= MOD_RSHIFT;
                goto done;
            case SC_LCTRL:
                if (is_break) modifiers &= ~MOD_LCTRL;
                else          modifiers |= MOD_LCTRL;
                goto done;
            case SC_LALT:
                if (is_break) modifiers &= ~MOD_LALT;
                else          modifiers |= MOD_LALT;
                goto done;
            case SC_CAPSLOCK:
                if (!is_break) {
                    caps_lock_on ^= 1;
                }
                goto done;
        }
    } else {
        /* E0-prefixed modifier keys */
        switch (make_code) {
            case SC_RCTRL_E0:
                if (is_break) modifiers &= ~MOD_RCTRL;
                else          modifiers |= MOD_RCTRL;
                goto done;
            case SC_RALT_E0:
                if (is_break) modifiers &= ~MOD_RALT;
                else          modifiers |= MOD_RALT;
                goto done;
        }
    }

    /* --- Regular key: only process make codes --- */
    if (is_break) goto done;
    if (make_code >= 128) goto done;

    /* --- E0-prefixed special keys --- */
    if (e0_prefix == 1) {
        switch (make_code) {
            case SC_E0_UP:    send_ansi_seq("\x1b[A");  goto done;
            case SC_E0_DOWN:  send_ansi_seq("\x1b[B");  goto done;
            case SC_E0_RIGHT: send_ansi_seq("\x1b[C");  goto done;
            case SC_E0_LEFT:  send_ansi_seq("\x1b[D");  goto done;
            case SC_E0_HOME:  send_ansi_seq("\x1b[H");  goto done;
            case SC_E0_END:   send_ansi_seq("\x1b[F");  goto done;
            case SC_E0_DELETE:send_ansi_seq("\x1b[3~"); goto done;
            case SC_E0_PGUP:  /* not handled in console */  goto done;
            case SC_E0_PGDN:  /* not handled in console */  goto done;
            case SC_E0_INSERT:/* toggle insert mode */      goto done;
            default: break;
        }
    }

    /* --- Determine shift state --- */
    int shifted = (modifiers & (MOD_LSHIFT | MOD_RSHIFT)) != 0;

    /* Caps Lock inverts shift for letters */
    int is_letter = (make_code >= 16 && make_code <= 25) ||  /* Q-P */
                    (make_code >= 30 && make_code <= 38) ||  /* A-L */
                    (make_code >= 44 && make_code <= 50);    /* Z-M */
    if (is_letter && caps_lock_on) shifted = !shifted;

    /* --- Map scancode to character --- */
    char ch;
    if (shifted && make_code < sizeof(scancode_shifted)) {
        ch = scancode_shifted[make_code];
    } else if (make_code < sizeof(scancode_base)) {
        ch = scancode_base[make_code];
    } else {
        ch = 0;
    }

    if (ch == 0) goto done;  /* unmapped key */

    /* --- Ctrl combinations --- */
    if (modifiers & (MOD_LCTRL | MOD_RCTRL)) {
        /* Ctrl+letter → ASCII control character */
        if (ch >= 'a' && ch <= 'z') {
            ch = ch - 'a' + 1;   /* Ctrl+A = 1, Ctrl+C = 3, etc. */
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = ch - 'A' + 1;
        } else if (ch >= '[' && ch <= '_') {
            ch = ch - '[' + 27;  /* Ctrl+[ = ESC (27) */
        }
        /* Ctrl+C → send SIGINT to foreground process */
        if (ch == 3) {
            log_printf(LOG_LEVEL_DEBUG, "keyboard: Ctrl+C pressed\n");
            /*
             * Send SIGINT to the first user process (not the shell).
             * In this simple kernel, iterate through PIDs to find
             * a non-shell task to interrupt.
             */
            if (current) {
                /* Skip if current is the shell (PID 1) */
                int is_shell = 0;
                for (int i = 0; current->name[i] && i < 31; i++) {
                    if (current->name[i] == 's' && current->name[i+1] == 'h' &&
                        current->name[i+2] == 'e' && current->name[i+3] == 'l' &&
                        current->name[i+4] == 'l') {
                        is_shell = 1;
                        break;
                    }
                }
                if (!is_shell) {
                    do_sys_kill(current->pid, SIGINT);
                }
            }
        }
    }

    /* --- Feed character to console --- */
    console_input_char(ch);

done:
    e0_prefix = 0;  /* reset prefix state */
}

void keyboard_init(void) {
    modifiers = 0;
    caps_lock_on = 0;
    e0_prefix = 0;
    log_printf(LOG_LEVEL_INFO, "keyboard: initialized (modifier keys + E0 prefix support)\n");
}
