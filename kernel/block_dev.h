/*
 * block_dev.h - Block device abstraction layer
 */
#ifndef BLOCK_DEV_H
#define BLOCK_DEV_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * Block Device Structure
 * ================================================================ */
struct block_device {
    char     name[32];
    uint32_t block_size;       /* usually 512 */
    uint64_t total_sectors;    /* device capacity in sectors */

    /* Operations: self is implicit; caller holds bdev pointer */
    int  (*read)(void *buf, uint64_t sector, int count);
    int  (*write)(const void *buf, uint64_t sector, int count);
    int  (*ioctl)(int cmd, void *arg);

    void *priv;                /* driver-private data */

    struct block_device *next;
};

/* ================================================================
 * Block Device API
 * ================================================================ */
int  block_dev_register(struct block_device *bdev);
struct block_device *block_dev_find(const char *name);
int  block_dev_read(struct block_device *bdev, void *buf, uint64_t sector,
                    int count);
int  block_dev_write(struct block_device *bdev, const void *buf, uint64_t sector,
                     int count);

/* ================================================================
 * RAM disk (for testing)
 * ================================================================ */
int ramdisk_init(uint64_t size_mb);

#endif /* BLOCK_DEV_H */