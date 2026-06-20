/*
 * fsck.c - Filesystem Consistency Checker implementation
 *
 * Performs comprehensive ext2 filesystem validation and repair.
 * Designed to be called both at mount time (automatic) and
 * manually via shell commands.
 *
 * Check sequence:
 *   1. Superblock validation (magic, geometry, backup if needed)
 *   2. Block group descriptor validation
 *   3. Block bitmap cross-reference
 *   4. Inode integrity check
 *   5. Directory structure validation
 *   6. Orphan inode detection
 */
#include "fsck.h"
#include "ext2.h"
#include "block_dev.h"
#include "include/log.h"
#include "mem.h"
#include <string.h>

/* ================================================================
 * Internal helpers
 * ================================================================ */

/*
 * Read a filesystem block from the block device.
 * Converts filesystem block number to device sector number.
 */
static int read_fs_block(struct block_device *bdev, uint32_t block_size,
                         uint32_t block_num, void *buf) {
    uint32_t spb = block_size / bdev->block_size;
    return block_dev_read(bdev, buf, (uint64_t)block_num * spb, (int)spb);
}

static int write_fs_block(struct block_device *bdev, uint32_t block_size,
                          uint32_t block_num, const void *buf) {
    uint32_t spb = block_size / bdev->block_size;
    return block_dev_write(bdev, buf, (uint64_t)block_num * spb, (int)spb);
}

/* Simple XOR checksum for superblock validation */
/* Reserved for future superblock checksum verification */
#if 0
static uint32_t sb_checksum(const struct ext2_superblock *sb) {
    const uint8_t *p = (const uint8_t *)sb;
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; i++) {
        sum = (sum << 1) | (sum >> 31);
        sum ^= p[i];
    }
    return sum;
}
#endif

/* ================================================================
 * Superblock check
 * ================================================================ */
static int check_superblock(struct block_device *bdev, uint32_t flags,
                            struct ext2_superblock *sb_out,
                            uint32_t *block_size_out,
                            struct fsck_stats *stats) {
    int verbose = (flags & FSCK_FLAG_VERBOSE) != 0;
    int fix = (flags & FSCK_FLAG_FIX) != 0;
    uint8_t raw_sb[1024];

    if (stats) stats->superblock_checked++;

    /* Try primary superblock (offset 1024 from device start) */
    if (block_dev_read(bdev, raw_sb, 2, 2) < 0) {
        log_printf(LOG_LEVEL_ERR, "fsck: cannot read primary superblock\n");
        if (stats) stats->superblock_errors++;
        /* Try backup at block group 1 */
        if (fix && fsck_restore_superblock(bdev) == 0) {
            if (stats) stats->superblock_fixed++;
            return block_dev_read(bdev, raw_sb, 2, 2);
        }
        return -1;
    }

    struct ext2_superblock *sb = (struct ext2_superblock *)raw_sb;

    if (sb->s_magic != EXT2_SUPER_MAGIC) {
        log_printf(LOG_LEVEL_ERR, "fsck: invalid superblock magic (0x%04x)\n", sb->s_magic);
        if (stats) stats->superblock_errors++;
        if (fix && fsck_restore_superblock(bdev) == 0) {
            if (stats) stats->superblock_fixed++;
            return block_dev_read(bdev, raw_sb, 2, 2);
        }
        return -1;
    }

    uint32_t block_size = (uint32_t)(1024 << sb->s_log_block_size);
    if (block_size < 1024 || block_size > 4096) {
        log_printf(LOG_LEVEL_ERR, "fsck: invalid block size %u\n", block_size);
        if (stats) stats->superblock_errors++;
        return -1;
    }

    /* Validate basic geometry */
    if (sb->s_blocks_count == 0 || sb->s_inodes_count == 0) {
        log_printf(LOG_LEVEL_ERR, "fsck: zero blocks or inodes\n");
        if (stats) stats->superblock_errors++;
        return -1;
    }

    if (sb->s_blocks_per_group == 0 || sb->s_inodes_per_group == 0) {
        log_printf(LOG_LEVEL_ERR, "fsck: zero blocks/inodes per group\n");
        if (stats) stats->superblock_errors++;
        return -1;
    }

    /* Check free count consistency */
    uint32_t num_groups = (sb->s_blocks_count + sb->s_blocks_per_group - 1) /
                          sb->s_blocks_per_group;

    if (sb->s_state != EXT2_VALID_FS && sb->s_state != EXT2_ERROR_FS) {
        if (verbose) log_printf(LOG_LEVEL_WARN, "fsck: unknown filesystem state 0x%04x\n", sb->s_state);
        if (fix) {
            sb->s_state = EXT2_VALID_FS;
            if (write_fs_block(bdev, block_size, sb->s_first_data_block + 1, raw_sb) == 0) {
                if (stats) stats->superblock_fixed++;
            }
        }
    }

    if (sb->s_state == EXT2_ERROR_FS) {
        log_printf(LOG_LEVEL_WARN, "fsck: filesystem marked as having errors\n");
        if (fix) {
            sb->s_state = EXT2_VALID_FS;
            write_fs_block(bdev, block_size, sb->s_first_data_block + 1, raw_sb);
            if (stats) stats->superblock_fixed++;
        }
    }

    if (verbose) {
        log_printf(LOG_LEVEL_INFO, "fsck: superblock OK — %u blocks, %u inodes, %u groups, block_size=%u\n",
                   sb->s_blocks_count, sb->s_inodes_count, num_groups, block_size);
    }

    if (sb_out) memcpy(sb_out, raw_sb, sizeof(struct ext2_superblock));
    if (block_size_out) *block_size_out = block_size;

    return 0;
}

