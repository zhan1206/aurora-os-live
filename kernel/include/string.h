/*
 * string.h - Freestanding string/memory function declarations
 * For -ffreestanding kernel builds (no libc).
 */
#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);

/* snprintf: simple formatted output to buffer (supports %s, %d, %u, %x, %p) */
int snprintf(char *buf, size_t size, const char *fmt, ...);

#endif /* KERNEL_STRING_H */
