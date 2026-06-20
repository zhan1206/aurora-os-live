/*
 * file.c - File descriptor table management (FIXED)
 *
 * Fixes applied:
 *   - fd_table now uses uintptr_t (not int), eliminating 64→32 bit
 *     pointer truncation. (Report §8.2, issue #1 in §4.1)
 *   - fd_close_all: close all fds on process exit.
 */
#include "fs.h"
#include "vfs.h"
#include "sched.h"
#include "include/log.h"
#include <string.h>

void fd_table_init(struct task_struct *t) {
    for (int i = 0; i < MAX_FDS; ++i)
        t->fd_table[i] = (uintptr_t)-1;  /* all bits set = unused */
}

int fd_alloc(struct task_struct *t, void *filp) {
    if (!t || !filp) return -1;
    for (int i = 0; i < MAX_FDS; ++i) {
        if (t->fd_table[i] == (uintptr_t)-1) {
            t->fd_table[i] = (uintptr_t)filp;
            return i;
        }
    }
    return -1; /* table full */
}

void *fd_get(struct task_struct *t, int fd) {
    if (!t) return NULL;
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    if (t->fd_table[fd] == (uintptr_t)-1) return NULL;
    return (void *)t->fd_table[fd];
}

int fd_close(struct task_struct *t, int fd) {
    void *f = fd_get(t, fd);
    if (!f) return -1;
    vfs_close((struct file *)f);
    t->fd_table[fd] = (uintptr_t)-1;
    return 0;
}

int fd_open_path(struct task_struct *t, const char *path) {
    struct file *f = vfs_open(path, 0);
    if (!f) return -1;
    int fd = fd_alloc(t, f);
    if (fd < 0) { vfs_close(f); return -1; }
    return fd;
}

ssize_t fd_read_fd(struct task_struct *t, int fd, void *buf, size_t count) {
    struct file *f = (struct file *)fd_get(t, fd);
    if (!f) return -1;
    return vfs_read(f, buf, count);
}

ssize_t fd_write_fd(struct task_struct *t, int fd, const void *buf, size_t count) {
    struct file *f = (struct file *)fd_get(t, fd);
    if (!f) return -1;
    return vfs_write(f, buf, count);
}

void fd_close_all(struct task_struct *t) {
    if (!t) return;
    for (int i = 0; i < MAX_FDS; ++i) {
        if (t->fd_table[i] != (uintptr_t)-1) {
            vfs_close((struct file *)t->fd_table[i]);
            t->fd_table[i] = (uintptr_t)-1;
        }
    }
}

/*
 * fd_dup: Duplicate a file descriptor (like POSIX dup()).
 * Returns the new fd, or -1 on error.
 */
int fd_dup(struct task_struct *t, int oldfd) {
    if (!t) return -1;
    void *f = fd_get(t, oldfd);
    if (!f) return -1;
    /* Increment file refcount since both fds share the same file */
    vfs_file_dup((struct file *)f);
    return fd_alloc(t, f);
}

/*
 * fd_dup2: Duplicate oldfd to newfd (like POSIX dup2()).
 * If newfd is already open, it is silently closed first.
 * Returns newfd on success, -1 on error.
 */
int fd_dup2(struct task_struct *t, int oldfd, int newfd) {
    if (!t) return -1;
    if (oldfd < 0 || oldfd >= MAX_FDS) return -1;
    if (newfd < 0 || newfd >= MAX_FDS) return -1;
    if (oldfd == newfd) return newfd;

    void *f = fd_get(t, oldfd);
    if (!f) return -1;

    /* Close newfd if already open */
    if (t->fd_table[newfd] != (uintptr_t)-1) {
        fd_close(t, newfd);
    }

    /* Increment file refcount and assign */
    vfs_file_dup((struct file *)f);
    t->fd_table[newfd] = (uintptr_t)f;
    return newfd;
}
