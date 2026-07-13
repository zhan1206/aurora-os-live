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
 *
 * FIXED (v4.0.6):
 *   - Added seccomp_lock spinlock to prevent UAF race between
 *     seccomp_set_filter() and seccomp_check() on SMP systems.
 *   - seccomp_check() now holds the lock while dereferencing
 *     the filter pointer, preventing concurrent kfree().
 */

#include "seccomp.h"
#include "sched.h"
#include "smp.h"
#include "mem.h"
#include "include/log.h"
#include "include/errno.h"
#include <stdint.h>
#include <string.h>

/* ================================================================
 * seccomp_set_filter
 *
 * Atomically installs or removes a seccomp filter for the given
 * task. Uses seccomp_lock to prevent races with seccomp_check().
 * ================================================================ */

int seccomp_set_filter(struct task_struct *task, struct seccomp_filter *filter) {
    if (!task) return -1;

    spin_lock((spinlock_t*)&task->seccomp_lock);

    /* If filter is NULL, remove the existing filter */
    if (!filter) {
        struct seccomp_filter *old = task->seccomp;
        task->seccomp = NULL;
        spin_unlock((spinlock_t*)&task->seccomp_lock);

        if (old) {
            kfree(old);
            log_printf(LOG_LEVEL_INFO, "seccomp: filter removed for pid=%d\n",
                       task->pid);
        }
        return 0;
    }

    /* Allocate and copy the new filter */
    struct seccomp_filter *new_filter = kmalloc(sizeof(struct seccomp_filter));
    if (!new_filter) {
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        return -1;
    }

    memcpy(new_filter, filter, sizeof(struct seccomp_filter));

    /* Atomically swap in the new filter under the lock */
    struct seccomp_filter *old = task->seccomp;
    task->seccomp = new_filter;
    spin_unlock((spinlock_t*)&task->seccomp_lock);

    if (old) kfree(old);

    log_printf(LOG_LEVEL_INFO, "seccomp: filter installed for pid=%d\n",
               task->pid);

    return 0;
}

/* ================================================================
 * seccomp_check
 *
 * Check if a syscall is allowed by the task's seccomp filter.
 * Holds seccomp_lock while dereferencing the filter pointer
 * to prevent UAF with concurrent seccomp_set_filter(NULL).
 * ================================================================ */

int seccomp_check(struct task_struct *task, int syscall_num) {
    if (!task) return 0;  /* safety: allow if no task context */

    spin_lock((spinlock_t*)&task->seccomp_lock);

    /* No filter installed: all syscalls allowed */
    struct seccomp_filter *filter = task->seccomp;
    if (!filter) {
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        return 0;
    }

    /* Bounds check: syscall numbers outside 0..255 are always denied */
    if (syscall_num < 0 || syscall_num >= 256) {
        spin_unlock((spinlock_t*)&task->seccomp_lock);
        return -1;
    }

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

    int result = (filter->syscall_mask[word_idx] & mask) ? 0 : -1;

    spin_unlock((spinlock_t*)&task->seccomp_lock);
    return result;
}