/*
 * pipe.c - Anonymous pipe (NEW — Phase 1)
 *
 * A pipe is a pair of file descriptors (read end, write end) backed
 * by a ring buffer in kernel memory. Data written to the write end
 * can be read from the read end in FIFO order.
 *
 * Design:
 *   - Ring buffer: PIPE_BUF_SIZE bytes, circular.
 *   - Blocking read/write: if buffer is empty/full, blocks the task
 *     (TASK_BLOCKED) and yields. Woken by the other end.
 *   - Close detection: read returns 0 when write end is closed and
 *     buffer is empty (EOF).
 *
 * VFS integration: pipefs is a simple filesystem that creates
 * inode pairs for each pipe. The inode's priv points to the
 * pipe_ring structure.
 */
#include "fs.h"
#include "vfs.h"
#include "sched.h"
#include "include/log.h"
#include "include/userspace.h"
#include "include/errno.h"
#include "mem.h"
#include <string.h>
#include <stdint.h>

#define PIPE_BUF_SIZE 4096

/* Pipe ring buffer */
struct pipe_ring {
    char     buf[PIPE_BUF_SIZE];
    uint32_t head;        /* read position */
    uint32_t tail;        /* write position */
    uint32_t count;       /* bytes in buffer */
    int      read_open;   /* read end still open? */
    int      write_open;  /* write end still open? */
};

/* ================================================================
 * Pipe file operations
 * ================================================================ */

static int pipe_open(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp;
    return 0;
}

static ssize_t pipe_read(struct file *filp, void *buf, size_t count,
                         off_t *offset) {
    (void)offset;
    if (!filp || !filp->inode) return -1;
    struct pipe_ring *ring = (struct pipe_ring *)filp->inode->priv;
    if (!ring) return -1;

    /* Block until data available, write end closed, or signal */
    while (ring->count == 0) {
        if (!ring->write_open) return 0;  /* EOF */
        if (!current) return -1;
        if (current->sig && current->sig->pending) { current->t_errno = EINTR; return -1; }
        current->state = TASK_BLOCKED;
        schedule();
    }

    /* Read from ring buffer */
    size_t toread = count;
    if (toread > ring->count) toread = ring->count;

    /*
     * Copy from ring buffer. First copy from head to end of buffer,
     * then wrap around and copy from start if needed.
     */
    size_t first_chunk = PIPE_BUF_SIZE - ring->head;
    if (first_chunk > toread) first_chunk = toread;
    memcpy((char *)buf, ring->buf + ring->head, first_chunk);
    if (toread > first_chunk) {
        memcpy((char *)buf + first_chunk, ring->buf, toread - first_chunk);
    }
    ring->head = (ring->head + toread) % PIPE_BUF_SIZE;
    ring->count -= (uint32_t)toread;

    return (ssize_t)toread;
}

static ssize_t pipe_write(struct file *filp, const void *buf, size_t count,
                          off_t *offset) {
    (void)offset;
    if (!filp || !filp->inode) return -1;
    struct pipe_ring *ring = (struct pipe_ring *)filp->inode->priv;
    if (!ring) return -1;
    if (!ring->read_open) {
        /* Reader closed → SIGPIPE (simplified: return -1) */
        return -1;
    }

    size_t total = 0;
    const char *src = (const char *)buf;

    while (total < count) {
        /* Block if buffer is full */
        while (ring->count >= PIPE_BUF_SIZE) {
            if (!ring->read_open) { current->t_errno = EPIPE; return -1; }
            if (!current) return -1;
            if (current->sig && current->sig->pending) { current->t_errno = EINTR; return -1; }
            current->state = TASK_BLOCKED;
            schedule();
        }

        size_t space = PIPE_BUF_SIZE - ring->count;
        size_t towrite = count - total;
        if (towrite > space) towrite = space;

        /*
         * Copy to ring buffer. First copy from tail to end of buffer,
         * then wrap around and copy from start if needed.
         */
        size_t first_chunk = PIPE_BUF_SIZE - ring->tail;
        if (first_chunk > towrite) first_chunk = towrite;
        memcpy(ring->buf + ring->tail, src + total, first_chunk);
        if (towrite > first_chunk) {
            memcpy(ring->buf, src + total + first_chunk, towrite - first_chunk);
        }
        ring->tail = (ring->tail + towrite) % PIPE_BUF_SIZE;
        ring->count += (uint32_t)towrite;

        total += towrite;
    }

    return (ssize_t)total;
}

