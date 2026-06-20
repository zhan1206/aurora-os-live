/*
 * fsck.h - Filesystem Consistency Checker
 *
 * Validates and repairs ext2 filesystem integrity.
 * Checks performed:
 *   - Superblock: magic number, block size, group counts
 *   - Block group descriptors: valid ranges
 *   - Block bitmap: cross-reference with actual allocation
 *   - Inode bitmap: cross-reference with directory entries
 *   - Directory entries: valid inode numbers, name lengths
 *   - Inode integrity: valid block pointers, size consistency
 *
 * Repair actions:
 *   - Restore superblock from backup if primary is corrupted
 *   - Clear stale block bitmap entries
 *   - Clear stale inode bitmap entries
 *   - Fix directory entry rec_len values
 */
#ifndef FSCK_H
#define FSCK_H

#include <stdint.h>

/* ================================================================
 * Fsck result codes
 * ================================================================ */
#define FSCK_OK         0   /* filesystem is clean */
#define FSCK_WARN       1   /* minor issues found and fixed */
#define FSCK_ERROR      2   /* errors found, some fixed */
#define FSCK_FATAL      3   /* unrecoverable errors */

/* ================================================================
 * Fsck flags
 * ================================================================ */
#define FSCK_FLAG_FIX    0x01  /* automatically fix errors */
#define FSCK_FLAG_FORCE  0x02  /* force check even if clean */
#define FSCK_FLAG_VERBOSE 0x04 /* verbose output */

/* ================================================================
 * Fsck statistics
 * ================================================================ */
struct fsck_stats {
    uint32_t superblock_checked;
    uint32_t superblock_errors;
    uint32_t superblock_fixed;

    uint32_t groups_checked;
    uint32_t groups_errors;
    uint32_t groups_fixed;

    uint32_t inodes_checked;
    uint32_t inodes_errors;
    uint32_t inodes_fixed;

    uint32_t blocks_checked;
    uint32_t blocks_errors;
    uint32_t blocks_fixed;

    uint32_t dirs_checked;
    uint32_t dirs_errors;
    uint32_t dirs_fixed;

    uint32_t orphans_found;
    uint32_t orphans_cleared;
};

/* ================================================================
 * Fsck API
 * ================================================================ */

struct block_device;
struct super_block;

/*
 * fsck_run: Run filesystem consistency check.
 * @bdev:  block device containing the filesystem
 * @flags: FSCK_FLAG_* bitmask
 * @stats: output statistics (can be NULL)
 * Returns FSCK_OK, FSCK_WARN, FSCK_ERROR, or FSCK_FATAL.
 */
int fsck_run(struct block_device *bdev, uint32_t flags,
             struct fsck_stats *stats);

/*
 * fsck_superblock_backup: Read superblock from a backup location.
 * Ext2 stores backup superblocks at the start of block groups 1, 3, 5, 7, ...
 * (groups with powers of 3, 5, 7).
 * @bdev:       block device
 * @group_num:  block group number to try (0 = primary)
 * @sb_out:     output buffer for superblock (must be 1024 bytes)
 * Returns 0 on success, -1 on failure.
 */
int fsck_read_superblock(struct block_device *bdev, uint32_t group_num,
                         void *sb_out);

/*
 * fsck_restore_superblock: Restore primary superblock from backup.
 * @bdev: block device
 * Returns 0 on success, -1 on failure.
 */
int fsck_restore_superblock(struct block_device *bdev);

/*
 * fsck_quick_check: Fast check — only verify superblock magic.
 * Returns 0 if superblock is valid, -1 if corrupted.
 */
int fsck_quick_check(struct block_device *bdev);

#endif /* FSCK_H */