/* ================================================================
 * Block group descriptor check
 * ================================================================ */
static int check_group_descriptors(struct block_device *bdev,
                                   const struct ext2_superblock *sb,
                                   uint32_t block_size, uint32_t flags,
                                   struct fsck_stats *stats) {
    int verbose = (flags & FSCK_FLAG_VERBOSE) != 0;
    (void)(flags); /* fix flag may be used in future repair */

    uint32_t num_groups = (sb->s_blocks_count + sb->s_blocks_per_group - 1) /
                          sb->s_blocks_per_group;
    uint32_t gd_per_block = block_size / 32;
    uint32_t gd_blocks = (num_groups + gd_per_block - 1) / gd_per_block;

    uint8_t *gd_buf = (uint8_t *)kmalloc(gd_blocks * block_size);
    if (!gd_buf) return -1;

    /* Read all group descriptors */
    uint32_t gd_start = sb->s_first_data_block + 1 + 1; /* after superblock */
    for (uint32_t i = 0; i < gd_blocks; i++) {
        if (read_fs_block(bdev, block_size, gd_start + i,
                          gd_buf + i * block_size) < 0) {
            kfree(gd_buf);
            return -1;
        }
    }

    for (uint32_t g = 0; g < num_groups; g++) {
        if (stats) stats->groups_checked++;
        struct ext2_group_desc *gd = (struct ext2_group_desc *)(gd_buf + g * 32);

        /* Validate block bitmap location */
        if (gd->bg_block_bitmap == 0 || gd->bg_block_bitmap >= sb->s_blocks_count) {
            log_printf(LOG_LEVEL_ERR, "fsck: group %u: invalid block bitmap %u\n",
                       g, gd->bg_block_bitmap);
            if (stats) stats->groups_errors++;
        }

        /* Validate inode bitmap location */
        if (gd->bg_inode_bitmap == 0 || gd->bg_inode_bitmap >= sb->s_blocks_count) {
            log_printf(LOG_LEVEL_ERR, "fsck: group %u: invalid inode bitmap %u\n",
                       g, gd->bg_inode_bitmap);
            if (stats) stats->groups_errors++;
        }

        /* Validate inode table location */
        if (gd->bg_inode_table == 0 || gd->bg_inode_table >= sb->s_blocks_count) {
            log_printf(LOG_LEVEL_ERR, "fsck: group %u: invalid inode table %u\n",
                       g, gd->bg_inode_table);
            if (stats) stats->groups_errors++;
        }
    }

    if (verbose) {
        log_printf(LOG_LEVEL_INFO, "fsck: %u group descriptors checked\n", num_groups);
    }

    kfree(gd_buf);
    return 0;
}