static int pipe_close(struct inode *inode, struct file *filp) {
    if (!inode) return 0;
    struct pipe_ring *ring = (struct pipe_ring *)inode->priv;
    if (!ring) return 0;

    /*
     * Determine which end is being closed based on the inode.
     * We use a convention: the read-end inode has name "pipe:[r]",
     * the write-end inode has name "pipe:[w]".
     */
    if (inode->name && inode->name[0] == 'r') {
        ring->read_open = 0;
    } else {
        ring->write_open = 0;
    }

    /* If both ends closed, free the ring */
    if (!ring->read_open && !ring->write_open) {
        kfree(ring);
        inode->priv = NULL;
    }

    (void)filp;
    return 0;
}

static struct file_ops pipe_read_ops = {
    .open  = pipe_open,
    .read  = pipe_read,
    .write = NULL,       /* read end doesn't support write */
    .close = pipe_close,
};

static struct file_ops pipe_write_ops = {
    .open  = pipe_open,
    .read  = NULL,       /* write end doesn't support read */
    .write = pipe_write,
    .close = pipe_close,
};

/* ================================================================
 * sys_pipe: Create a pipe, return two fds
 *
 * @fds: user-space array of 2 ints → fds[0]=read, fds[1]=write.
 * Returns 0 on success, -1 on error.
 * ================================================================ */

int sys_pipe(int *fds) {
    if (!fds) return -1;
    if (!user_addr_range_ok(fds, 2 * sizeof(int))) return -1;

    /* Allocate ring buffer */
    struct pipe_ring *ring = (struct pipe_ring *)kmalloc(sizeof(*ring));
    if (!ring) return -1;
    memset(ring, 0, sizeof(*ring));
    ring->read_open  = 1;
    ring->write_open = 1;

    /* Create read-end inode */
    struct inode *rinode = (struct inode *)kmalloc(sizeof(*rinode));
    if (!rinode) { kfree(ring); return -1; }
    memset(rinode, 0, sizeof(*rinode));
    rinode->name = "r";
    rinode->ops  = &pipe_read_ops;
    rinode->priv = ring;

    /* Create write-end inode */
    struct inode *winode = (struct inode *)kmalloc(sizeof(*winode));
    if (!winode) { kfree(rinode); kfree(ring); return -1; }
    memset(winode, 0, sizeof(*winode));
    winode->name = "w";
    winode->ops  = &pipe_write_ops;
    winode->priv = ring;

    /* Create file objects */
    struct file *rfilp = (struct file *)kmalloc(sizeof(*rfilp));
    struct file *wfilp = (struct file *)kmalloc(sizeof(*wfilp));
    if (!rfilp || !wfilp) {
        if (rfilp) kfree(rfilp);
        if (wfilp) kfree(wfilp);
        kfree(winode); kfree(rinode); kfree(ring);
        return -1;
    }
    memset(rfilp, 0, sizeof(*rfilp));
    memset(wfilp, 0, sizeof(*wfilp));
    rfilp->inode = rinode;
    wfilp->inode = winode;

    /* Allocate fds */
    int rfd = fd_alloc(current, rfilp);
    int wfd = fd_alloc(current, wfilp);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) fd_close(current, rfd);
        else if (rfilp) { kfree(rfilp); rfilp = NULL; }
        if (wfd >= 0) fd_close(current, wfd);
        else if (wfilp) { kfree(wfilp); wfilp = NULL; }
        /* Clean up remaining resources: inodes and ring buffer */
        kfree(ring); kfree(rinode); kfree(winode);
        return -1;
    }

    /* Write fds to user space */
    int kfds[2] = { rfd, wfd };
    if (copy_to_user(fds, kfds, sizeof(kfds)) != 0) {
        fd_close(current, rfd);
        fd_close(current, wfd);
        return -1;
    }

    log_printf(LOG_LEVEL_DEBUG, "pipe: created fds [%d, %d]\n", rfd, wfd);
    return 0;
}
