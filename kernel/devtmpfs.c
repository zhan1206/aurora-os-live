/*
 * devtmpfs.c - /dev virtual filesystem implementation
 *
 * Inspired by CoolPotOS's devtmpfs. Provides a simple device
 * filesystem that auto-creates device nodes in /dev.
 *
 * Each device file is backed by a function that handles read/write
 * operations. Uses the existing VFS infrastructure.
 */

#include "devtmpfs.h"
#include "fs.h"
#include "vfs.h"
#include "mem.h"
#include "console.h"
#include "include/log.h"
#include <string.h>
#include <stdint.h>

/* ================================================================
 * Device entry types
 * ================================================================ */
#define DEV_TYPE_NULL    0
#define DEV_TYPE_ZERO    1
#define DEV_TYPE_CONSOLE 2
#define DEV_TYPE_TTY     3
#define DEV_TYPE_RANDOM  4
#define DEV_TYPE_URANDOM 5

struct dev_entry {
    const char *name;
    int         type;
};

/* ================================================================
 * Device inode private data
 * ================================================================ */
struct devtmpfs_inode_data {
    int type;  /* DEV_TYPE_* */
};

/* ================================================================
 * Device table
 * ================================================================ */
static struct dev_entry dev_entries[] = {
    { "null",    DEV_TYPE_NULL    },
    { "zero",    DEV_TYPE_ZERO    },
    { "console", DEV_TYPE_CONSOLE },
    { "tty",     DEV_TYPE_TTY     },
    { "random",  DEV_TYPE_RANDOM  },
    { "urandom", DEV_TYPE_URANDOM },
    { NULL,      0                },  /* sentinel */
};

/* ================================================================
 * Forward declarations
 * ================================================================ */
static struct file_ops devtmpfs_file_ops;
static struct file_ops devtmpfs_dir_ops;
static int devtmpfs_lookup(struct inode *dir, struct dentry *dentry);

/* ================================================================
 * Device read/write handlers
 * ================================================================ */

/*
 * dev_null_read: Always returns EOF (0 bytes).
 */
static ssize_t dev_null_read(struct file *filp, void *buf, size_t count,
                             off_t *offset) {
    (void)filp; (void)buf; (void)count; (void)offset;
    return 0;  /* EOF */
}

/*
 * dev_null_write: Discards all data (returns count as if written).
 */
static ssize_t dev_null_write(struct file *filp, const void *buf, size_t count,
                              off_t *offset) {
    (void)filp; (void)buf; (void)offset;
    return (ssize_t)count;  /* Silently discard */
}

/*
 * dev_zero_read: Returns zero-filled buffer.
 */
static ssize_t dev_zero_read(struct file *filp, void *buf, size_t count,
                             off_t *offset) {
    (void)filp; (void)offset;
    if (!buf || count == 0) return 0;
    memset(buf, 0, count);
    return (ssize_t)count;
}

/*
 * dev_zero_write: Discards data (like /dev/null).
 */
static ssize_t dev_zero_write(struct file *filp, const void *buf, size_t count,
                              off_t *offset) {
    (void)filp; (void)buf; (void)offset;
    return (ssize_t)count;
}

/*
 * dev_console_read: Read from console input (blocking line read).
 */
static ssize_t dev_console_read(struct file *filp, void *buf, size_t count,
                                off_t *offset) {
    (void)filp; (void)offset;
    if (!buf || count == 0) return 0;

    /* Block until a line is available from the console */
    int len = console_getline((char *)buf, count);
    if (len <= 0) return 0;
    return (ssize_t)len;
}

/*
 * dev_console_write: Write to the console.
 */
static ssize_t dev_console_write(struct file *filp, const void *buf, size_t count,
                                 off_t *offset) {
    (void)filp; (void)offset;
    if (!buf || count == 0) return 0;

    const char *cbuf = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        console_putc(cbuf[i]);
    }
    return (ssize_t)count;
}

/*
 * rdrand32: Get a 32-bit random value using RDRAND instruction.
 * Returns 1 on success, 0 if RDRAND is not available or failed.
 */
static int rdrand32(uint32_t *val) {
    int ok = 0;
    asm volatile (
        "1:\n\t"
        "rdrand %0\n\t"
        "setc %1\n\t"
        "jnc 1b\n\t"
        : "=r"(*val), "=qm"(ok)
        :
        : "cc"
    );
    return ok;
}

/*
 * dev_random_read: Read random bytes using RDRAND.
 * Blocks until enough random bytes are available (for /dev/random).
 */
static ssize_t dev_random_read(struct file *filp, void *buf, size_t count,
                                off_t *offset, int blocking) {
    (void)filp; (void)offset;
    if (!buf || count == 0) return 0;

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = count;

    while (remaining > 0) {
        uint32_t rand_val;
        if (!rdrand32(&rand_val)) {
            if (blocking) {
                /* For /dev/random, retry on failure */
                continue;
            }
            /* For /dev/urandom, stop on failure */
            break;
        }
        size_t copy = (remaining < 4) ? remaining : 4;
        memcpy(dst, &rand_val, copy);
        dst += copy;
        remaining -= copy;
    }

    return (ssize_t)(count - remaining);
}

/*
 * dev_random_write: Discard data (like /dev/null).
 */
static ssize_t dev_random_write(struct file *filp, const void *buf, size_t count,
                                off_t *offset) {
    (void)filp; (void)buf; (void)offset;
    return (ssize_t)count;
}

/* ================================================================
 * Devtmpfs file operations
 * ================================================================ */

