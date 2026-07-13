/*
 * ext2.c - Simplified ext2 filesystem driver for AuroraOS
 *
 * Implements a read/write ext2 driver that integrates with the VFS.
 * Supports:
 *   - Superblock reading and verification
 *   - Inode reading and writing
 *   - Block reading (direct + single indirect)
 *   - Block writing (direct blocks only)
 *   - Directory lookup and readdir
 *   - File creation in directories
 *   - File read/write operations
 *
 * Block device I/O is abstracted through struct block_device.
 */
#include "ext2.h"
#include "fs.h"
#include "block_dev.h"
#include "include/log.h"
#include "include/errno.h"
#include "mem.h"
#include <string.h>

/* ================================================================
 * Internal structures
 * ================================================================ */

/* Filesystem-private data attached to super_block */
struct ext2_sb_info {
    struct block_device *bdev;          /* backing block device */
    struct ext2_superblock *sb_raw;     /* cached raw superblock */
    uint32_t block_size;                /* actual block size (1024, 2048, 4096) */
    uint32_t blocks_per_group;          /* blocks per block group */
    uint32_t inodes_per_group;          /* inodes per block group */
    uint32_t inode_size;                /* size of each inode structure */
    uint32_t first_data_block;          /* first data block */
    uint32_t num_groups;                /* total number of block groups */
    struct ext2_group_desc *gd;         /* cached group descriptor table */
    uint32_t gd_blocks;                 /* number of blocks for the GD table */
};

/* Inode-private data attached to inode->priv */
struct ext2_inode_info {
    struct ext2_inode raw;              /* cached on-disk inode */
    uint32_t inode_num;                 /* inode number */
    struct ext2_sb_info *sbi;           /* pointer to fs-private data */
};

/* ================================================================
 * Block device helpers
 *
 * These convert filesystem-relative block numbers to device-relative
 * block numbers, accounting for the block size difference.
 * ================================================================ */

static int read_block(struct block_device *bdev, uint32_t block_size,
                      uint32_t block_num, void *buf) {
    uint32_t sectors_per_block = block_size / bdev->block_size;
    uint64_t sector = (uint64_t)block_num * sectors_per_block;
    return block_dev_read(bdev, buf, sector, sectors_per_block);
}

static int write_block(struct block_device *bdev, uint32_t block_size,
                       uint32_t block_num, const void *buf) {
    uint32_t sectors_per_block = block_size / bdev->block_size;
    uint64_t sector = (uint64_t)block_num * sectors_per_block;
    return block_dev_write(bdev, buf, sector, sectors_per_block);
}

/* ================================================================
 * Internal helpers
 * ================================================================ */

/*
 * Round up to the next multiple of 4 (required for directory entry lengths).
 */
static inline uint16_t rec_len_for_name(uint8_t name_len) {
    uint16_t len = (uint16_t)(sizeof(struct ext2_dir_entry) + name_len);
    return (uint16_t)((len + 3) & ~3u);
}

/*
 * Get the block group number containing a given inode.
 */
static inline uint32_t inode_group(struct ext2_sb_info *sbi, uint32_t ino) {
    return (ino - 1) / sbi->inodes_per_group;
}

/*
 * Get the index within the group for a given inode.
 */
static inline uint32_t inode_index(struct ext2_sb_info *sbi, uint32_t ino) {
    return (ino - 1) % sbi->inodes_per_group;
}

/*
 * Read an inode from disk.
 * @sbi: filesystem private data
 * @inum: inode number (1-based)
 * @raw: output buffer for the raw inode
 * Returns 0 on success, negative on error.
 */
static int ext2_read_inode_raw(struct ext2_sb_info *sbi, uint32_t inum,
                               struct ext2_inode *raw) {
    if (inum == 0) return -EINVAL;

    uint32_t group = inode_group(sbi, inum);
    uint32_t index = inode_index(sbi, inum);

    if (group >= sbi->num_groups) return -EINVAL;

    uint32_t inode_table_block = sbi->gd[group].bg_inode_table;
    uint32_t inodes_per_block = sbi->block_size / sbi->inode_size;
    uint32_t block_offset = index / inodes_per_block;
    uint32_t offset_in_block = (index % inodes_per_block) * sbi->inode_size;

    uint8_t *block_buf = (uint8_t *)kmalloc(sbi->block_size);
    if (!block_buf) return -ENOMEM;

    if (read_block(sbi->bdev, sbi->block_size,
                   inode_table_block + block_offset, block_buf) < 0) {
        kfree(block_buf);
        return -EIO;
    }

    memcpy(raw, block_buf + offset_in_block, sbi->inode_size);
    kfree(block_buf);
    return 0;
}

/*
 * Write an inode back to disk.
 */
