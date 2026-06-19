/*
 * capability.c - File descriptor capability system (Vision §3 foundation)
 *
 * Extends the simple fd_table with capability flags per descriptor.
 * Each fd now stores a cap_entry { file*, caps } instead of raw file*.
 *
 * Design:
 *   - fd_alloc now takes capability flags as a parameter.
 *   - fd_check_cap enforces capability checks before operations.
 *   - fd_derive creates a new fd with reduced capabilities.
 *   - fd_send transfers an fd to another process (capability passing).
 *
 * NOTE: This module is currently a forward-looking design.
 * The existing fd_table in file.c uses raw uintptr_t to store file*
 * pointers. The capability system wraps them in cap_entry structs
 * with additional permission flags.
 *
 * Migration path:
 *   Phase 1 (current): cap_fd_* functions coexist with fd_* functions.
 *   Phase 2: integrate cap_entry into fd_table natively.
 *   Phase 3: all syscall paths enforce capability checks.
 */
#include "capability.h"
#include "sched.h"
#include "fs.h"
#include "vfs.h"
#include "include/log.h"
#include "mem.h"
#include <string.h>

/* Maximum file descriptors per process — defined in sched.h as MAX_FDS */

/*
 * We store cap_entry in the task's fd_table.
 * sched.h currently has uintptr_t fd_table[MAX_FDS].
 * For backward compatibility, we reinterpret the uintptr_t as
 * storing a pointer to cap_entry (allocated via kmalloc).
 *
 * WARNING: cap_fd_* and fd_* (file.c) share the same fd_table but use
 * different pointer types. fd_alloc stores raw file* pointers, while
 * cap_fd_alloc stores cap_entry* wrappers. The two systems are NOT
 * interoperable — mixing them will cause vfs_close to be called on
 * a cap_entry* pointer, leading to memory corruption.
 *
 * Currently, only fd_* (file.c) is used by the syscall path.
 * cap_fd_* is reserved for future capability-based security model.
 *
 * Upgrade path: change fd_table to struct cap_entry fd_table[MAX_FDS]
 * when all callers are migrated. The cap_entry struct would embed the
 * file pointer and capability flags inline without separate allocation.
 */

/* ================================================================
 * fd_table with capability support
 *
 * NOTE: fd_table_init is provided by file.c (initializes all slots to -1).
 * The cap_fd_alloc below allocates a cap_entry wrapper per fd.
 * ================================================================ */

/*
 * Allocate a new fd with given capabilities.
 * Returns fd number, or -1 on failure.
 */
int cap_fd_alloc(struct task_struct *t, void *filp, uint32_t caps) {
    if (!t || !filp) return -1;

    struct cap_entry *entry = (struct cap_entry *)kmalloc(sizeof(*entry));
    if (!entry) return -1;
    entry->file = filp;
    entry->caps = caps;

    for (int i = 0; i < MAX_FDS; ++i) {
        if (t->fd_table[i] == (uintptr_t)-1) {
            t->fd_table[i] = (uintptr_t)entry;
            return i;
        }
    }

    kfree(entry);
    return -1;  /* table full */
}

/* Get the cap_entry for an fd */
static struct cap_entry *cap_get(struct task_struct *t, int fd) {
    if (!t || fd < 0 || fd >= MAX_FDS) return NULL;
    if (t->fd_table[fd] == (uintptr_t)-1) return NULL;
    return (struct cap_entry *)t->fd_table[fd];
}

/* Get the file pointer for an fd */
void *cap_fd_get_file(struct task_struct *t, int fd) {
    struct cap_entry *entry = cap_get(t, fd);
    return entry ? entry->file : NULL;
}

/* Close an fd and free the cap_entry */
int cap_fd_close(struct task_struct *t, int fd) {
    struct cap_entry *entry = cap_get(t, fd);
    if (!entry) return -1;

    vfs_close((struct file *)entry->file);
    kfree(entry);
    t->fd_table[fd] = (uintptr_t)-1;
    return 0;
}

/* Close all fds for a task */
void cap_fd_close_all(struct task_struct *t) {
    if (!t) return;
    for (int i = 0; i < MAX_FDS; ++i) {
        if (t->fd_table[i] != (uintptr_t)-1) {
            struct cap_entry *entry = (struct cap_entry *)t->fd_table[i];
            vfs_close((struct file *)entry->file);
            kfree(entry);
            t->fd_table[i] = (uintptr_t)-1;
        }
    }
}

/* ================================================================
 * Capability checks
 * ================================================================ */

int fd_check_cap(int fd, uint32_t required) {
    struct cap_entry *entry = cap_get(current, fd);
    if (!entry) return -1;
    if ((entry->caps & required) != required) {
        log_printf(LOG_LEVEL_WARN, "cap: fd %d lacks required caps 0x%x (has 0x%x)\n",
                   fd, required, entry->caps);
        return -1;
    }
    return 0;
}

/* ================================================================
 * Capability derivation and transfer
 * ================================================================ */

int fd_derive(int old_fd, uint32_t new_caps) {
    struct cap_entry *entry = cap_get(current, old_fd);
    if (!entry) return -1;

    /* New caps must be a subset of existing caps */
    if ((new_caps & ~entry->caps) != 0) {
        log_printf(LOG_LEVEL_WARN, "cap: fd_derive %d: requested 0x%x > existing 0x%x\n",
                   old_fd, new_caps, entry->caps);
        return -1;
    }

    /* Increment file refcount since both fds now share the same file */
    vfs_file_dup((struct file *)entry->file);

    /* Allocate new fd with reduced capabilities, same file pointer */
    return cap_fd_alloc(current, entry->file, new_caps);
}

int fd_send(int fd, int target_pid) {
    struct cap_entry *entry = cap_get(current, fd);
    if (!entry) return -1;

    /* Must have CAP_SEND to transfer */
    if (!(entry->caps & CAP_SEND)) {
        log_printf(LOG_LEVEL_WARN, "cap: fd_send %d: lacks CAP_SEND\n", fd);
        return -1;
    }

    struct task_struct *target = find_task_by_pid(target_pid);
    if (!target) return -1;

    /* Allocate fd in target with same capabilities.
     * Since this is a transfer (not a copy), the file's refcount
     * doesn't change — ownership moves from sender to receiver.
     */
    uint32_t saved_caps = entry->caps;
    int new_fd = cap_fd_alloc(target, entry->file, entry->caps);
    if (new_fd < 0) return -1;

    /* Remove from sender (transfer, not copy) */
    kfree(entry);
    current->fd_table[fd] = (uintptr_t)-1;

    log_printf(LOG_LEVEL_DEBUG, "cap: sent fd %d -> pid %d as fd %d (caps=0x%x)\n",
               fd, target_pid, new_fd, saved_caps);
    return 0;
}
