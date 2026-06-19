/*
 * seccomp.h - System call access control (seccomp-like filtering)
 *
 * Provides a simple bitmap-based syscall filter.  Each task can have
 * an optional seccomp_filter that specifies which syscalls are allowed.
 * A cleared bit means the syscall is denied (returns -EPERM).
 *
 * The filter uses a 256-bit bitmap (4 × 64-bit words) to cover all
 * possible syscall numbers 0..255.
 *
 * Default behaviour: no filter = all syscalls allowed.
 */
#ifndef SECCOMP_H
#define SECCOMP_H

#include <stdint.h>

/* Forward declaration */
struct task_struct;

/* ================================================================
 * Seccomp filter
 * ================================================================ */

/*
 * seccomp_filter: 256-bit bitmap for syscall numbers 0..255.
 * Bit i corresponds to syscall number i.
 *   - Bit = 1: syscall is ALLOWED
 *   - Bit = 0: syscall is DENIED (returns -EPERM)
 */
struct seccomp_filter {
    uint64_t syscall_mask[4];  /* 4 × 64 = 256 bits */
};

/* ================================================================
 * API
 * ================================================================ */

/*
 * seccomp_set_filter: Install a syscall filter for a task.
 * @task:    Target task (must be current or a child).
 * @filter:  Filter bitmap to install (copied into kernel memory).
 * Returns:  0 on success, -1 on error (invalid arguments).
 *
 * If filter is NULL, the task's existing filter is removed (all
 * syscalls become allowed again).
 */
int seccomp_set_filter(struct task_struct *task, struct seccomp_filter *filter);

/*
 * seccomp_check: Check if a syscall number is allowed by the task's filter.
 * @task:        Task to check.
 * @syscall_num: Syscall number to test.
 * Returns:       0 if allowed, -1 if denied (caller should set EPERM).
 *
 * If the task has no filter (seccomp == NULL), all syscalls are allowed.
 */
int seccomp_check(struct task_struct *task, int syscall_num);

#endif /* SECCOMP_H */