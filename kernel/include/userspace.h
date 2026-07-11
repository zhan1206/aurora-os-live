/*
 * userspace.h - Safe user/kernel data copy interface
 *
 * Provides copy_from_user / copy_to_user for validated data transfer
 * between user space (0x0 ~ 0x7FFFFFFFFFFF) and kernel space.
 *
 * Validates both address range and page table mappings to prevent
 * kernel panics from accessing unmapped user pages. Returns 0 on
 * success, -EFAULT if any byte is in kernel space or unmapped.
 */
#ifndef USERSPACE_H
#define USERSPACE_H

#include <stdint.h>
#include <stddef.h>

/* User address space bounds (canonical lower half) */
#define USER_ADDR_MIN  0x0000000000000000ULL
#define USER_ADDR_MAX  0x00007FFFFFFFFFFFULL

/* Forward declarations */
#include <string.h>
#include "../pagetable.h"

/* Validate that a buffer [addr, addr+size) is entirely in user space */
static inline int user_addr_range_ok(const void *addr, size_t size) {
    uint64_t start = (uint64_t)(uintptr_t)addr;
    uint64_t end   = start + size;

    /* Check for wraparound */
    if (end < start) return 0;

    /* Check bounds (USER_ADDR_MIN is 0, but keep check for future changes) */
    if (end > USER_ADDR_MAX) return 0;

    return 1;
}

/*
 * Validate that all pages in the range [addr, addr+size) are mapped
 * in the current page table. This prevents kernel panics from
 * copy_from_user/copy_to_user accessing unmapped user addresses.
 */
static inline int user_pages_mapped(const void *addr, size_t size) {
    uint64_t page = (uint64_t)(uintptr_t)addr & ~0xFFFULL;
    uint64_t end  = (uint64_t)(uintptr_t)addr + size;
    for (; page < end; page += 0x1000) {
        if (!user_page_present(page)) return 0;
    }
    return 1;
}

/*
 * copy_from_user: Copy data from user space to kernel buffer.
 * @dst:  kernel buffer (must be valid kernel memory).
 * @src:  user space pointer (validated).
 * @size: number of bytes to copy.
 * Returns 0 on success, -EFAULT if src range is invalid or unmapped.
 */
static inline int copy_from_user(void *dst, const void *src, size_t size) {
    if (!dst || !src) return -1;
    if (!user_addr_range_ok(src, size)) return -1;
    if (!user_pages_mapped(src, size)) return -1;
    stac();
    memcpy(dst, src, size);
    clac();
    return 0;
}

/*
 * copy_to_user: Copy data from kernel buffer to user space.
 * @dst:  user space pointer (validated).
 * @src:  kernel buffer.
 * @size: number of bytes to copy.
 * Returns 0 on success, -EFAULT if dst range is invalid or unmapped.
 */
static inline int copy_to_user(void *dst, const void *src, size_t size) {
    if (!dst || !src) return -1;
    if (!user_addr_range_ok(dst, size)) return -1;
    if (!user_pages_mapped(dst, size)) return -1;
    stac();
    memcpy(dst, src, size);
    clac();
    return 0;
}

/*
 * strncpy_from_user: Copy a null-terminated string from user space.
 * @dst:  kernel buffer.
 * @src:  user space string pointer.
 * @max:  maximum bytes to copy (including null terminator).
 * Returns: number of bytes copied (including null), or -EFAULT.
 */
static inline int strncpy_from_user(char *dst, const char *src, size_t max) {
    if (!dst || !src || max == 0) return -1;
    if (!user_addr_range_ok(src, max)) return -1;
    if (!user_pages_mapped(src, max)) return -1;

    size_t i;
    stac();
    for (i = 0; i < max - 1; ++i) {
        dst[i] = src[i];
        if (src[i] == '\0') break;
    }
    clac();
    dst[i] = '\0';
    return (int)(i + 1);
}

#endif /* USERSPACE_H */
