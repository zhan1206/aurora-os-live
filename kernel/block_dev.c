/*
 * block_dev.c - Block device abstraction layer implementation
 */
#include "block_dev.h"
#include "include/log.h"
#include "include/string.h"
#include "mem.h"
#include <stdint.h>

static struct block_device *block_dev_list = NULL;

int block_dev_register(struct block_device *bdev) {
    if (!bdev) return -1;

    bdev->next = block_dev_list;
    block_dev_list = bdev;

    log_printf(LOG_LEVEL_INFO,
               "block: registered '%s' (%u sectors of %u bytes)\n",
               bdev->name, (uint32_t)bdev->total_sectors, bdev->block_size);
    return 0;
}

struct block_device *block_dev_find(const char *name) {
    if (!name) return NULL;
    struct block_device *bdev = block_dev_list;
    while (bdev) {
        if (strcmp(bdev->name, name) == 0) return bdev;
        bdev = bdev->next;
    }
    return NULL;
}

int block_dev_read(struct block_device *bdev, void *buf, uint64_t sector,
                   int count) {
    if (!bdev || !bdev->read || !buf) return -1;
    if (count <= 0) return -1;
    /* Overflow-safe bounds check: sector + count could overflow */
    if (sector > bdev->total_sectors ||
        (uint64_t)count > bdev->total_sectors - sector) return -1;
    return bdev->read(buf, sector, count);
}

int block_dev_write(struct block_device *bdev, const void *buf,
                    uint64_t sector, int count) {
    if (!bdev || !bdev->write || !buf) return -1;
    if (count <= 0) return -1;
    if (sector > bdev->total_sectors ||
        (uint64_t)count > bdev->total_sectors - sector) return -1;
    return bdev->write(buf, sector, count);
}