static int ext2_write_inode_raw(struct ext2_sb_info *sbi, uint32_t inum,
                                const struct ext2_inode *raw) {
    if (inum == 0) return -EINVAL;

    uint32_t group = inode_group(sbi, inum);
    uint32_t index = inode_index(sbi, inum);

    if (group >= sbi->num_groups) return -EINVAL;

    uint32_t inode_table_block = sbi->gd[group].bg_inode_table;
    uint32_t inodes_per_block = sbi->block_size / sbi->inode_size;
    uint32_t block_offset = index / inodes_per_block;
    uint32_t offset_in_block = (index % inodes_per_block) * sbi->inode_size;

    uint8_t *block_buf = (uint8_t *)kmalloc(sbi->block_size);
    if (!block_buf) return -ENOMEM;

    /* Read the entire block first, then modify the inode portion */
    if (read_block(sbi->bdev, sbi->block_size,
                   inode_table_block + block_offset, block_buf) < 0) {
        kfree(block_buf);
        return -EIO;
    }

    memcpy(block_buf + offset_in_block, raw, sbi->inode_size);

    if (write_block(sbi->bdev, sbi->block_size,
                    inode_table_block + block_offset, block_buf) < 0) {
        kfree(block_buf);
        return -EIO;
    }

    kfree(block_buf);
    return 0;
}

/*
 * Read a data block from the filesystem.
 * Handles direct blocks, single indirect, and double indirect blocks.
 * @sbi: filesystem private data
 * @raw: the inode to read from
 * @logical_block: zero-based logical block index within the file
 * @buf: output buffer (must be at least block_size bytes)
 * Returns 0 on success, negative on error.
 */
static int ext2_read_data_block(struct ext2_sb_info *sbi,
                                const struct ext2_inode *raw,
                                uint32_t logical_block, void *buf) {
    uint32_t block_size = sbi->block_size;
    uint32_t ptrs_per_block = block_size / 4;

    /* Direct blocks */
    if (logical_block < EXT2_NDIR_BLOCKS) {
        uint32_t blk = raw->i_block[logical_block];
        if (blk == 0) return -EIO; /* sparse block not yet allocated */
        return read_block(sbi->bdev, block_size, blk, buf);
    }

    /* Single indirect */
    logical_block -= EXT2_NDIR_BLOCKS;
    if (logical_block < ptrs_per_block) {
        uint32_t ind_blk = raw->i_block[EXT2_IND_BLOCK];
        if (ind_blk == 0) return -EIO;

        uint32_t *ind_buf = (uint32_t *)kmalloc(block_size);
        if (!ind_buf) return -ENOMEM;

        if (read_block(sbi->bdev, block_size, ind_blk, ind_buf) < 0) {
            kfree(ind_buf);
            return -EIO;
        }

        uint32_t blk = ind_buf[logical_block];
        kfree(ind_buf);

        if (blk == 0) return -EIO;
        return read_block(sbi->bdev, block_size, blk, buf);
    }

    /* Double indirect */
    logical_block -= ptrs_per_block;
    if (logical_block < ptrs_per_block * ptrs_per_block) {
        uint32_t dind_blk = raw->i_block[EXT2_DIND_BLOCK];
        if (dind_blk == 0) return -EIO;

        /* Read the double-indirect block (array of single-indirect pointers) */
        uint32_t *dind_buf = (uint32_t *)kmalloc(block_size);
        if (!dind_buf) return -ENOMEM;

        if (read_block(sbi->bdev, block_size, dind_blk, dind_buf) < 0) {
            kfree(dind_buf);
            return -EIO;
        }

        /* Index into the double-indirect array */
        uint32_t dind_idx = logical_block / ptrs_per_block;
        uint32_t ind_idx  = logical_block % ptrs_per_block;

        uint32_t ind_blk = dind_buf[dind_idx];
        if (ind_blk == 0) {
            kfree(dind_buf);
            return -EIO;
        }

        /* Read the single-indirect block */
        uint32_t *ind_buf = (uint32_t *)kmalloc(block_size);
        if (!ind_buf) {
            kfree(dind_buf);
            return -ENOMEM;
        }

        int ret = read_block(sbi->bdev, block_size, ind_blk, ind_buf);
        kfree(dind_buf);

        if (ret < 0) {
            kfree(ind_buf);
            return -EIO;
        }

        uint32_t blk = ind_buf[ind_idx];
        kfree(ind_buf);

        if (blk == 0) return -EIO;
        return read_block(sbi->bdev, block_size, blk, buf);
    }

    /* Triple indirect not supported (files > ~4GB are rare for this OS) */
    return -ENOSYS;
}

/*
 * Write a data block to the filesystem.
 * Handles direct blocks and single indirect blocks.
 * Double indirect write is not yet supported (requires block allocation).
 */
static int ext2_write_data_block(struct ext2_sb_info *sbi,
                                 struct ext2_inode *raw,
                                 uint32_t logical_block, const void *buf) {
    uint32_t block_size = sbi->block_size;
    uint32_t ptrs_per_block = block_size / 4;

    if (logical_block < EXT2_NDIR_BLOCKS) {
        uint32_t blk = raw->i_block[logical_block];
        if (blk == 0) return -EIO; /* block not allocated */
        return write_block(sbi->bdev, block_size, blk, buf);
    }

    /* Single indirect write */
    logical_block -= EXT2_NDIR_BLOCKS;
    if (logical_block < ptrs_per_block) {
        uint32_t ind_blk = raw->i_block[EXT2_IND_BLOCK];
        if (ind_blk == 0) return -EIO;

        uint32_t *ind_buf = (uint32_t *)kmalloc(block_size);
        if (!ind_buf) return -ENOMEM;

        if (read_block(sbi->bdev, block_size, ind_blk, ind_buf) < 0) {
            kfree(ind_buf);
            return -EIO;
        }

        uint32_t blk = ind_buf[logical_block];
        if (blk == 0) {
            kfree(ind_buf);
            return -EIO;
        }

        int ret = write_block(sbi->bdev, block_size, blk, buf);
        kfree(ind_buf);
        return ret;
    }

    /* Double/triple indirect write not supported */
    return -ENOSYS;
}

