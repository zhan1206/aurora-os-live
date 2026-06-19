/*
 * seccomp.c - System call filtering implementation
 *
 * Provides a simple bitmap-based syscall filter for per-task
 * access control.  Each task can have an optional seccomp_filter
 * that specifies which syscalls are allowed.
 *
 * The filter is a 256-bit bitmap (4 × 64-bit words).  Bit i
 * set to 1 means syscall i is allowed.  Bit i set to 0 means
 * syscall i is denied.
 *
 * Default: no filter (NULL) = all syscalls allowed.
 */
#include "seccomp.h"
#include "sched.h"
#include "mem.h"
#include "include/log.h"
#include "include/errno.h"
#include <stdint.h>
#include <string.h>

/* ================================================================
 * seccomp_set_filter
 * ================================================================ */

int seccomp_set_filter(struct task_struct *task, struct seccomp_filter *filter) {
    if (!task) return -1;

    /* Free existing filter if any */
    if (task->seccomp) {
        kfree(task->seccomp);
        task->seccomp = NULL;
    }

    /* If filter is NULL, just remove the existing filter */
    if (!filter) {
        log_printf(LOG_LEVEL_INFO, "seccomp: filter removed for pid=%d\n",
                   task->pid);
        return 0;
    }

    /* Allocate and copy the new filter */
    struct seccomp_filter *new_filter = kmalloc(sizeof(struct seccomp_filter));
    if (!new_filter) return -1;

    memcpy(new_filter, filter, sizeof(struct seccomp_filter));
    task->seccomp = new_filter;

    log_printf(LOG_LEVEL_INFO, "seccomp: filter installed for pid=%d (mask=%p %p %p %p)\n",
               task->pid,
               (void *)(uintptr_t)new_filter->syscall_mask[0],
               (void *)(uintptr_t)new_filter->syscall_mask[1],
               (void *)(uintptr_t)new_filter->syscall_mask[2],
               (void *)(uintptr_t)new_filter->syscall_mask[3]);

    return 0;
}

/* ================================================================
 * seccomp_check
 * ================================================================ */

int seccomp_check(struct task_struct *task, int syscall_num) {
    if (!task) return 0;  /* safety: allow if no task context */

    /* No filter installed: all syscalls allowed */
    if (!task->seccomp) return 0;

    /* Bounds check: syscall numbers outside 0..255 are always denied */
    if (syscall_num < 0 || syscall_num >= 256) return -1;

    /*
     * Check the bitmap: each uint64_t covers 64 syscalls.
     * syscall_mask[0] covers syscalls 0..63
     * syscall_mask[1] covers syscalls 64..127
     * syscall_mask[2] covers syscalls 128..191
     * syscall_mask[3] covers syscalls 192..255
     */
    int word_idx = syscall_num / 64;
    int bit_idx  = syscall_num % 64;
    uint64_t mask = 1ULL << bit_idx;

    if (task->seccomp->syscall_mask[word_idx] & mask) {
        return 0;  /* bit is set: syscall allowed */
    }

    return -1;  /* bit is clear: syscall denied */
}