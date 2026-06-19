/*
 * ramdisk.c - In-memory block device for testing filesystems
 *
 * Provides a simple RAM-backed block device that can be used
 * to test ext2 or other filesystems without real hardware.
 *
 * Default size: 16 MiB
 * Block size: 512 bytes
 */
#include "block_dev.h"
#include "include/log.h"
#include "mem.h"
#include <string.h>

#define RAMDISK_DEFAULT_MB  16

struct ramdisk_priv {
    uint8_t *data;       /* memory buffer */
    uint64_t size;       /* total size in bytes */
};

/* We store the ramdisk priv pointer globally so read/write can find it.
 * The read/write ops don't receive a bdev pointer. */
static struct ramdisk_priv *g_ramdisk_priv = NULL;
static uint32_t g_ramdisk_block_size = 512;

static int ramdisk_read(void *buf, uint64_t sector, int count) {
    if (!g_ramdisk_priv) return -1;

    uint64_t byte_offset = sector * g_ramdisk_block_size;
    uint64_t byte_count  = (uint64_t)count * g_ramdisk_block_size;

    if (byte_offset + byte_count > g_ramdisk_priv->size) return -1;

    memcpy(buf, g_ramdisk_priv->data + byte_offset, (size_t)byte_count);
    return 0;
}

static int ramdisk_write(const void *buf, uint64_t sector, int count) {
    if (!g_ramdisk_priv) return -1;

    uint64_t byte_offset = sector * g_ramdisk_block_size;
    uint64_t byte_count  = (uint64_t)count * g_ramdisk_block_size;

    if (byte_offset + byte_count > g_ramdisk_priv->size) return -1;

    memcpy(g_ramdisk_priv->data + byte_offset, buf, (size_t)byte_count);
    return 0;
}

/*
 * ramdisk_init: Create and register a RAM disk.
 * @size_mb: size in megabytes (0 = default 16 MiB)
 * Returns 0 on success, negative on error.
 */
int ramdisk_init(uint64_t size_mb) {
    struct block_device *bdev = (struct block_device *)kmalloc(sizeof(*bdev));
    if (!bdev) return -1;
    memset(bdev, 0, sizeof(*bdev));

    struct ramdisk_priv *priv = (struct ramdisk_priv *)kmalloc(sizeof(*priv));
    if (!priv) {
        kfree(bdev);
        return -1;
    }
    memset(priv, 0, sizeof(*priv));

    uint64_t size = size_mb ? size_mb * 1024 * 1024 :
                    (uint64_t)RAMDISK_DEFAULT_MB * 1024 * 1024;
    priv->data = (uint8_t *)kmalloc((size_t)size);
    if (!priv->data) {
        kfree(priv);
        kfree(bdev);
        return -1;
    }
    memset(priv->data, 0, (size_t)size);
    priv->size = size;

    /* Store in globals for the read/write callbacks */
    g_ramdisk_priv = priv;
    g_ramdisk_block_size = 512;

    /* Build the block device descriptor */
    strncpy(bdev->name, "ramdisk0", sizeof(bdev->name) - 1);
    bdev->name[sizeof(bdev->name) - 1] = '\0';
    bdev->block_size    = 512;
    bdev->total_sectors = size / 512;
    bdev->read          = ramdisk_read;
    bdev->write         = ramdisk_write;
    bdev->ioctl         = NULL;
    bdev->priv          = priv;

    return block_dev_register(bdev);
}