/*
 * Allocate a new block (simplified: just returns the next free block).
 * In a real implementation this would search the block bitmap.
 * For now, we use a simple counter.
 */
static uint32_t ext2_alloc_block(struct ext2_sb_info *sbi) {
    /* Find the last allocated block by scanning block bitmaps */
    /* This is a simplistic approach — for real use, maintain free lists */
    uint32_t group = 0;
    for (group = 0; group < sbi->num_groups; group++) {
        if (sbi->gd[group].bg_free_blocks_count > 0) {
            /* Read the block bitmap to find a free block */
            uint32_t bitmap_block = sbi->gd[group].bg_block_bitmap;
            uint8_t *bitmap = (uint8_t *)kmalloc(sbi->block_size);
            if (!bitmap) return 0;

            if (read_block(sbi->bdev, sbi->block_size, bitmap_block, bitmap) < 0) {
                kfree(bitmap);
                return 0;
            }

            uint32_t blocks_in_group = sbi->blocks_per_group;
            uint32_t found = 0;
            for (uint32_t i = 0; i < blocks_in_group && i < sbi->block_size * 8; i++) {
                uint32_t byte_idx = i / 8;
                uint32_t bit_idx  = i % 8;
                if (!(bitmap[byte_idx] & (1u << bit_idx))) {
                    /* Found a free block — mark it used */
                    bitmap[byte_idx] |= (uint8_t)(1u << bit_idx);
                    write_block(sbi->bdev, sbi->block_size, bitmap_block, bitmap);
                    sbi->gd[group].bg_free_blocks_count--;
                    found = group * sbi->blocks_per_group + i + sbi->first_data_block;
                    kfree(bitmap);
                    return found;
                }
            }
            kfree(bitmap);
        }
    }
    return 0; /* no free blocks */
}

/*
 * Allocate a new inode.
 */
static uint32_t ext2_alloc_inode(struct ext2_sb_info *sbi) {
    uint32_t group;
    for (group = 0; group < sbi->num_groups; group++) {
        if (sbi->gd[group].bg_free_inodes_count > 0) {
            uint32_t bitmap_block = sbi->gd[group].bg_inode_bitmap;
            uint8_t *bitmap = (uint8_t *)kmalloc(sbi->block_size);
            if (!bitmap) return 0;

            if (read_block(sbi->bdev, sbi->block_size, bitmap_block, bitmap) < 0) {
                kfree(bitmap);
                return 0;
            }

            uint32_t inodes_in_group = sbi->inodes_per_group;
            for (uint32_t i = 0; i < inodes_in_group && i < sbi->block_size * 8; i++) {
                uint32_t byte_idx = i / 8;
                uint32_t bit_idx  = i % 8;
                if (!(bitmap[byte_idx] & (1u << bit_idx))) {
                    bitmap[byte_idx] |= (uint8_t)(1u << bit_idx);
                    write_block(sbi->bdev, sbi->block_size, bitmap_block, bitmap);
                    sbi->gd[group].bg_free_inodes_count--;
                    kfree(bitmap);
                    return group * sbi->inodes_per_group + i + 1;
                }
            }
            kfree(bitmap);
        }
    }
    return 0;
}

/*
 * Free an allocated block (used for cleanup on error paths).
 */
static void ext2_free_block(struct ext2_sb_info *sbi, uint32_t blk) {
    if (blk == 0) return;
    uint32_t group = (blk - sbi->first_data_block) / sbi->blocks_per_group;
    uint32_t index = (blk - sbi->first_data_block) % sbi->blocks_per_group;
    if (group >= sbi->num_groups) return;

    uint32_t bitmap_block = sbi->gd[group].bg_block_bitmap;
    uint8_t *bitmap = (uint8_t *)kmalloc(sbi->block_size);
    if (!bitmap) return;

    if (read_block(sbi->bdev, sbi->block_size, bitmap_block, bitmap) < 0) {
        kfree(bitmap);
        return;
    }

    uint32_t byte_idx = index / 8;
    uint32_t bit_idx  = index % 8;
    bitmap[byte_idx] &= (uint8_t)~(1u << bit_idx);
    write_block(sbi->bdev, sbi->block_size, bitmap_block, bitmap);
    sbi->gd[group].bg_free_blocks_count++;
    kfree(bitmap);
}

/*
 * Free an allocated inode (used for cleanup on error paths).
 */
