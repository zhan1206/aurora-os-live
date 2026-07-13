/*
 * print.c - Serial port and VGA output primitives
 */
#include "include/print.h"
#include "include/portio.h"
#include "console.h"
#include <stdint.h>

void serial_init(void) {
    outb(0x3F8 + 1, 0x00);   /* Disable interrupts */
    outb(0x3F8 + 3, 0x80);   /* Enable DLAB (divisor latch access) */
    /*
     * Baud rate = 115200 / divisor.
     * Divisor 1 → 115200 baud (fast, works well with QEMU -serial stdio).
     * For 38400: divisor = 3; for 9600: divisor = 12.
     */
    outb(0x3F8 + 0, 0x01);   /* Divisor low (115200 baud) */
    outb(0x3F8 + 1, 0x00);   /* Divisor high */
    outb(0x3F8 + 3, 0x03);   /* 8N1, no parity, 1 stop bit */
    outb(0x3F8 + 2, 0xC7);   /* Enable FIFO, clear, 14-byte threshold */
    outb(0x3F8 + 4, 0x0B);   /* DTR, RTS, OUT2 (enable IRQ if needed) */
}

static int serial_is_transmit_empty(void) {
    return inb(0x3F8 + 5) & 0x20;
}

/* Serial putc with timeout to prevent infinite hang on hardware failure */
#define SERIAL_TIMEOUT 100000
static void serial_putc(char c) {
    int timeout = SERIAL_TIMEOUT;
    while (!serial_is_transmit_empty() && --timeout > 0) {
        asm volatile ("pause" ::: "memory");
    }
    if (timeout > 0) {
        outb(0x3F8, (uint8_t)c);
    }
}

/* Flag: set to 1 after console_init() is called */
static int console_ready = 0;

void printk_console_ready(void) {
    console_ready = 1;
}

void printk(const char *s) {
    if (!s) return;
    for (int i = 0; s[i]; ++i) {
        serial_putc(s[i]);
        if (console_ready) {
            console_putc(s[i]);
        }
    }
}
