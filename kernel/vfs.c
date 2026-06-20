/*
 * vfs.c - Virtual File System with dentry cache + multi-level paths
 *
 * Phase 2: LRU dentry eviction + thread-safe locking.
 *   - spinlock_t protects all dentry cache operations.
 *   - LRU linked list tracks access order; oldest unreferenced
 *     dentries are evicted when the cache exceeds MAX_DENTRIES.
 *   - vfs_dentry_evict() is called automatically from dentry_alloc()
 *     and can also be called explicitly via the shell's "vfs-evict" command.
 */
#include "vfs.h"
#include "fs.h"
#include "include/log.h"
#include "include/userspace.h"
#include "mem.h"
#include <string.h>
#include <stdint.h>

static struct super_block *root_sb = NULL;
static struct dentry *root_dentry = NULL;

/* ================================================================
 * Dentry cache statistics
 * ================================================================ */
static int dentry_total = 0;    /* current number of cached dentries */
static int dentry_evicted = 0;  /* total evictions since boot */

/* ================================================================
 * VFS spinlock (protects all dentry cache operations)
 *
 * Uses atomic cmpxchg with PAUSE for SMP safety.
 * Lock ordering: vfs_lock is always acquired AFTER buddy_lock
 * and slab_lock when both are needed (consistent with mem.c convention).
 * ================================================================ */
typedef struct {
    volatile uint32_t locked;
} spinlock_t;

static spinlock_t vfs_lock_ = {0};

static inline void vfs_lock(void) {
    while (1) {
        uint32_t old = 0, new = 1;
        asm volatile (
            "lock cmpxchgl %2, %1"
            : "=a"(old), "+m"(vfs_lock_.locked)
            : "r"(new), "0"(old)
            : "memory"
        );
        if (old == 0) break;
        asm volatile ("pause" ::: "memory");
    }
}

static inline void vfs_unlock(void) {
    asm volatile ("movl $0, %0" : "=m"(vfs_lock_.locked) : : "memory");
}

/* ================================================================
 * Dentry LRU list
 *
 * Each dentry has an lru_prev/lru_next pointer.
 * The LRU list is a doubly-linked circular list.
 * The list head is a sentinel dentry (lru_head).
 * New dentries are added to the head (most recently used).
 * Eviction removes from the tail (least recently used).
 * ================================================================ */
static struct dentry lru_head;

static void lru_init(void) {
    lru_head.lru_prev = &lru_head;
    lru_head.lru_next = &lru_head;
}

static void lru_add(struct dentry *d) {
    /* Insert at head (most recently used) */
    d->lru_next = lru_head.lru_next;
    d->lru_prev = &lru_head;
    lru_head.lru_next->lru_prev = d;
    lru_head.lru_next = d;
}

static void lru_del(struct dentry *d) {
    /* Remove from LRU list */
    if (d->lru_prev && d->lru_next) {
        d->lru_prev->lru_next = d->lru_next;
        d->lru_next->lru_prev = d->lru_prev;
        d->lru_prev = NULL;
        d->lru_next = NULL;
    }
}

/* Move a dentry to the head of the LRU (mark as recently used) */
static void lru_touch(struct dentry *d) {
    lru_del(d);
    lru_add(d);
}

/* ================================================================
 * Dentry cache
 * ================================================================ */

static struct dentry *dentry_alloc(const char *name, struct dentry *parent) {
    struct dentry *d = (struct dentry *)kmalloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));

    /* Copy the name — we own the memory now, caller can free theirs */
    size_t name_len = 0;
    for (const char *p = name; *p; ++p) name_len++;
    char *name_copy = (char *)kmalloc(name_len + 1);
    if (!name_copy) {
        kfree(d);
        return NULL;
    }
    memcpy(name_copy, name, name_len + 1);
    d->name     = name_copy;
    d->parent   = parent;
    d->refcount = 1;
    d->access_count = 1;

    /* Add to LRU and increment counter */
    lru_add(d);
    dentry_total++;

    /* Evict old dentries if cache is too large */
    if (dentry_total > MAX_DENTRIES) {
        vfs_dentry_evict();
    }

    return d;
}

static void dentry_add_child(struct dentry *parent, struct dentry *child) {
    child->next = parent->child;
    parent->child = child;
}

static struct dentry *dentry_lookup_child(struct dentry *parent,
                                           const char *name) {
    struct dentry *d = parent->child;
    while (d) {
        if (d->name && strcmp(d->name, name) == 0)
            return d;
        d = d->next;
    }
    return NULL;
}

/* ================================================================
 * VFS init
 * ================================================================ */

void vfs_init(void) {
    root_sb    = NULL;
    root_dentry = NULL;
    lru_init();
    dentry_total = 0;
    dentry_evicted = 0;
}

