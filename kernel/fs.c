/*
 * fs.c - File system initialization (VFS + RamFS + embedded files)
 *
 * Separated from sched.c to respect single responsibility:
 *   - sched.c: process/task management only
 *   - fs.c:    file system initialization and mounting
 *
 * Mount sequence:
 *   1. Initialize VFS
 *   2. Try ext2 from ramdisk:
 *      a. Quick-check superblock integrity
 *      b. Mount ext2
 *      c. Initialize journal (WAL) for crash recovery
 *      d. If journal is dirty, replay transactions
 *      e. Run fsck to verify consistency
 *      f. Mount procfs + devtmpfs
 *      g. Launch embedded init
 *   3. Fall back to ramfs if ext2 fails
 */
#include "include/log.h"
#include "vfs.h"
#include "fs.h"
#include "pagetable.h"
#include "ext2.h"
#include "block_dev.h"
#include "procfs.h"
#include "devtmpfs.h"
#include "journal.h"
#include "fsck.h"

/* ================================================================
 * Journal configuration
 * ================================================================ */
#define JOURNAL_BLOCKS  256   /* 256 ext2 blocks = 256 KB journal area */

/* ================================================================
 * fs_init — Initialize VFS, RamFS, embedded files, and launch init
 * ================================================================ */
void fs_init(void) {
    vfs_init();

    /* Phase 1: Try to mount ext2 from ramdisk if available */
    struct block_device *ramdisk = block_dev_find("ramdisk0");
    if (ramdisk) {
        /* Quick superblock check before attempting mount */
        if (fsck_quick_check(ramdisk) < 0) {
            log_printf(LOG_LEVEL_WARN, "fs: ext2 superblock check failed, attempting recovery\n");
            fsck_restore_superblock(ramdisk);
        }

        struct super_block *ext2_sb = ext2_mount(ramdisk);
        if (ext2_sb) {
            vfs_mount_root(ext2_sb);
            log_printf(LOG_LEVEL_INFO, "fs: ext2 mounted from ramdisk0\n");

            /*
             * Initialize journal for crash recovery.
             * The journal is placed after the ext2 filesystem area.
             * total_device_blocks = total_sectors / sectors_per_ext2_block
             * journal_start = ext2_s_blocks_count
             * journal_size = remaining blocks (or JOURNAL_BLOCKS, whichever is smaller)
             */
            uint32_t block_size = 1024; /* default ext2 block size */
            if (ext2_sb->sb_data) {
                /* ext2_sb_info->block_size is not directly accessible;
                 * we use the default 1024 for ramdisk ext2 images */
            }
            uint32_t spb = block_size / ramdisk->block_size;
            uint64_t total_device_blocks = ramdisk->total_sectors / spb;

            /* Read s_blocks_count from the superblock */
            uint8_t raw_sb[1024];
            uint64_t ext2_blocks = 0;
            if (block_dev_read(ramdisk, raw_sb, 2, 2) == 0) {
                struct ext2_superblock *sb = (struct ext2_superblock *)raw_sb;
                if (sb->s_magic == EXT2_SUPER_MAGIC) {
                    ext2_blocks = sb->s_blocks_count;
                }
            }

            if (ext2_blocks > 0 && total_device_blocks > ext2_blocks + 8) {
                uint64_t journal_blocks = total_device_blocks - ext2_blocks;
                if (journal_blocks > JOURNAL_BLOCKS) journal_blocks = JOURNAL_BLOCKS;

                log_printf(LOG_LEVEL_INFO, "fs: initializing journal at block %llu (%llu blocks)\n",
                           (unsigned long long)ext2_blocks,
                           (unsigned long long)journal_blocks);

                if (journal_init(ramdisk, ext2_blocks, journal_blocks, block_size, ext2_blocks) == 0) {
                    if (!journal_is_clean()) {
                        log_printf(LOG_LEVEL_WARN, "fs: journal was dirty — recovery performed\n");
                    } else {
                        log_printf(LOG_LEVEL_INFO, "fs: journal initialized (clean)\n");
                    }

                    /* Run fsck to verify filesystem consistency */
                    struct fsck_stats fsck_st;
                    int fsck_result = fsck_run(ramdisk, FSCK_FLAG_FIX, &fsck_st);
                    if (fsck_result == FSCK_OK) {
                        log_printf(LOG_LEVEL_INFO, "fs: fsck passed — filesystem is clean\n");
                    } else if (fsck_result == FSCK_WARN) {
                        log_printf(LOG_LEVEL_WARN, "fs: fsck fixed %u errors\n",
                                   fsck_st.superblock_fixed + fsck_st.groups_fixed +
                                   fsck_st.blocks_fixed + fsck_st.inodes_fixed +
                                   fsck_st.dirs_fixed);
                    } else {
                        log_printf(LOG_LEVEL_ERR, "fs: fsck found %u unfixed errors\n",
                                   fsck_st.superblock_errors + fsck_st.groups_errors +
                                   fsck_st.blocks_errors + fsck_st.inodes_errors +
                                   fsck_st.dirs_errors);
                    }
                } else {
                    log_printf(LOG_LEVEL_WARN, "fs: journal initialization failed — continuing without journal\n");
                }
            } else {
                log_printf(LOG_LEVEL_WARN, "fs: not enough space for journal (ext2=%llu, total=%llu)\n",
                           (unsigned long long)ext2_blocks,
                           (unsigned long long)total_device_blocks);
            }

            /* Mount procfs at /proc */
            procfs_init();

            /* Mount devtmpfs at /dev */
            devtmpfs_init();

            embed_init();

            if (vfs_lookup("/hello")) {
                log_printf(LOG_LEVEL_INFO, "fs: Found /hello in ext2, attempting exec\n");
                int pid = exec_elf("/hello");
                log_printf(LOG_LEVEL_INFO, "fs: exec_elf returned pid=%d\n", pid);
            }
            return;
        }
        log_printf(LOG_LEVEL_WARN, "fs: ext2 mount from ramdisk0 failed, falling back to ramfs\n");
    }

    /* Phase 2: Fall back to ramfs (in-memory) */
    struct super_block *ram = ramfs_create();
    if (ram) {
        ramfs_add_file("hello.txt", "This is a ramfs file.\n");
        vfs_mount_root(ram);
        log_printf(LOG_LEVEL_INFO, "fs: RamFS mounted\n");

        /* Mount procfs at /proc */
        procfs_init();

        /* Mount devtmpfs at /dev */
        devtmpfs_init();

        embed_init();

        if (vfs_lookup("/hello")) {
            log_printf(LOG_LEVEL_INFO, "fs: Found /hello in ramfs, attempting exec\n");
            int pid = exec_elf("/hello");
            log_printf(LOG_LEVEL_INFO, "fs: exec_elf returned pid=%d\n", pid);
        }
    }
}