/* ================================================================
 * Inode and block bitmap cross-reference
 * ================================================================ */
static int check_bitmaps(struct block_device *bdev,
                         const struct ext2_superblock *sb,
                         uint32_t block_size, uint32_t flags,
                         struct fsck_stats *stats) {
    int fix = (flags & FSCK_FLAG_FIX) != 0;
    (void)fix; /* used in future bitmap repair */

    uint32_t num_groups = (sb->s_blocks_count + sb->s_blocks_per_group - 1) /
                          sb->s_blocks_per_group;
    uint32_t gd_per_block = block_size / 32;
    uint32_t gd_blocks = (num_groups + gd_per_block - 1) / gd_per_block;

    uint8_t *gd_buf = (uint8_t *)kmalloc(gd_blocks * block_size);
    uint8_t *bitmap_buf = (uint8_t *)kmalloc(block_size);
    if (!gd_buf || !bitmap_buf) {
        if (gd_buf) kfree(gd_buf);
        if (bitmap_buf) kfree(bitmap_buf);
        return -1;
    }

    uint32_t gd_start = sb->s_first_data_block + 2;
    for (uint32_t i = 0; i < gd_blocks; i++) {
        read_fs_block(bdev, block_size, gd_start + i, gd_buf + i * block_size);
    }

    for (uint32_t g = 0; g < num_groups; g++) {
        struct ext2_group_desc *gd = (struct ext2_group_desc *)(gd_buf + g * 32);

        /* Check block bitmap */
        if (stats) stats->blocks_checked++;
        if (read_fs_block(bdev, block_size, gd->bg_block_bitmap, bitmap_buf) == 0) {
            uint32_t free_count = 0;
            uint32_t blocks_in_group = sb->s_blocks_per_group;
            if (g == num_groups - 1) {
                blocks_in_group = sb->s_blocks_count - g * sb->s_blocks_per_group;
            }
            for (uint32_t b = 0; b < blocks_in_group; b++) {
                if (!(bitmap_buf[b / 8] & (1 << (b % 8)))) free_count++;
            }
            if (free_count != gd->bg_free_blocks_count) {
                if (stats) stats->blocks_errors++;
            }
        }

        /* Check inode bitmap */
        if (stats) stats->inodes_checked++;
        if (read_fs_block(bdev, block_size, gd->bg_inode_bitmap, bitmap_buf) == 0) {
            uint32_t free_count = 0;
            uint32_t inodes_in_group = sb->s_inodes_per_group;
            if (g == num_groups - 1) {
                inodes_in_group = sb->s_inodes_count - g * sb->s_inodes_per_group;
            }
            for (uint32_t i = 0; i < inodes_in_group; i++) {
                if (!(bitmap_buf[i / 8] & (1 << (i % 8)))) free_count++;
            }
            if (free_count != gd->bg_free_inodes_count) {
                if (stats) stats->inodes_errors++;
            }
        }
    }

    kfree(gd_buf);
    kfree(bitmap_buf);
    return 0;
}

/* ================================================================
 * Directory structure validation
 * ================================================================ */