struct super_block *vfs_get_root_sb(void) {
    return root_sb;
}

int vfs_mount_root(struct super_block *sb) {
    if (!sb || !sb->root) return -1;
    root_sb = sb;

    /* Create root dentry */
    root_dentry = dentry_alloc("/", NULL);
    if (!root_dentry) return -1;
    root_dentry->inode = sb->root;
    sb->root->dentry = root_dentry;
    sb->root_dentry  = root_dentry;

    log_printf(LOG_LEVEL_INFO, "VFS: mounted root fs '%s'\n", sb->fs_name);
    return 0;
}

/*
 * vfs_mount: Mount a filesystem at a subdirectory path.
 *
 * Resolves the parent directory of the mount point, creates a dentry
 * for the mount point name, and associates it with the superblock's
 * root inode.  The parent directory must already exist (e.g., "/").
 *
 * Example: vfs_mount("/proc", procfs_sb) mounts procfs at /proc.
 */
int vfs_mount(const char *path, struct super_block *sb) {
    if (!root_sb || !root_dentry || !path || !sb || !sb->root)
        return -1;
    if (path[0] != '/')
        return -1;

    /* Find the parent directory and the mount point name */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    const char *mount_name;
    struct dentry *parent_dentry;

    if (last_slash == path) {
        /* Mounting at root level, e.g., "/proc" */
        mount_name = path + 1;
        parent_dentry = root_dentry;
    } else {
        /* Mounting at a deeper path — resolve the parent */
        mount_name = last_slash + 1;
        /* We need to look up the parent. For simplicity, only support
         * single-level paths like "/proc". */
        if (last_slash != path) {
            /* Parent is not root — not supported for now */
            log_printf(LOG_LEVEL_WARN, "VFS: vfs_mount only supports root-level mounts\n");
            return -1;
        }
        parent_dentry = root_dentry;
    }

    if (!mount_name || *mount_name == '\0')
        return -1;

    /* Check if already mounted */
    struct dentry *existing = dentry_lookup_child(parent_dentry, mount_name);
    if (existing && existing->inode) {
        log_printf(LOG_LEVEL_WARN, "VFS: mount point '%s' already exists\n", path);
        return -1;
    }

    /* Create dentry for the mount point */
    struct dentry *mount_dentry = dentry_alloc(mount_name, parent_dentry);
    if (!mount_dentry) return -1;

    /* Associate with the superblock's root inode */
    mount_dentry->inode = sb->root;
    sb->root->dentry = mount_dentry;
    sb->root_dentry = mount_dentry;

    /* Add to parent's children */
    dentry_add_child(parent_dentry, mount_dentry);

    log_printf(LOG_LEVEL_INFO, "VFS: mounted '%s' at '%s'\n", sb->fs_name, path);
    return 0;
}

/* ================================================================
 * vfs_dentry_evict: LRU-based dentry eviction
 *
 * Evicts unreferenced (refcount <= 1) dentries from the tail of
 * the LRU list. Evicted dentries are freed along with their
 * associated inode if the inode is not referenced elsewhere.
 * Evicts at most 16 dentries per call to avoid long lock hold times.
 * ================================================================ */
void vfs_dentry_evict(void) {
    int evicted_this_round = 0;
    int max_evict = 16;

    vfs_lock();

    struct dentry *d = lru_head.lru_prev;  /* start from tail (oldest) */
    while (d != &lru_head && evicted_this_round < max_evict) {
        struct dentry *prev = d->lru_prev;

        /* Skip root dentry and dentries with active references */
        if (d == root_dentry || d->refcount > 1) {
            d = prev;
            continue;
        }

        /* Remove from LRU list */
        lru_del(d);

        /* Free the inode if it's only referenced by this dentry */
        if (d->inode) {
            if (d->inode->name) {
                kfree((void *)d->inode->name);
            }
            if (d->inode->priv) {
                kfree(d->inode->priv);
            }
            kfree(d->inode);
        }

        /* Free the name */
        if (d->name) {
            kfree((void *)d->name);
        }

        kfree(d);
        dentry_total--;
        dentry_evicted++;
        evicted_this_round++;

        d = prev;
    }

    vfs_unlock();

    if (evicted_this_round > 0) {
        log_printf(LOG_LEVEL_DEBUG, "VFS: evicted %d dentries (total=%d, evicted=%d)\n",
                   evicted_this_round, dentry_total, dentry_evicted);
    }
}

void vfs_dentry_stats(int *total, int *evicted) {
    if (total)   *total   = dentry_total;
    if (evicted) *evicted = dentry_evicted;
}