static void ext2_free_inode(struct ext2_sb_info *sbi, uint32_t ino) {
    if (ino == 0) return;
    uint32_t group = (ino - 1) / sbi->inodes_per_group;
    uint32_t index = (ino - 1) % sbi->inodes_per_group;
    if (group >= sbi->num_groups) return;

    uint32_t bitmap_block = sbi->gd[group].bg_inode_bitmap;
    uint8_t *bitmap = (uint8_t *)kmalloc(sbi->block_size);
    if (!bitmap) return;

    if (read_block(sbi->bdev, sbi->block_size, bitmap_block, bitmap) < 0) {
        kfree(bitmap);
        return;
    }

    uint32_t byte_idx = index / 8;
    uint32_t bit_idx  = index % 8;
    bitmap[byte_idx] &= (uint8_t)~(1u << bit_idx);
    write_block(sbi->bdev, sbi->block_size, bitmap_block, bitmap);
    sbi->gd[group].bg_free_inodes_count++;
    kfree(bitmap);
}

/* ================================================================
 * VFS file operations for ext2 files
 * ================================================================ */

static int ext2_file_open(struct inode *inode, struct file *filp) {
    (void)filp;
    if (!inode || !inode->priv) return -EINVAL;
    return 0;
}

static ssize_t ext2_file_read(struct file *filp, void *buf, size_t count,
                              off_t *offset) {
    if (!filp || !filp->inode || !filp->inode->priv) return -EINVAL;
    if (!buf || !offset) return -EINVAL;

    struct ext2_inode_info *info = (struct ext2_inode_info *)filp->inode->priv;
    struct ext2_sb_info *sbi = info->sbi;
    uint32_t file_size = info->raw.i_size;
    uint32_t block_size = sbi->block_size;

    if (*offset >= (off_t)file_size) return 0;

    size_t total_read = 0;
    uint8_t *block_buf = (uint8_t *)kmalloc(block_size);
    if (!block_buf) return -ENOMEM;

    while (count > 0 && *offset < (off_t)file_size) {
        uint32_t logical_block = (uint32_t)(*offset / block_size);
        uint32_t block_offset  = (uint32_t)(*offset % block_size);

        if (ext2_read_data_block(sbi, &info->raw, logical_block, block_buf) < 0) {
            kfree(block_buf);
            return total_read > 0 ? (ssize_t)total_read : -EIO;
        }

        size_t chunk = (size_t)(block_size - block_offset);
        if (chunk > count) chunk = count;
        if ((size_t)(*offset) + chunk > (size_t)file_size)
            chunk = (size_t)(file_size - (size_t)(*offset));

        memcpy((uint8_t *)buf + total_read, block_buf + block_offset, chunk);
        *offset += (off_t)chunk;
        total_read += chunk;
        count -= chunk;
    }

    kfree(block_buf);
    return (ssize_t)total_read;
}

static ssize_t ext2_file_write(struct file *filp, const void *buf, size_t count,
                               off_t *offset) {
    if (!filp || !filp->inode || !filp->inode->priv) return -EINVAL;
    if (!buf || !offset) return -EINVAL;

    struct ext2_inode_info *info = (struct ext2_inode_info *)filp->inode->priv;
    struct ext2_sb_info *sbi = info->sbi;
    uint32_t block_size = sbi->block_size;

    /* Only support writing within the first 12 direct blocks */
    uint32_t max_blocks = EXT2_NDIR_BLOCKS;
    uint32_t max_offset = max_blocks * block_size;

    if (*offset >= (off_t)max_offset) return -ENOSPC;

    size_t total_written = 0;
    uint8_t *block_buf = (uint8_t *)kmalloc(block_size);
    if (!block_buf) return -ENOMEM;

    while (count > 0 && *offset < (off_t)max_offset) {
        uint32_t logical_block = (uint32_t)(*offset / block_size);
        uint32_t block_offset  = (uint32_t)(*offset % block_size);

        /* Allocate block if needed */
        if (info->raw.i_block[logical_block] == 0) {
            uint32_t new_blk = ext2_alloc_block(sbi);
            if (new_blk == 0) {
                kfree(block_buf);
                return total_written > 0 ? (ssize_t)total_written : -ENOSPC;
            }
            info->raw.i_block[logical_block] = new_blk;
            info->raw.i_blocks += (block_size / 512);
            /* Zero the newly allocated block */
            memset(block_buf, 0, block_size);
            write_block(sbi->bdev, block_size, new_blk, block_buf);
        }

        /* Read-modify-write for partial block writes */
        if (block_offset != 0 || count < (size_t)block_size) {
            if (ext2_read_data_block(sbi, &info->raw, logical_block, block_buf) < 0) {
                kfree(block_buf);
                return total_written > 0 ? (ssize_t)total_written : -EIO;
            }
        }

        size_t chunk = (size_t)(block_size - block_offset);
        if (chunk > count) chunk = count;

        memcpy(block_buf + block_offset, (const uint8_t *)buf + total_written, chunk);

        if (ext2_write_data_block(sbi, &info->raw, logical_block, block_buf) < 0) {
            kfree(block_buf);
            return total_written > 0 ? (ssize_t)total_written : -EIO;
        }

        *offset += (off_t)chunk;
        total_written += chunk;
        count -= chunk;
    }

    /* Update file size if we wrote past the end */
    if ((uint32_t)(*offset) > info->raw.i_size)
        info->raw.i_size = (uint32_t)(*offset);

    /* Write updated inode back to disk */
    int ret = ext2_write_inode_raw(sbi, info->inode_num, &info->raw);
    if (ret < 0) {
        log_printf(LOG_LEVEL_ERR, "ext2: write_inode_raw failed for inode %u (ret=%d)\n",
                   info->inode_num, ret);
    }

    kfree(block_buf);
    return (ssize_t)total_written;
}