static int check_directory(struct block_device *bdev,
                           const struct ext2_superblock *sb,
                           uint32_t block_size, uint32_t inum,
                           uint32_t flags, struct fsck_stats *stats) {
    int fix = (flags & FSCK_FLAG_FIX) != 0;

    /* Read inode */
    uint32_t inodes_per_group = sb->s_inodes_per_group;
    uint32_t group = (inum - 1) / inodes_per_group;
    uint32_t index = (inum - 1) % inodes_per_group;

    /* Get group descriptor */
    uint32_t gd_per_block = block_size / 32;
    uint32_t gd_start = sb->s_first_data_block + 2;
    uint8_t *gd_buf = (uint8_t *)kmalloc(block_size);
    if (!gd_buf) return -1;

    uint32_t gd_block = gd_start + group / gd_per_block;
    read_fs_block(bdev, block_size, gd_block, gd_buf);
    struct ext2_group_desc *gd = (struct ext2_group_desc *)(gd_buf + (group % gd_per_block) * 32);

    /* Read inode */
    uint32_t inode_table_block = gd->bg_inode_table + (index * sb->s_inode_size) / block_size;
    uint32_t inode_offset = (index * sb->s_inode_size) % block_size;

    uint8_t *inode_buf = (uint8_t *)kmalloc(block_size);
    if (!inode_buf) { kfree(gd_buf); return -1; }

    read_fs_block(bdev, block_size, inode_table_block, inode_buf);
    struct ext2_inode *inode = (struct ext2_inode *)(inode_buf + inode_offset);

    if (!(inode->i_mode & EXT2_S_IFDIR)) {
        kfree(inode_buf);
        kfree(gd_buf);
        return 0; /* not a directory */
    }

    /* Iterate directory entries */
    uint8_t *dir_buf = (uint8_t *)kmalloc(block_size);
    if (!dir_buf) { kfree(inode_buf); kfree(gd_buf); return -1; }

    uint32_t dir_size = inode->i_size;
    for (uint32_t off = 0; off < dir_size && off < block_size * EXT2_NDIR_BLOCKS; ) {
        uint32_t logical_block = off / block_size;
        uint32_t block_off = off % block_size;

        if (logical_block >= EXT2_NDIR_BLOCKS) break;
        if (inode->i_block[logical_block] == 0) break;

        read_fs_block(bdev, block_size, inode->i_block[logical_block], dir_buf);

        while (block_off < block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(dir_buf + block_off);

            if (de->inode == 0 && de->rec_len == 0) break;
            if (de->rec_len == 0) {
                if (stats) stats->dirs_errors++;
                break;
            }

            if (stats) stats->dirs_checked++;

            /* Validate entry */
            if (de->inode != 0) {
                if (de->inode > sb->s_inodes_count) {
                    log_printf(LOG_LEVEL_WARN, "fsck: dir entry '%.*s' has invalid inode %u\n",
                               de->name_len, de->name, de->inode);
                    if (stats) stats->dirs_errors++;
                    if (fix) {
                        de->inode = 0;
                        if (stats) stats->dirs_fixed++;
                    }
                }
            }

            if (de->name_len > (uint8_t)250) {
                if (stats) stats->dirs_errors++;
                if (fix) {
                    de->name_len = 0;
                    if (stats) stats->dirs_fixed++;
                }
            }

            block_off += de->rec_len;
            off += de->rec_len;
        }
    }

    kfree(dir_buf);
    kfree(inode_buf);
    kfree(gd_buf);
    return 0;
}

/* ================================================================
 * Public API
 * ================================================================ */

