/*
 * capability.h - File descriptor capability flags (Vision §3 foundation)
 *
 * Each file descriptor carries capability flags that restrict what
 * operations can be performed through it. This is the first step
 * toward a full capability-based security model.
 *
 * Capability flags:
 *   CAP_READ    — read() allowed
 *   CAP_WRITE   — write() allowed
 *   CAP_EXEC    — exec() allowed (for executable files)
 *   CAP_SEND    — fd can be passed to another process via IPC
 *   CAP_SEEK    — lseek() allowed
 *   CAP_IOCTL   — ioctl() allowed
 *
 * When a file is opened, capabilities are derived from the open flags
 * and can only be DOWNGRADED (never escalated). For example:
 *   open("/file", O_RDONLY) → CAP_READ only
 *   fd_derive(fd, CAP_READ) → new fd with only CAP_READ
 */
#ifndef CAPABILITY_H
#define CAPABILITY_H

#include <stdint.h>

/* Bug #13: magic number to distinguish cap_entry from raw file pointers */
#define CAP_ENTRY_MAGIC  0xCAFEFD00

/* Capability bits */
#define CAP_READ    (1U << 0)
#define CAP_WRITE   (1U << 1)
#define CAP_EXEC    (1U << 2)
#define CAP_SEND    (1U << 3)
#define CAP_SEEK    (1U << 4)
#define CAP_IOCTL   (1U << 5)

/* Common combinations */
#define CAP_RW       (CAP_READ | CAP_WRITE)
#define CAP_RWX      (CAP_READ | CAP_WRITE | CAP_EXEC)
#define CAP_FULL     (CAP_RWX | CAP_SEND | CAP_SEEK | CAP_IOCTL)

/*
 * Capability descriptor: wraps a file pointer with capability flags.
 * Stored in fd_table alongside the file pointer.
 */
struct cap_entry {
    uint32_t  magic;      /* Bug #13: CAP_ENTRY_MAGIC to detect type confusion */
    void     *file;       /* struct file * */
    uint32_t  caps;       /* CAP_* bitmask */
};

/* ============ Capability API ============ */

/*
 * fd_derive: Create a new fd with reduced capabilities.
 * @old_fd:  source fd
 * @new_caps: desired capabilities (must be subset of old_fd's caps)
 * Returns new fd, or -1 on error.
 */
int fd_derive(int old_fd, uint32_t new_caps);

/*
 * fd_send: Send an fd to another process.
 * @fd:    fd to send (must have CAP_SEND)
 * @target_pid: destination process
 * Returns 0 on success, -1 on error.
 *
 * The fd is removed from the sender and inserted into the receiver's
 * fd table with the same capabilities.
 */
int fd_send(int fd, int target_pid);

/*
 * fd_check_cap: Verify an fd has required capabilities.
 * Returns 0 if all required caps are present, -1 otherwise.
 */
int fd_check_cap(int fd, uint32_t required);

#endif /* CAPABILITY_H */