static int ext2_file_close(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp;
    return 0;
}

/* Forward declarations */
static struct file_ops ext2_file_ops;
static struct file_ops ext2_dir_ops;

static int ext2_dir_lookup(struct inode *dir, struct dentry *dentry) {
    if (!dir || !dir->priv || !dentry) return -EINVAL;

    struct ext2_inode_info *info = (struct ext2_inode_info *)dir->priv;
    struct ext2_sb_info *sbi = info->sbi;
    uint32_t block_size = sbi->block_size;
    uint32_t dir_size = info->raw.i_size;
    const char *lookup_name = dentry->name;
    size_t lookup_len = strlen(lookup_name);

    uint8_t *block_buf = (uint8_t *)kmalloc(block_size);
    if (!block_buf) return -ENOMEM;

    /* Iterate through directory blocks */
    for (uint32_t off = 0; off < dir_size; ) {
        uint32_t logical_block = off / block_size;
        uint32_t block_offset  = off % block_size;

        if (ext2_read_data_block(sbi, &info->raw, logical_block, block_buf) < 0) {
            kfree(block_buf);
            return -EIO;
        }

        /* Parse directory entries */
        while (block_offset < block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + block_offset);
            if (de->inode == 0 && de->rec_len == 0) {
                /* End of directory */
                kfree(block_buf);
                return -ENOENT;
            }
            if (de->rec_len == 0) {
                kfree(block_buf);
                return -ENOENT;
            }

            if (de->name_len == lookup_len &&
                strncmp(de->name, lookup_name, lookup_len) == 0) {
                /* Found the entry — create an inode for it */
                struct ext2_inode_info *child_info = (struct ext2_inode_info *)
                    kmalloc(sizeof(*child_info));
                if (!child_info) {
                    kfree(block_buf);
                    return -ENOMEM;
                }
                memset(child_info, 0, sizeof(*child_info));
                child_info->inode_num = de->inode;
                child_info->sbi = sbi;

                if (ext2_read_inode_raw(sbi, de->inode, &child_info->raw) < 0) {
                    kfree(child_info);
                    kfree(block_buf);
                    return -EIO;
                }

                struct inode *child_inode = (struct inode *)kmalloc(sizeof(*child_inode));
                if (!child_inode) {
                    kfree(child_info);
                    kfree(block_buf);
                    return -ENOMEM;
                }
                memset(child_inode, 0, sizeof(*child_inode));

                /* Copy the name */
                char *name_copy = (char *)kmalloc(de->name_len + 1);
                if (!name_copy) {
                    kfree(child_inode);
                    kfree(child_info);
                    kfree(block_buf);
                    return -ENOMEM;
                }
                memcpy(name_copy, de->name, de->name_len);
                name_copy[de->name_len] = '\0';

                child_inode->name  = name_copy;
                child_inode->priv  = child_info;
                child_inode->dentry = dentry;

                /* Assign operations based on type */
                if (child_info->raw.i_mode & EXT2_S_IFDIR) {
                    child_inode->ops    = &ext2_dir_ops;
                    child_inode->is_dir = 1;
                } else {
                    child_inode->ops    = &ext2_file_ops;
                    child_inode->is_dir = 0;
                }
                dentry->inode = child_inode;

                kfree(block_buf);
                return 0;
            }

            off += de->rec_len;
            block_offset += de->rec_len;
        }
    }

    kfree(block_buf);
    return -ENOENT;
}

/* ================================================================
 * VFS file operations tables
 * ================================================================ */

static struct file_ops ext2_file_ops = {
    .open   = ext2_file_open,
    .read   = ext2_file_read,
    .write  = ext2_file_write,
    .close  = ext2_file_close,
    .lookup = NULL,
};

static struct file_ops ext2_dir_ops = {
    .open   = ext2_file_open,
    .read   = NULL,    /* directories use readdir, not read */
    .write  = NULL,
    .close  = ext2_file_close,
    .lookup = ext2_dir_lookup,
};

/* ================================================================
 * Public API
 * ================================================================ */

/*
 * ext2_read_inode_sbi: Read an inode given an sbi directly.
 * Internal helper used during mount (before super_block is ready).
 */
static struct inode *ext2_read_inode_sbi(struct ext2_sb_info *sbi, uint32_t inum) {
    struct ext2_inode_info *info = (struct ext2_inode_info *)
        kmalloc(sizeof(*info));
    if (!info) return NULL;
    memset(info, 0, sizeof(*info));
    info->inode_num = inum;
    info->sbi = sbi;

    if (ext2_read_inode_raw(sbi, inum, &info->raw) < 0) {
        kfree(info);
        return NULL;
    }

    struct inode *inode = (struct inode *)kmalloc(sizeof(*inode));
    if (!inode) {
        kfree(info);
        return NULL;
    }
    memset(inode, 0, sizeof(*inode));

