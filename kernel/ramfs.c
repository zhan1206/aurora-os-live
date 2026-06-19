/*
 * ramfs.c - In-memory filesystem (Phase 1: dentry lookup support)
 */
#include "fs.h"
#include "include/log.h"
#include "mem.h"
#include <string.h>

struct ramfs_node {
    struct inode      inode;
    struct ramfs_node *next;   /* linked list of directory entries */
    size_t            size;
    char             *data;
};

static struct ramfs_node *ramfs_root = NULL;

/* ================================================================
 * File operations
 * ================================================================ */

static int ramfs_open(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp; return 0;
}

static ssize_t ramfs_read(struct file *filp, void *buf, size_t count,
                          off_t *offset) {
    struct ramfs_node *n = (struct ramfs_node *)filp->inode;
    if (!n || !buf) return -1;
    if (*offset >= (off_t)n->size) return 0;
    size_t toread = count;
    if ((size_t)(*offset) + toread > n->size) toread = n->size - (*offset);
    memcpy(buf, n->data + (*offset), toread);
    *offset += toread;
    return toread;
}

static int ramfs_close(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp; return 0;
}

/*
 * ramfs_lookup: Resolve a name within a ramfs directory.
 * Searches the linked list for a matching name.
 */
static int ramfs_lookup(struct inode *dir, struct dentry *dentry) {
    struct ramfs_node *head = (struct ramfs_node *)dir;
    struct ramfs_node *n = head->next;  /* skip root sentinel */

    while (n) {
        if (n->inode.name && strcmp(n->inode.name, dentry->name) == 0) {
            dentry->inode = &n->inode;
            n->inode.dentry = dentry;
            return 0;
        }
        n = n->next;
    }
    return -1;  /* not found (negative dentry) */
}

/*
 * ramfs_write: Write data to a ramfs file.
 * If the write extends beyond current file size, the buffer is expanded.
 */
static ssize_t ramfs_write(struct file *filp, const void *buf, size_t count,
                           off_t *offset) {
    struct ramfs_node *n = (struct ramfs_node *)filp->inode;
    if (!n || !buf || !offset) return -1;

    size_t new_size = (size_t)(*offset) + count;
    if (new_size > n->size) {
        /* Expand buffer */
        char *new_data = (char *)kmalloc(new_size);
        if (!new_data) return -1;
        if (n->data && n->size > 0)
            memcpy(new_data, n->data, n->size);
        if (n->data) kfree(n->data);
        n->data = new_data;
        n->size = new_size;
    }
    memcpy(n->data + (*offset), buf, count);
    *offset += (off_t)count;
    return (ssize_t)count;
}

static struct file_ops ramfs_file_ops = {
    .open   = ramfs_open,
    .read   = ramfs_read,
    .write  = ramfs_write,
    .close  = ramfs_close,
    .lookup = NULL,
};

static struct file_ops ramfs_dir_ops = {
    .open   = ramfs_open,
    .read   = NULL,   /* directories not readable as files */
    .write  = NULL,
    .close  = ramfs_close,
    .lookup = ramfs_lookup,
};

/* ================================================================
 * Filesystem operations
 * ================================================================ */

struct super_block *ramfs_create(void) {
    struct ramfs_node *head = (struct ramfs_node *)kmalloc(sizeof(*head));
    if (!head) return NULL;
    memset(head, 0, sizeof(*head));
    head->inode.name  = "";
    head->inode.ops   = &ramfs_dir_ops;  /* root is a directory */
    head->inode.is_dir = 1;
    head->inode.priv  = NULL;
    head->next = NULL;

    struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
    if (!sb) { kfree(head); return NULL; }
    memset(sb, 0, sizeof(*sb));
    sb->fs_name = "ramfs";
    sb->root    = &head->inode;
    sb->sb_data = head;
    ramfs_root  = head;
    return sb;
}

int ramfs_add_file(const char *name, const char *content) {
    if (!ramfs_root) return -1;
    if (!name || !content) return -1;

    /* Check for duplicate file names */
    struct ramfs_node *existing = ramfs_root->next;
    while (existing) {
        if (existing->inode.name && strcmp(existing->inode.name, name) == 0) {
            log_printf(LOG_LEVEL_WARN, "ramfs: duplicate file '%s' ignored\n", name);
            return -1;
        }
        existing = existing->next;
    }

    struct ramfs_node *n = (struct ramfs_node *)kmalloc(sizeof(*n));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));

    n->inode.name   = kmalloc(strlen(name) + 1);
    if (!n->inode.name) { kfree(n); return -1; }
    strcpy((char *)n->inode.name, name);

    n->size = strlen(content);
    n->data = (char *)kmalloc(n->size + 1);
    if (!n->data) { kfree((void *)n->inode.name); kfree(n); return -1; }
    memcpy(n->data, content, n->size);
    n->data[n->size] = '\0';

    n->inode.ops    = &ramfs_file_ops;
    n->inode.is_dir = 0;
    n->inode.priv   = NULL;

    /* Insert at head of directory listing */
    n->next = ramfs_root->next;
    ramfs_root->next = n;
    return 0;
}

int ramfs_add_file_data(const char *name, const void *data, size_t size) {
    if (!ramfs_root) return -1;
    if (!name || !data) return -1;

    /* Check for duplicate file names */
    struct ramfs_node *existing = ramfs_root->next;
    while (existing) {
        if (existing->inode.name && strcmp(existing->inode.name, name) == 0) {
            log_printf(LOG_LEVEL_WARN, "ramfs: duplicate file '%s' ignored\n", name);
            return -1;
        }
        existing = existing->next;
    }

    struct ramfs_node *n = (struct ramfs_node *)kmalloc(sizeof(*n));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));

    n->inode.name = kmalloc(strlen(name) + 1);
    if (!n->inode.name) { kfree(n); return -1; }
    strcpy((char *)n->inode.name, name);

    n->size = size;
    n->data = (char *)kmalloc(size);
    if (!n->data) { kfree((void *)n->inode.name); kfree(n); return -1; }
    memcpy(n->data, data, size);

    n->inode.ops    = &ramfs_file_ops;
    n->inode.is_dir = 0;
    n->inode.priv   = NULL;

    n->next = ramfs_root->next;
    ramfs_root->next = n;
    return 0;
}