/* ================================================================
 * vfs_lookup: Multi-level path resolution with dentry cache
 *
 * Path components are separated by '/'.
 * Walks the dentry tree from root, creating new dentries as needed.
 * For each component, asks the parent inode's lookup() op to
 * resolve the child (if not already cached in dentry).
 *
 * Security: Rejects path traversal attempts (".." components).
 * Paths must be absolute (start with '/').
 * Component names are limited to 255 bytes.
 *
 * Returns the inode for the final component, or NULL.
 * ================================================================ */

/* Check if a path component is a traversal attempt ("." or "..") */
static int is_path_traversal(const char *name, size_t len) {
    if (len == 1 && name[0] == '.') return 0;  /* "." is valid: current directory */
    if (len == 2 && name[0] == '.' && name[1] == '.') return 1;  /* ".." is traversal */
    return 0;
}

struct inode *vfs_lookup(const char *path) {
    if (!root_sb || !root_dentry) return NULL;
    if (!path || path[0] != '/') return NULL;

    /* Reject paths that are too long (security: prevent buffer overflow) */
    size_t path_len = 0;
    for (const char *q = path; *q; q++) path_len++;
    if (path_len > 4096) {
        log_printf(LOG_LEVEL_WARN, "VFS: path too long (%zu bytes)\n", path_len);
        return NULL;
    }

    struct dentry *cur = root_dentry;
    const char *p = path + 1;  /* skip leading '/' */

    /* Handle root path "/" */
    if (*p == '\0') return cur->inode;

    while (*p) {
        /* Extract next component name */
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 0) { p++; continue; }  /* skip "//" */
        if (len > 255) return NULL;        /* component too long */

        /* Reject path traversal ("..") */
        if (is_path_traversal(start, len)) {
            log_printf(LOG_LEVEL_WARN, "VFS: path traversal rejected: %.*s\n",
                       (int)len, start);
            return NULL;
        }

        /* Handle "." component: skip it, stay in current directory */
        if (len == 1 && start[0] == '.') {
            if (*p == '/') p++;
            continue;
        }

        /* Use stack buffer to avoid kmalloc for temporary name */
        char name[256];
        memcpy(name, start, len);
        name[len] = '\0';

        /* Look up in dentry cache */
        struct dentry *child = dentry_lookup_child(cur, name);

        if (child && child->inode) {
            /* Cache hit — mark as recently used */
            child->access_count++;
            lru_touch(child);
            cur = child;
        } else {
            /* Cache miss: ask filesystem */
            if (!child) {
                child = dentry_alloc(name, cur);
                if (!child) return NULL;
                dentry_add_child(cur, child);
            }

            /* Ask parent inode to resolve this component */
            if (cur->inode && cur->inode->ops && cur->inode->ops->lookup) {
                cur->inode->ops->lookup(cur->inode, child);
            }

            if (!child->inode) {
                /* Negative dentry: component not found */
                return NULL;
            }
            cur = child;
        }

        if (*p == '/') p++;
    }

    return cur->inode;
}

/* ================================================================
 * vfs_open / vfs_read / vfs_close (unchanged API)
 * ================================================================ */

struct file *vfs_open(const char *path, int flags) {
    struct inode *inode = vfs_lookup(path);
    if (!inode) return NULL;

    struct file *filp = (struct file *)kmalloc(sizeof(*filp));
    if (!filp) return NULL;
    memset(filp, 0, sizeof(*filp));
    filp->inode    = inode;
    filp->flags    = flags;
    filp->refcount = 1;

    if (inode->ops && inode->ops->open) {
        if (inode->ops->open(inode, filp) < 0) {
            kfree(filp);
            return NULL;
        }
    }
    return filp;
}

ssize_t vfs_read(struct file *filp, void *buf, size_t count) {
    if (!filp || !filp->inode || !filp->inode->ops || !filp->inode->ops->read)
        return -1;
    return filp->inode->ops->read(filp, buf, count, &filp->offset);
}

ssize_t vfs_write(struct file *filp, const void *buf, size_t count) {
    if (!filp || !filp->inode || !filp->inode->ops || !filp->inode->ops->write)
        return -1;
    return filp->inode->ops->write(filp, buf, count, &filp->offset);
}

int vfs_close(struct file *filp) {
    if (!filp) return -1;
    if (filp->refcount <= 0) return -1;

    /* Only actually close when last reference is dropped */
    if (--filp->refcount > 0) return 0;

    if (filp->inode && filp->inode->ops && filp->inode->ops->close)
        filp->inode->ops->close(filp->inode, filp);
    kfree(filp);
    return 0;
}

/*
 * vfs_file_dup: Increment refcount for fork/clone fd sharing.
 * Returns 0 on success, -1 if filp is NULL.
 */
int vfs_file_dup(struct file *filp) {
    if (!filp) return -1;
    if (filp->refcount <= 0) return -1;
    filp->refcount++;
    return 0;
}