    inode->name  = NULL;
    inode->priv  = info;
    inode->ops   = (info->raw.i_mode & EXT2_S_IFDIR) ? &ext2_dir_ops : &ext2_file_ops;
    inode->is_dir = (info->raw.i_mode & EXT2_S_IFDIR) ? 1 : 0;

    return inode;
}

/*
 * ext2_read_inode: Read an inode from the filesystem and populate
 * a VFS inode structure. Used by external callers.
 * @sb: super_block
 * @inum: inode number (1-based)
 * Returns a VFS inode on success, NULL on failure.
 */
struct inode *ext2_read_inode(struct super_block *sb, uint32_t inum) {
    if (!sb || !sb->sb_data) return NULL;
    struct ext2_sb_info *sbi = (struct ext2_sb_info *)sb->sb_data;
    return ext2_read_inode_sbi(sbi, inum);
}

/*
 * ext2_readdir: Read directory entries.
 * @dir: VFS inode for the directory
 * @buf: output buffer for directory entries
 * @bufsize: size of output buffer
 * @offset: position in the directory (incremented by caller)
 * Returns number of bytes written, 0 for EOF, negative on error.
 *
 * Output format: each entry is a ext2_dir_entry structure.
 */
ssize_t ext2_readdir(struct inode *dir, void *buf, size_t bufsize,
                     off_t *offset) {
    if (!dir || !dir->priv || !buf || !offset) return -EINVAL;

    struct ext2_inode_info *info = (struct ext2_inode_info *)dir->priv;
    struct ext2_sb_info *sbi = info->sbi;
    uint32_t block_size = sbi->block_size;
    uint32_t dir_size = info->raw.i_size;

    if (*offset >= (off_t)dir_size) return 0;

    uint8_t *block_buf = (uint8_t *)kmalloc(block_size);
    if (!block_buf) return -ENOMEM;

    size_t total = 0;
    uint32_t pos = (uint32_t)(*offset);

    while (pos < dir_size && total < bufsize) {
        uint32_t logical_block = pos / block_size;
        uint32_t block_offset  = pos % block_size;

        if (ext2_read_data_block(sbi, &info->raw, logical_block, block_buf) < 0) {
            kfree(block_buf);
            return total > 0 ? (ssize_t)total : -EIO;
        }

        while (block_offset < block_size && total < bufsize) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + block_offset);

            if (de->inode == 0 && de->rec_len == 0) {
                /* End of directory */
                kfree(block_buf);
                *offset = (off_t)pos;
                return (ssize_t)total;
            }
            if (de->rec_len == 0) {
                kfree(block_buf);
                *offset = (off_t)pos;
                return (ssize_t)total;
            }

            uint16_t entry_size = rec_len_for_name(de->name_len);
            if (total + entry_size > bufsize) {
                /* Buffer full — stop here */
                kfree(block_buf);
                *offset = (off_t)pos;
                return (ssize_t)total;
            }

            /* Copy the entry to user buffer */
            struct ext2_dir_entry *out = (struct ext2_dir_entry *)((uint8_t *)buf + total);
            out->inode    = de->inode;
            out->rec_len  = entry_size;
            out->name_len = de->name_len;
            out->file_type = de->file_type;
            memcpy(out->name, de->name, de->name_len);

            total += entry_size;
            pos += de->rec_len;
            block_offset += de->rec_len;
        }
    }

    kfree(block_buf);
    *offset = (off_t)pos;
    return (ssize_t)total;
}

/*
 * ext2_create: Create a new file in a directory.
 * @sb: super_block of the filesystem
 * @dir: VFS inode for the parent directory
 * @name: name of the new file
 * @mode: file type (EXT2_S_IFREG or EXT2_S_IFDIR)
 * Returns the new inode on success, NULL on failure.
 */
