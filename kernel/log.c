/*
 * log.c - Kernel logging with ring buffer (FIXED)
 *
 * Fixes applied:
 *   - Added ring buffer for persistent log storage (Design-v0.2 §1.3)
 *   - Fixed panic() va_list misuse: first log_vprintf no longer uses
 *     the caller's va_list for the "PANIC: " prefix.
 *     (Report §10.2, issue #4)
 */
#include "include/log.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Low-level output */
extern void printk(const char *s);

/* ================================================================
 * Ring buffer for kernel log
 * ================================================================ */
#define LOG_BUF_SIZE 4096
static char log_ring[LOG_BUF_SIZE];
static unsigned log_wpos = 0;  /* write position */

static void log_ring_write(const char *s, int len) {
    for (int i = 0; i < len; ++i) {
        log_ring[log_wpos % LOG_BUF_SIZE] = s[i];
        log_wpos++;
    }
    /* Ensure null termination at current position */
    log_ring[log_wpos % LOG_BUF_SIZE] = '\0';
}

/* Read the ring buffer from oldest to newest */
void log_ring_dump(void (*emit)(char c)) {
    if (!emit) return;
    unsigned start = (log_wpos >= LOG_BUF_SIZE) ? log_wpos % LOG_BUF_SIZE : 0;
    unsigned end   = log_wpos % LOG_BUF_SIZE;

    if (log_wpos < LOG_BUF_SIZE) {
        /* Buffer hasn't wrapped */
        for (unsigned i = 0; i < log_wpos; ++i)
            emit(log_ring[i]);
    } else {
        /* Buffer has wrapped: print [start..end) then [0..end) */
        for (unsigned i = start; i < LOG_BUF_SIZE; ++i)
            emit(log_ring[i]);
        for (unsigned i = 0; i < end; ++i)
            emit(log_ring[i]);
    }
}

/*
 * log_ring_read: Copy log ring buffer contents into a user buffer.
 * Returns the number of bytes copied (not including null terminator).
 * Useful for /proc/kmsg and other consumer interfaces.
 */
int log_ring_read(char *buf, size_t size) {
    if (!buf || size < 2) return 0;
    int len = 0;
    unsigned total = (log_wpos < LOG_BUF_SIZE) ? log_wpos : LOG_BUF_SIZE;
    unsigned start = (log_wpos >= LOG_BUF_SIZE) ? log_wpos % LOG_BUF_SIZE : 0;

    for (unsigned i = 0; i < total && len < (int)size - 1; i++) {
        unsigned idx = (start + i) % LOG_BUF_SIZE;
        buf[len++] = log_ring[idx];
    }
    buf[len] = '\0';
    return len;
}

/* ================================================================
 * Log level
 * ================================================================ */
static int current_level = LOG_LEVEL_INFO;

void log_set_level(int level) {
    current_level = level;
}

/* ================================================================
 * Formatted log output
 * ================================================================ */

void log_vprintf(int level, const char *fmt, va_list ap) {
    if (level > current_level) return;

    char buf[512];
    int n = 0;
    const char *p = fmt;

    while (*p && n < (int)sizeof(buf) - 1) {
        if (*p == '%') {
            p++;
            /* Handle length modifiers: hh, h, l, ll */
            int long_mod = 0;  /* 0=none, 1=l, 2=ll */
            if (*p == 'l') { p++; long_mod = 1; }
            if (*p == 'l') { p++; long_mod = 2; }
            if (*p == 's') {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && n < (int)sizeof(buf) - 1) buf[n++] = *s++;
            } else if (*p == 'c') {
                char c = (char)va_arg(ap, int);
                buf[n++] = c;
            } else if (*p == 'd') {
                int64_t v;
                if (long_mod == 2)      v = va_arg(ap, int64_t);
                else if (long_mod == 1) v = va_arg(ap, long);
                else                    v = va_arg(ap, int);
                if (v == 0) { buf[n++] = '0'; }
                else {
                    char tmp[32]; int tn = 0; int neg = v < 0;
                    /* Avoid undefined behavior when v == INT64_MIN:
                     * -v would overflow, so use unsigned arithmetic */
                    uint64_t uv = neg ? (uint64_t)(-(v + 1)) + 1ULL : (uint64_t)v;
                    while (uv && tn < (int)sizeof(tmp)) { tmp[tn++] = '0' + (uv % 10); uv /= 10; }
                    if (neg) tmp[tn++] = '-';
                    for (int i = tn - 1; i >= 0; --i) buf[n++] = tmp[i];
                }
            } else if (*p == 'u') {
                uint64_t v;
                if (long_mod == 2)      v = va_arg(ap, uint64_t);
                else if (long_mod == 1) v = va_arg(ap, unsigned long);
                else                    v = va_arg(ap, unsigned int);
                char tmp[32]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                while (v && tn < (int)sizeof(tmp)) {
                    tmp[tn++] = '0' + (v % 10);
                    v /= 10;
                }
                for (int i = tn - 1; i >= 0; --i) buf[n++] = tmp[i];
            } else if (*p == 'x' || *p == 'p') {
                uint64_t v;
                if (long_mod == 2)      v = va_arg(ap, uint64_t);
                else if (long_mod == 1) v = va_arg(ap, unsigned long);
                else                    v = va_arg(ap, unsigned long);
                char tmp[32]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                while (v && tn < (int)sizeof(tmp)) {
                    const char *hex = "0123456789abcdef";
                    tmp[tn++] = hex[v & 0xF]; v >>= 4;
                }
                if (*p == 'p') { buf[n++] = '0'; buf[n++] = 'x'; }
                for (int i = tn - 1; i >= 0; --i) buf[n++] = tmp[i];
            } else {
                buf[n++] = '%';
                if (*p) buf[n++] = *p;
            }
            if (*p) p++;
        } else {
            buf[n++] = *p++;
        }
    }
    buf[n] = '\0';

    /* Write to ring buffer */
    log_ring_write(buf, n);

    /* Write to serial/VGA */
    printk(buf);
}

void log_printf(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(level, fmt, ap);
    va_end(ap);
}
