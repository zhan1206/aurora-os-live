/*
 * fs.c - File system initialization (VFS + RamFS + embedded files)
 *
 * Separated from sched.c to respect single responsibility:
 *   - sched.c: process/task management only
 *   - fs.c:    file system initialization and mounting
 */

#include "include/log.h"
#include "vfs.h"
#include "fs.h"
#include "pagetable.h"
#include "ext2.h"
#include "block_dev.h"
#include "procfs.h"
#include "devtmpfs.h"

/* ================================================================
 * fs_init — Initialize VFS, RamFS, embedded files, and launch init
 * ================================================================ */
void fs_init(void) {
    vfs_init();

    /* Phase 1: Try to mount ext2 from ramdisk if available */
    struct block_device *ramdisk = block_dev_find("ramdisk0");
    if (ramdisk) {
        struct super_block *ext2_sb = ext2_mount(ramdisk);
        if (ext2_sb) {
            vfs_mount_root(ext2_sb);
            log_printf(LOG_LEVEL_INFO, "fs: ext2 mounted from ramdisk0\n");

            /* Mount procfs at /proc */
            procfs_init();

            /* Mount devtmpfs at /dev (CoolPotOS-inspired) */
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

        /* Mount devtmpfs at /dev (CoolPotOS-inspired) */
        devtmpfs_init();

        embed_init();

        if (vfs_lookup("/hello")) {
            log_printf(LOG_LEVEL_INFO, "fs: Found /hello in ramfs, attempting exec\n");
            int pid = exec_elf("/hello");
            log_printf(LOG_LEVEL_INFO, "fs: exec_elf returned pid=%d\n", pid);
        }
    }
}