struct inode *ext2_create(struct super_block *sb, struct inode *dir,
                          const char *name, uint16_t mode) {
    if (!sb || !dir || !dir->priv || !name) return NULL;

    struct ext2_inode_info *dir_info = (struct ext2_inode_info *)dir->priv;
    struct ext2_sb_info *sbi = dir_info->sbi;
    uint32_t block_size = sbi->block_size;
    uint8_t name_len = (uint8_t)strlen(name);
    /* name_len is uint8_t, so < 256 always; check if name is too long via strlen */
    if (strlen(name) > 255) return NULL;

    uint32_t new_ino = 0;
    uint32_t new_blk = 0;
    uint8_t *block_buf = NULL;

    /* Allocate a new inode */
    new_ino = ext2_alloc_inode(sbi);
    if (new_ino == 0) return NULL;

    /* Initialize the new inode on disk */
    struct ext2_inode new_raw;
    memset(&new_raw, 0, sizeof(new_raw));
    new_raw.i_mode  = mode | EXT2_S_IRUSR | EXT2_S_IWUSR | EXT2_S_IRGRP | EXT2_S_IROTH;
    new_raw.i_uid   = 0;
    new_raw.i_gid   = 0;
    new_raw.i_size  = 0;
    new_raw.i_blocks = 0;
    new_raw.i_links_count = 1;

    if (ext2_write_inode_raw(sbi, new_ino, &new_raw) < 0) goto out_free_inode;

    /* Add a directory entry in the parent directory */
    uint32_t dir_size = dir_info->raw.i_size;
    uint16_t new_rec_len = rec_len_for_name(name_len);
    block_buf = (uint8_t *)kmalloc(block_size);
    if (!block_buf) goto out_free_inode;

    int found_slot = 0;
    uint32_t new_offset = dir_size;

    if (dir_size > 0) {
        uint32_t last_block_idx = (dir_size - 1) / block_size;

        if (last_block_idx < EXT2_NDIR_BLOCKS &&
            dir_info->raw.i_block[last_block_idx] != 0) {

            if (read_block(sbi->bdev, block_size,
                           dir_info->raw.i_block[last_block_idx],
                           block_buf) < 0) {
                goto out_free_inode;
            }

            /* Scan the last block for a slot */
            uint32_t off = 0;
            while (off < block_size) {
                struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + off);
                if (de->rec_len == 0) break;

                uint16_t min_rec = rec_len_for_name(de->name_len);
                if (de->rec_len >= min_rec + new_rec_len) {
                    /* Split this entry */
                    uint16_t old_rec_len = de->rec_len;
                    de->rec_len = min_rec;

                    struct ext2_dir_entry *new_de =
                        (struct ext2_dir_entry *)(block_buf + off + min_rec);
                    new_de->inode     = new_ino;
                    new_de->rec_len   = (uint16_t)(old_rec_len - min_rec);
                    new_de->name_len  = name_len;
                    new_de->file_type = (mode & EXT2_S_IFDIR) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
                    memcpy(new_de->name, name, name_len);

                    write_block(sbi->bdev, block_size,
                                dir_info->raw.i_block[last_block_idx], block_buf);
                    found_slot = 1;
                    break;
                }
                off += de->rec_len;
            }

            if (!found_slot) {
                new_offset = ((last_block_idx + 1) * block_size);
            }
        }
    }

    if (!found_slot) {
        /* Append a new entry */
        uint32_t blk_idx = new_offset / block_size;
        uint32_t blk_off = new_offset % block_size;

        if (blk_off == 0) {
            /* Need a new block */
            new_blk = ext2_alloc_block(sbi);
            if (new_blk == 0) goto out_free_inode;
            if (blk_idx >= EXT2_NDIR_BLOCKS) goto out_free_block;
            dir_info->raw.i_block[blk_idx] = new_blk;
            dir_info->raw.i_blocks += (block_size / 512);
            memset(block_buf, 0, block_size);
        } else {
            /* Read the existing block */
            if (read_block(sbi->bdev, block_size,
                           dir_info->raw.i_block[blk_idx], block_buf) < 0) {
                goto out_free_inode;
            }
        }

        struct ext2_dir_entry *new_de =
            (struct ext2_dir_entry *)(block_buf + blk_off);
        new_de->inode     = new_ino;
        new_de->rec_len   = (uint16_t)(block_size - blk_off);
        new_de->name_len  = name_len;
        new_de->file_type = (mode & EXT2_S_IFDIR) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
        memcpy(new_de->name, name, name_len);

        write_block(sbi->bdev, block_size, dir_info->raw.i_block[blk_idx], block_buf);

        dir_info->raw.i_size = new_offset + new_rec_len;
    }

    /* Update parent directory inode */
    ext2_write_inode_raw(sbi, dir_info->inode_num, &dir_info->raw);

    if (mode & EXT2_S_IFDIR) {
        dir_info->raw.i_links_count++;
        ext2_write_inode_raw(sbi, dir_info->inode_num, &dir_info->raw);
    }

    kfree(block_buf);

    /* Build the VFS inode for the new file */
    return ext2_read_inode(sb, new_ino);

out_free_block:
    ext2_free_block(sbi, new_blk);
out_free_inode:
    ext2_free_inode(sbi, new_ino);
    kfree(block_buf);
    return NULL;
}

/* ================================================================
 * ext2_mount: Mount an ext2 filesystem
 * ================================================================ */

struct super_block *ext2_mount(struct block_device *bdev) {
    if (!bdev) return NULL;

    log_printf(LOG_LEVEL_INFO, "ext2: mounting from '%s'\n", bdev->name);

    /* Allocate filesystem-private data */
    struct ext2_sb_info *sbi = (struct ext2_sb_info *)kmalloc(sizeof(*sbi));
    if (!sbi) return NULL;
    memset(sbi, 0, sizeof(*sbi));
    sbi->bdev = bdev;

    /* Read the superblock (offset 1024 bytes from start of filesystem) */
    /* The superblock is always 1024 bytes, regardless of block size */
    /* For block_size >= 1024, the superblock is in block 1 */
    /* For block_size == 512 (not used in ext2), it would be in block 2 */
    uint8_t *sb_buf = (uint8_t *)kmalloc(1024);
    if (!sb_buf) {
        kfree(sbi);
        return NULL;
    }