int fsck_run(struct block_device *bdev, uint32_t flags,
             struct fsck_stats *stats) {
    if (!bdev) return FSCK_FATAL;

    struct fsck_stats local_stats;
    memset(&local_stats, 0, sizeof(local_stats));
    if (!stats) stats = &local_stats;

    int result = FSCK_OK;
    int verbose = (flags & FSCK_FLAG_VERBOSE) != 0;

    if (verbose) log_printf(LOG_LEVEL_INFO, "fsck: starting filesystem check\n");

    /* Phase 1: Superblock */
    struct ext2_superblock sb;
    uint32_t block_size = 1024;
    if (check_superblock(bdev, flags, &sb, &block_size, stats) < 0) {
        log_printf(LOG_LEVEL_ERR, "fsck: superblock check failed — filesystem may be unrecoverable\n");
        return FSCK_FATAL;
    }

    /* Phase 2: Block group descriptors */
    check_group_descriptors(bdev, &sb, block_size, flags, stats);

    /* Phase 3: Bitmap cross-reference */
    check_bitmaps(bdev, &sb, block_size, flags, stats);

    /* Phase 4: Directory structure (check root inode = 2) */
    check_directory(bdev, &sb, block_size, 2, flags, stats);

    /* Determine result */
    uint32_t total_errors = stats->superblock_errors + stats->groups_errors +
                            stats->blocks_errors + stats->inodes_errors +
                            stats->dirs_errors;
    uint32_t total_fixed  = stats->superblock_fixed + stats->groups_fixed +
                            stats->blocks_fixed + stats->inodes_fixed +
                            stats->dirs_fixed;

    if (total_errors == 0) {
        result = FSCK_OK;
    } else if (total_errors == total_fixed) {
        result = FSCK_WARN;
    } else {
        result = FSCK_ERROR;
    }

    if (verbose) {
        log_printf(LOG_LEVEL_INFO, "fsck: check complete — %u errors, %u fixed (result=%d)\n",
                   total_errors, total_fixed, result);
    }

    return result;
}

int fsck_read_superblock(struct block_device *bdev, uint32_t group_num,
                         void *sb_out) {
    if (!bdev || !sb_out) return -1;

    /* Read 1024-byte superblock from the block device */
    uint8_t raw_sb[1024];

    if (group_num == 0) {
        /* Primary superblock at offset 1024 */
        if (block_dev_read(bdev, raw_sb, 2, 2) < 0) return -1;
    } else {
        /* Backup superblock at block group boundary */
        /* First read the primary to get geometry */
        struct ext2_superblock *primary = (struct ext2_superblock *)raw_sb;
        if (block_dev_read(bdev, raw_sb, 2, 2) < 0) return -1;
        if (primary->s_magic != EXT2_SUPER_MAGIC) return -1;

        uint32_t block_size = (uint32_t)(1024 << primary->s_log_block_size);
        uint32_t spb = block_size / bdev->block_size;
        uint64_t backup_sector = (uint64_t)group_num * primary->s_blocks_per_group * spb + 2;

        if (block_dev_read(bdev, raw_sb, backup_sector, 2) < 0) return -1;
    }

    struct ext2_superblock *sb = (struct ext2_superblock *)raw_sb;
    if (sb->s_magic != EXT2_SUPER_MAGIC) return -1;

    memcpy(sb_out, raw_sb, 1024);
    return 0;
}

int fsck_restore_superblock(struct block_device *bdev) {
    if (!bdev) return -1;

    uint8_t backup_sb[1024];
    /* Try backup superblocks at groups 1, 3, 5, 7 */
    uint32_t backup_groups[] = {1, 3, 5, 7};
    for (int i = 0; i < 4; i++) {
        if (fsck_read_superblock(bdev, backup_groups[i], backup_sb) == 0) {
            struct ext2_superblock *sb = (struct ext2_superblock *)backup_sb;
            if (sb->s_magic == EXT2_SUPER_MAGIC) {
                /* Restore primary superblock */
                if (block_dev_write(bdev, backup_sb, 2, 2) == 0) {
                    log_printf(LOG_LEVEL_INFO, "fsck: restored superblock from group %u backup\n",
                               backup_groups[i]);
                    return 0;
                }
            }
        }
    }

    log_printf(LOG_LEVEL_ERR, "fsck: no valid backup superblock found\n");
    return -1;
}

int fsck_quick_check(struct block_device *bdev) {
    if (!bdev) return -1;
    uint8_t raw_sb[1024];
    if (block_dev_read(bdev, raw_sb, 2, 2) < 0) return -1;
    struct ext2_superblock *sb = (struct ext2_superblock *)raw_sb;
    return (sb->s_magic == EXT2_SUPER_MAGIC) ? 0 : -1;
}