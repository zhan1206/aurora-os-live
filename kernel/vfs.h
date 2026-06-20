/*
 * vfs.h - Virtual File System interface
 *
 * Thread safety: All public VFS functions acquire the dentry lock
 * (vfs_lock) internally. Callers do not need to hold locks.
 *
 * Dentry cache: LRU-based eviction to prevent unbounded memory growth.
 * Maximum dentry count is configurable via MAX_DENTRIES (default 256).
 */
#ifndef VFS_H
#define VFS_H

#include "fs.h"

/* Maximum number of cached dentries before LRU eviction */
#define MAX_DENTRIES  512

void vfs_init(void);
int vfs_mount_root(struct super_block *sb);

/*
 * vfs_mount: Mount a filesystem at a given path.
 * Creates a directory inode at the specified path (the parent must exist)
 * and associates it with the given super_block's root inode.
 * Returns 0 on success, -1 on failure.
 */
int vfs_mount(const char *path, struct super_block *sb);

struct inode *vfs_lookup(const char *path);
struct file *vfs_open(const char *path, int flags);
ssize_t vfs_read(struct file *filp, void *buf, size_t count);
ssize_t vfs_write(struct file *filp, const void *buf, size_t count);
int vfs_close(struct file *filp);
int vfs_file_dup(struct file *filp);
struct super_block *vfs_get_root_sb(void);

/*
 * vfs_dentry_evict: Evict unreferenced dentries from the cache.
 * Automatically called when the dentry count exceeds MAX_DENTRIES.
 * Evicts LRU dentries (least recently used) first, skipping
 * dentries with active references (refcount > 1).
 */
void vfs_dentry_evict(void);

/* Dentry cache statistics */
void vfs_dentry_stats(int *total, int *evicted);

#endif
