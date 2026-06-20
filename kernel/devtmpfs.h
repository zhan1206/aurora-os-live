/*
 * devtmpfs.h - devtmpfs device filesystem header
 *
 * Provides a simple /dev virtual filesystem that auto-creates
 * device nodes. Inspired by CoolPotOS's devtmpfs.
 *
 * Supported devices:
 *   /dev/null    - Data sink (write) / EOF (read)
 *   /dev/zero    - Zero bytes source (read)
 *   /dev/console - System console
 *   /dev/tty     - Current terminal
 */

#ifndef DEVTMPFS_H
#define DEVTMPFS_H

struct super_block;

/* Initialize and mount devtmpfs at /dev */
void devtmpfs_init(void);

/* Create the devtmpfs super block */
struct super_block *devtmpfs_create(void);

#endif /* DEVTMPFS_H */