    /* Read the superblock at offset 1024.
     * With 512-byte sectors, the superblock starts at sector 2.
     * Read 2 sectors (1024 bytes) starting at sector 2. */
    if (block_dev_read(bdev, sb_buf, 2, 2) < 0) {
        /* Try reading sector 1 in case of 1024-byte sectors */
        if (block_dev_read(bdev, sb_buf, 1, 1) < 0) {
            /* Last resort: read 2048 bytes from sector 0, extract SB */
            uint8_t *temp = (uint8_t *)kmalloc(2048);
            if (!temp) {
                kfree(sb_buf);
                kfree(sbi);
                return NULL;
            }
            if (block_dev_read(bdev, temp, 0, 4) < 0) {
                kfree(temp);
                kfree(sb_buf);
                kfree(sbi);
                return NULL;
            }
            memcpy(sb_buf, temp + 1024, 1024);
            kfree(temp);
        }
    }

    struct ext2_superblock *sb_raw = (struct ext2_superblock *)sb_buf;
    sbi->sb_raw = (struct ext2_superblock *)kmalloc(sizeof(*sb_raw));
    if (!sbi->sb_raw) {
        kfree(sb_buf);
        kfree(sbi);
        return NULL;
    }
    memcpy(sbi->sb_raw, sb_raw, sizeof(*sb_raw));
    kfree(sb_buf);

    /* Verify magic */
    if (sbi->sb_raw->s_magic != EXT2_SUPER_MAGIC) {
        log_printf(LOG_LEVEL_ERR,
                   "ext2: bad magic 0x%04x (expected 0x%04x)\n",
                   sbi->sb_raw->s_magic, EXT2_SUPER_MAGIC);
        kfree(sbi->sb_raw);
        kfree(sbi);
        return NULL;
    }

    /* Compute block size: 1024 << s_log_block_size */
    sbi->block_size = (uint32_t)(1024 << sbi->sb_raw->s_log_block_size);
    if (sbi->block_size < EXT2_MIN_BLOCK_SIZE ||
        sbi->block_size > EXT2_MAX_BLOCK_SIZE) {
        log_printf(LOG_LEVEL_ERR, "ext2: unsupported block size %u\n",
                   sbi->block_size);
        kfree(sbi->sb_raw);
        kfree(sbi);
        return NULL;
    }

    sbi->blocks_per_group = sbi->sb_raw->s_blocks_per_group;
    sbi->inodes_per_group = sbi->sb_raw->s_inodes_per_group;
    sbi->inode_size       = sbi->sb_raw->s_inode_size;
    if (sbi->inode_size == 0) sbi->inode_size = 128;

    /* Validate critical fields: zero blocks_per_group or inodes_per_group
     * indicate a corrupted or malicious superblock. Division by zero in
     * group count computation would trigger a panic. */
    if (sbi->blocks_per_group == 0 || sbi->inodes_per_group == 0) {
        log_printf(LOG_LEVEL_ERR, "ext2: corrupted superblock: blocks_per_group=%u inodes_per_group=%u\n",
                   sbi->blocks_per_group, sbi->inodes_per_group);
        kfree(sbi->sb_raw);
        kfree(sbi);
        return NULL;
    }
    sbi->first_data_block = sbi->sb_raw->s_first_data_block;

    /* Compute number of block groups */
    uint32_t total_blocks = sbi->sb_raw->s_blocks_count;
    sbi->num_groups = (total_blocks + sbi->blocks_per_group - 1) /
                      sbi->blocks_per_group;

    /* Load the group descriptor table */
    sbi->gd_blocks = (sbi->num_groups * sizeof(struct ext2_group_desc) +
                      sbi->block_size - 1) / sbi->block_size;
    sbi->gd = (struct ext2_group_desc *)kmalloc(sbi->gd_blocks * sbi->block_size);
    if (!sbi->gd) {
        kfree(sbi->sb_raw);
        kfree(sbi);
        return NULL;
    }

    /* The GD table starts at block 2 (block 0 = boot, block 1 = superblock) */
    uint32_t gd_start = (sbi->block_size == 1024) ? 2 : 1;
    for (uint32_t i = 0; i < sbi->gd_blocks; i++) {
        if (read_block(bdev, sbi->block_size, gd_start + i,
                       (uint8_t *)sbi->gd + i * sbi->block_size) < 0) {
            log_printf(LOG_LEVEL_ERR, "ext2: failed to read GD table\n");
            kfree(sbi->gd);
            kfree(sbi->sb_raw);
            kfree(sbi);
            return NULL;
        }
    }

    /* Create the root inode (inode 2) */
    struct inode *root_inode = ext2_read_inode_sbi(sbi, 2);
    if (!root_inode) {
        log_printf(LOG_LEVEL_ERR, "ext2: failed to read root inode\n");
        kfree(sbi->gd);
        kfree(sbi->sb_raw);
        kfree(sbi);
        return NULL;
    }

    /* Create the super_block */
    struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
    if (!sb) {
        kfree(root_inode->priv);
        kfree(root_inode);
        kfree(sbi->gd);
        kfree(sbi->sb_raw);
        kfree(sbi);
        return NULL;
    }
    memset(sb, 0, sizeof(*sb));
    sb->fs_name = "ext2";
    sb->root    = root_inode;
    sb->sb_data = sbi;

    log_printf(LOG_LEVEL_INFO,
               "ext2: mounted (block_size=%u, inodes=%u, blocks=%u)\n",
               sbi->block_size, sbi->sb_raw->s_inodes_count,
               sbi->sb_raw->s_blocks_count);

    return sb;
}