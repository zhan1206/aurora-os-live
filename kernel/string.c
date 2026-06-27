/*
* string.c - Freestanding string/memory functions
*/
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t remaining = n;

    /* Copy 8 bytes at a time when both src and dst are 8-byte aligned */
    if ((((uintptr_t)d & 7) == 0) && (((uintptr_t)s & 7) == 0)) {
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        while (remaining >= 8) {
            *d64++ = *s64++;
            remaining -= 8;
        }
        d = (uint8_t *)d64;
        s = (const uint8_t *)s64;
    }

    /* Copy remaining bytes */
    for (size_t i = 0; i < remaining; ++i)
        d[i] = s[i];
    return dst;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    size_t remaining = n;
    uint8_t byte_val = (uint8_t)c;

    /* Fill 8 bytes at a time when aligned */
    if (((uintptr_t)p & 7) == 0) {
        uint64_t val64 = ((uint64_t)byte_val << 56) | ((uint64_t)byte_val << 48) |
                         ((uint64_t)byte_val << 40) | ((uint64_t)byte_val << 32) |
                         ((uint64_t)byte_val << 24) | ((uint64_t)byte_val << 16) |
                         ((uint64_t)byte_val << 8)  | (uint64_t)byte_val;
        uint64_t *p64 = (uint64_t *)p;
        while (remaining >= 8) {
            *p64++ = val64;
            remaining -= 8;
        }
        p = (uint8_t *)p64;
    }

    /* Fill remaining bytes */
    for (size_t i = 0; i < remaining; ++i)
        p[i] = byte_val;
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    if (!s) return 0;
    /* Aligned 8-byte fast path: check one word at a time for null byte */
    const char *p = s;
    while (((uintptr_t)p & 7) != 0) {
        if (!*p) return (size_t)(p - s);
        ++p;
    }
    const uint64_t *w = (const uint64_t *)p;
    /* 0x8080808080808080: high bit of each byte; ~0/256: all-ones */
    const uint64_t himagic = 0x8080808080808080ULL;
    const uint64_t lomagic = 0x0101010101010101ULL;
    for (;;) {
        uint64_t v = *w++;
        /* Check if any byte is zero: (v - 0x01) & ~v & 0x80 */
        if ((v - lomagic) & ~v & himagic) break;
    }
    /* Find the exact null byte */
    p = (const char *)(w - 1);
    while (*p) ++p;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    /* Aligned 8-byte fast path */
    while (((uintptr_t)a & 7) != 0 && ((uintptr_t)b & 7) != 0) {
        if (!*a || *a != *b) return (unsigned char)*a - (unsigned char)*b;
        ++a; ++b;
    }
    if (((uintptr_t)a & 7) == 0 && ((uintptr_t)b & 7) == 0) {
        const uint64_t *wa = (const uint64_t *)a;
        const uint64_t *wb = (const uint64_t *)b;
        const uint64_t himagic = 0x8080808080808080ULL;
        const uint64_t lomagic = 0x0101010101010101ULL;
        for (;;) {
            uint64_t va = *wa, vb = *wb;
            if (va != vb) {
                a = (const char *)wa; b = (const char *)wb;
                break;
            }
            if ((va - lomagic) & ~va & himagic) {
                /* Both strings ended at same position */
                return 0;
            }
            ++wa; ++wb;
        }
    }
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    if (!dst || !src) return dst;
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    if (!dst || !src) return dst;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; ++i)
        dst[i] = src[i];
    for (; i < n; ++i)
        dst[i] = '\0';
    return dst;
}

/* Simple snprintf — supports %s, %d, %u, %x, %p, %c */
#include <stdarg.h>

static int sn_write_char(char *buf, size_t size, size_t *pos, char c) {
    if (*pos + 1 < size) {
        buf[*pos] = c;
    }
    (*pos)++;
    return 1;
}

static int sn_write_str(char *buf, size_t size, size_t *pos, const char *s) {
    if (!s) s = "(null)";
    int written = 0;
    while (*s) {
        sn_write_char(buf, size, pos, *s++);
        written++;
    }
    return written;
}

static int sn_write_uint(char *buf, size_t size, size_t *pos, uint64_t val, int base) {
    char tmp[21];
    int tn = 0;
    if (val == 0) tmp[tn++] = '0';
    while (val > 0 && tn < 20) {
        int digit = (int)(val % (uint64_t)base);
        tmp[tn++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        val /= (uint64_t)base;
    }
    int written = 0;
    for (int i = tn - 1; i >= 0; i--) {
        sn_write_char(buf, size, pos, tmp[i]);
        written++;
    }
    return written;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    if (!buf || size == 0) return 0;
    if (!fmt) {
        buf[0] = '\0';
        return 0;
    }

    va_list ap;
    va_start(ap, fmt);
    size_t pos = 0;
    const char *p = fmt;

    while (*p && pos < size) {
        if (*p != '%') {
            sn_write_char(buf, size, &pos, *p++);
            continue;
        }
        p++; /* skip '%' */

        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            sn_write_str(buf, size, &pos, s);
            p++;
        } else if (*p == 'd') {
            int val = va_arg(ap, int);
            if (val < 0) {
                sn_write_char(buf, size, &pos, '-');
                sn_write_uint(buf, size, &pos, (uint64_t)(-(val + 1)) + 1, 10);
            } else {
                sn_write_uint(buf, size, &pos, (uint64_t)val, 10);
            }
            p++;
        } else if (*p == 'u') {
            uint64_t val = va_arg(ap, uint64_t);
            sn_write_uint(buf, size, &pos, val, 10);
            p++;
        } else if (*p == 'x') {
            uint64_t val = va_arg(ap, uint64_t);
            sn_write_uint(buf, size, &pos, val, 16);
            p++;
        } else if (*p == 'p') {
            uint64_t val = (uint64_t)(uintptr_t)va_arg(ap, void *);
            if (val == 0) {
                sn_write_str(buf, size, &pos, "(nil)");
            } else {
                sn_write_str(buf, size, &pos, "0x");
                sn_write_uint(buf, size, &pos, val, 16);
            }
            p++;
        } else if (*p == 'c') {
            char c = (char)va_arg(ap, int);
            sn_write_char(buf, size, &pos, c);
            p++;
        } else if (*p == '%') {
            sn_write_char(buf, size, &pos, '%');
            p++;
        } else {
            /* Unknown format specifier — print as-is */
            sn_write_char(buf, size, &pos, '%');
            if (*p) sn_write_char(buf, size, &pos, *p++);
        }
    }

    buf[pos < size ? pos : size - 1] = '\0';
    va_end(ap);
    return (int)pos;
}