static int devtmpfs_open(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp;
    return 0;
}

static ssize_t devtmpfs_read(struct file *filp, void *buf, size_t count,
                             off_t *offset) {
    if (!filp || !filp->inode || !buf) return -1;

    struct devtmpfs_inode_data *data =
        (struct devtmpfs_inode_data *)filp->inode->priv;
    if (!data) return -1;

    switch (data->type) {
        case DEV_TYPE_NULL:    return dev_null_read(filp, buf, count, offset);
        case DEV_TYPE_ZERO:    return dev_zero_read(filp, buf, count, offset);
        case DEV_TYPE_CONSOLE: return dev_console_read(filp, buf, count, offset);
        case DEV_TYPE_TTY:     return dev_console_read(filp, buf, count, offset);
        case DEV_TYPE_RANDOM:  return dev_random_read(filp, buf, count, offset, 1);
        case DEV_TYPE_URANDOM: return dev_random_read(filp, buf, count, offset, 0);
        default:               return -1;
    }
}

static ssize_t devtmpfs_write(struct file *filp, const void *buf, size_t count,
                              off_t *offset) {
    if (!filp || !filp->inode || !buf) return -1;

    struct devtmpfs_inode_data *data =
        (struct devtmpfs_inode_data *)filp->inode->priv;
    if (!data) return -1;

    switch (data->type) {
        case DEV_TYPE_NULL:    return dev_null_write(filp, buf, count, offset);
        case DEV_TYPE_ZERO:    return dev_zero_write(filp, buf, count, offset);
        case DEV_TYPE_CONSOLE: return dev_console_write(filp, buf, count, offset);
        case DEV_TYPE_TTY:     return dev_console_write(filp, buf, count, offset);
        case DEV_TYPE_RANDOM:  return dev_random_write(filp, buf, count, offset);
        case DEV_TYPE_URANDOM: return dev_random_write(filp, buf, count, offset);
        default:               return -1;
    }
}

static int devtmpfs_close(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp;
    return 0;
}

/* ================================================================
 * File operations tables
 * ================================================================ */

static struct file_ops devtmpfs_file_ops = {
    .open   = devtmpfs_open,
    .read   = devtmpfs_read,
    .write  = devtmpfs_write,
    .close  = devtmpfs_close,
    .lookup = NULL,
};

static struct file_ops devtmpfs_dir_ops = {
    .open   = devtmpfs_open,
    .read   = NULL,
    .write  = NULL,
    .close  = devtmpfs_close,
    .lookup = devtmpfs_lookup,
};

/* ================================================================
 * lookup: Resolve a name within the devtmpfs root directory
 * ================================================================ */

static int devtmpfs_lookup(struct inode *dir, struct dentry *dentry) {
    if (!dir || !dentry || !dentry->name) return -1;

    /* Search the device entry table */
    for (int i = 0; dev_entries[i].name != NULL; i++) {
        struct dev_entry *e = &dev_entries[i];
        if (strcmp(e->name, dentry->name) == 0) {
            /* Create an inode for this device */
            struct inode *inode = (struct inode *)kmalloc(sizeof(*inode));
            if (!inode) return -1;
            memset(inode, 0, sizeof(*inode));

            struct devtmpfs_inode_data *data =
                (struct devtmpfs_inode_data *)kmalloc(sizeof(*data));
            if (!data) { kfree(inode); return -1; }
            memset(data, 0, sizeof(*data));

            data->type = e->type;

            inode->name = e->name;
            inode->priv = data;
            inode->is_dir = 0;
            inode->ops = &devtmpfs_file_ops;
            inode->dentry = dentry;
            dentry->inode = inode;
            return 0;
        }
    }

    return -1;  /* Not found */
}

/* ================================================================
 * devtmpfs_create: Create the devtmpfs filesystem super block
 * ================================================================ */

struct super_block *devtmpfs_create(void) {
    /* Create the root inode for devtmpfs */
    struct inode *root_inode = (struct inode *)kmalloc(sizeof(*root_inode));
    if (!root_inode) return NULL;
    memset(root_inode, 0, sizeof(*root_inode));

    struct devtmpfs_inode_data *root_data =
        (struct devtmpfs_inode_data *)kmalloc(sizeof(*root_data));
    if (!root_data) { kfree(root_inode); return NULL; }
    memset(root_data, 0, sizeof(*root_data));

    root_data->type = -1;  /* Root directory, not a device */

    root_inode->name = "";
    root_inode->priv = root_data;
    root_inode->is_dir = 1;
    root_inode->ops = &devtmpfs_dir_ops;

    /* Create the super block */
    struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
    if (!sb) { kfree(root_data); kfree(root_inode); return NULL; }
    memset(sb, 0, sizeof(*sb));

    sb->fs_name = "devtmpfs";
    sb->root = root_inode;
    sb->sb_data = NULL;

    return sb;
}

/* ================================================================
 * devtmpfs_init: Create devtmpfs and mount it at /dev
 * ================================================================ */

void devtmpfs_init(void) {
    struct super_block *dev_sb = devtmpfs_create();
    if (!dev_sb) {
        log_printf(LOG_LEVEL_ERR, "devtmpfs: failed to create super block\n");
        return;
    }

    if (vfs_mount("/dev", dev_sb) < 0) {
        log_printf(LOG_LEVEL_ERR, "devtmpfs: failed to mount at /dev\n");
        return;
    }

    log_printf(LOG_LEVEL_INFO, "devtmpfs: mounted at /dev\n");
}