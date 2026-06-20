/*
 * fs.h - Virtual File System core structures (Phase 1: dentry added)
 */
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>
#include "types.h"

struct inode;
struct file;
struct dentry;

/* File operations table */
struct file_ops {
    int (*open)(struct inode *inode, struct file *filp);
    ssize_t (*read)(struct file *filp, void *buf, size_t count, off_t *offset);
    ssize_t (*write)(struct file *filp, const void *buf, size_t count, off_t *offset);
    int (*close)(struct inode *inode, struct file *filp);
    /* Directory operations (for directory inodes) */
    int (*lookup)(struct inode *dir, struct dentry *dentry);
};

/* Inode: filesystem object */
struct inode {
    const char      *name;   /* entry name within parent directory */
    void            *priv;   /* fs-private data */
    struct file_ops *ops;    /* operations table */
    struct dentry   *dentry; /* back-pointer to dentry (may be NULL for root) */
    int              is_dir; /* 1 if this inode represents a directory */
};

/* Dentry: directory entry cache */
struct dentry {
    const char      *name;    /* component name */
    struct inode    *inode;   /* associated inode (NULL if negative) */
    struct dentry   *parent;  /* parent directory dentry */
    struct dentry   *child;   /* first child dentry (linked list) */
    struct dentry   *next;    /* next sibling dentry */
    struct dentry   *lru_prev; /* LRU list: previous entry */
    struct dentry   *lru_next; /* LRU list: next entry */
    int              refcount;
    int              access_count; /* access count for LRU aging */
};

/* Open file description */
struct file {
    struct inode *inode;
    void         *private_data;
    off_t         offset;
    int           flags;
    int           refcount;     /* reference count for fork/clone sharing */
};

/* ============ FS initialization (from ramfs.c, embed.c) ============ */
struct super_block *ramfs_create(void);
int ramfs_add_file(const char *name, const char *content);
void embed_init(void);
void fs_init(void);

/* Superblock: mounted filesystem */
struct super_block {
    const char     *fs_name;
    struct inode   *root;       /* root inode */
    struct dentry  *root_dentry; /* root dentry */
    void           *sb_data;    /* fs-specific data */
};

#endif /* FS_H */
