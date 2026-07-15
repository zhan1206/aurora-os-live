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

/* Simple spinlock for pipe SMP safety */
static inline void pipe_spin_lock(volatile uint32_t *lock) {
    while (1) {
        uint32_t old = 0;
        uint32_t new = 1;
        asm volatile (
            "lock cmpxchgl %2, %1"
            : "=a"(old), "+m"(*lock)
            : "r"(new), "0"(old)
            : "memory"
        );
        if (old == 0) break;
        asm volatile ("pause" ::: "memory");
    }
}

static inline void pipe_spin_unlock(volatile uint32_t *lock) {
    asm volatile ("movl $0, %0" : "=m"(*lock) : : "memory");
}

/* Pipe ring buffer */
struct pipe_ring {
    char     buf[PIPE_BUF_SIZE];
    uint32_t head;        /* read position */
    uint32_t tail;        /* write position */
    uint32_t count;       /* bytes in buffer */
    int      read_open;   /* read end still open? */
    int      write_open;  /* write end still open? */
    volatile uint32_t lock;  /* spinlock for SMP safety */
    struct task_struct *blocked_reader;  /* task blocked on read (wakeup target) */
    struct task_struct *blocked_writer;  /* task blocked on write (wakeup target) */
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

    /* Block until data available, write end closed, or signal.
     * Use spinlock for SMP safety: protect ring buffer state checks
     * and modifications from concurrent access by pipe_write/pipe_close. */
    for (;;) {
        pipe_spin_lock(&ring->lock);

        if (ring->count > 0) break;  /* data available */
        if (!ring->write_open) {
            pipe_spin_unlock(&ring->lock);
            return 0;  /* EOF */
        }
        /* Set blocked_reader BEFORE releasing the lock to avoid a
         * window where a concurrent write misses the wakeup. */
        ring->blocked_reader = current;
        pipe_spin_unlock(&ring->lock);

        if (!current) return -1;
        if (current->sig && current->sig->pending) {
            /* Clear blocked_reader on early return */
            pipe_spin_lock(&ring->lock);
            if (ring->blocked_reader == current) ring->blocked_reader = NULL;
            pipe_spin_unlock(&ring->lock);
            current->t_errno = EINTR; return -1;
        }
        current->state = TASK_BLOCKED;
        schedule();
        /* After wakeup, re-acquire lock and re-check */
    }

    /* Read from ring buffer (lock still held) */
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

    /* Wake a blocked writer now that we've freed some space */
    if (ring->blocked_writer && ring->blocked_writer->state == TASK_BLOCKED) {
        ring->blocked_writer->state = TASK_READY;
        ring->blocked_writer = NULL;
    }

    pipe_spin_unlock(&ring->lock);
    return (ssize_t)toread;
}

static ssize_t pipe_write(struct file *filp, const void *buf, size_t count,
                          off_t *offset) {
    (void)offset;
    if (!filp || !filp->inode) return -1;
    struct pipe_ring *ring = (struct pipe_ring *)filp->inode->priv;
    if (!ring) return -1;

    pipe_spin_lock(&ring->lock);
    if (!ring->read_open) {
        pipe_spin_unlock(&ring->lock);
        return -1;  /* Reader closed → SIGPIPE */
    }
    pipe_spin_unlock(&ring->lock);

    size_t total = 0;
    const char *src = (const char *)buf;

    while (total < count) {
        /* Block if buffer is full (with SMP-safe spinlock) */
        for (;;) {
            pipe_spin_lock(&ring->lock);
            if (ring->count < PIPE_BUF_SIZE) break;  /* space available */
            if (!ring->read_open) {
                pipe_spin_unlock(&ring->lock);
                current->t_errno = EPIPE; return -1;
            }
            pipe_spin_unlock(&ring->lock);

            if (!current) return -1;
            if (current->sig && current->sig->pending) { current->t_errno = EINTR; return -1; }
            /* Mark ourselves as blocked writer, then block */
            pipe_spin_lock(&ring->lock);
            ring->blocked_writer = current;
            pipe_spin_unlock(&ring->lock);
            current->state = TASK_BLOCKED;
            schedule();
        }

        /* lock still held: write to ring buffer */
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

        /* Wake a blocked reader now that we've written some data */
        if (ring->blocked_reader && ring->blocked_reader->state == TASK_BLOCKED) {
            ring->blocked_reader->state = TASK_READY;
            ring->blocked_reader = NULL;
        }

        pipe_spin_unlock(&ring->lock);
        total += towrite;
    }

    return (ssize_t)total;
}

static int pipe_close(struct inode *inode, struct file *filp) {
    if (!inode) return 0;
    struct pipe_ring *ring = (struct pipe_ring *)inode->priv;
    if (!ring) return 0;

    /* Spinlock for SMP safety: prevent concurrent close from both ends */
    pipe_spin_lock(&ring->lock);

    /* Determine read/write end by comparing the inode's ops table */
    if (inode->ops == &pipe_read_ops) {
        ring->read_open = 0;
    } else {
        ring->write_open = 0;
    }

    /* If both ends closed, free the ring.
     * Clear inode->priv BEFORE releasing the lock to prevent another
     * CPU from accessing the freed ring buffer. */
    if (!ring->read_open && !ring->write_open) {
        inode->priv = NULL;
        pipe_spin_unlock(&ring->lock);
        kfree(ring);
    } else {
        pipe_spin_unlock(&ring->lock);
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
