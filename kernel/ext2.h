/*
 * ext2.h - ext2 filesystem on-disk structures
 *
 * Based on the Second Extended File System (ext2) specification.
 * All multi-byte fields are little-endian.
 */
#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>

/* ================================================================
 * Block size constants
 * ================================================================ */
#define EXT2_MIN_BLOCK_SIZE  1024
#define EXT2_MAX_BLOCK_SIZE  4096

/* ================================================================
 * Filesystem identification
 * ================================================================ */
#define EXT2_SUPER_MAGIC      0xEF53

/* ================================================================
 * Inode type flags (i_mode)
 * ================================================================ */
#define EXT2_S_IFSOCK  0xC000
#define EXT2_S_IFLNK   0xA000
#define EXT2_S_IFREG   0x8000
#define EXT2_S_IFBLK   0x6000
#define EXT2_S_IFDIR   0x4000
#define EXT2_S_IFCHR   0x2000
#define EXT2_S_IFIFO   0x1000

/* Permission bits mask */
#define EXT2_S_IRWXU   0x01C0
#define EXT2_S_IRUSR   0x0100
#define EXT2_S_IWUSR   0x0080
#define EXT2_S_IXUSR   0x0040
#define EXT2_S_IRWXG   0x0038
#define EXT2_S_IRGRP   0x0020
#define EXT2_S_IWGRP   0x0010
#define EXT2_S_IXGRP   0x0008
#define EXT2_S_IRWXO   0x0007
#define EXT2_S_IROTH   0x0004
#define EXT2_S_IWOTH   0x0002
#define EXT2_S_IXOTH   0x0001

/* ================================================================
 * Superblock state flags
 * ================================================================ */
#define EXT2_VALID_FS   1
#define EXT2_ERROR_FS   2

/* ================================================================
 * Superblock: 1024 bytes starting at offset 1024
 * ================================================================ */
struct ext2_superblock {
    uint32_t s_inodes_count;       /* Total number of inodes */
    uint32_t s_blocks_count;       /* Total number of blocks */
    uint32_t s_r_blocks_count;     /* Reserved blocks */
    uint32_t s_free_blocks_count;  /* Free blocks counter */
    uint32_t s_free_inodes_count;  /* Free inodes counter */
    uint32_t s_first_data_block;   /* First data block (0 for >= 1024-byte blocks) */
    uint32_t s_log_block_size;     /* Block size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;      /* Fragment size (unused in ext2) */
    uint32_t s_blocks_per_group;   /* Blocks per group */
    uint32_t s_frags_per_group;    /* Fragments per group */
    uint32_t s_inodes_per_group;   /* Inodes per group */
    uint32_t s_mtime;              /* Mount time */
    uint32_t s_wtime;              /* Write time */
    uint16_t s_mnt_count;          /* Mount count */
    uint16_t s_max_mnt_count;      /* Max mount count before fsck */
    uint16_t s_magic;              /* EXT2_SUPER_MAGIC (0xEF53) */
    uint16_t s_state;              /* File system state */
    uint16_t s_errors;             /* Error handling policy */
    uint16_t s_minor_rev_level;    /* Minor revision level */
    uint32_t s_lastcheck;          /* Last check time */
    uint32_t s_checkinterval;      /* Max time between checks */
    uint32_t s_creator_os;         /* Creator OS */
    uint32_t s_rev_level;          /* Revision level (0 = original, 1 = dynamic) */
    uint16_t s_def_resuid;         /* Default UID for reserved blocks */
    uint16_t s_def_resgid;         /* Default GID for reserved blocks */

    /* Extended superblock fields (rev_level >= 1) */
    uint32_t s_first_ino;          /* First non-reserved inode */
    uint16_t s_inode_size;         /* Size of inode structure */
    uint16_t s_block_group_nr;     /* Block group # of this superblock */
    uint32_t s_feature_compat;     /* Compatible feature set */
    uint32_t s_feature_incompat;   /* Incompatible feature set */
    uint32_t s_feature_ro_compat;  /* Read-only compatible feature set */
    uint8_t  s_uuid[16];           /* 128-bit UUID */
    char     s_volume_name[16];    /* Volume name */
    char     s_last_mounted[64];   /* Directory where last mounted */
    uint32_t s_algo_bitmap;        /* Compression algorithm usage bitmap */

    /* Performance hints */
    uint8_t  s_prealloc_blocks;    /* Blocks to preallocate for files */
    uint8_t  s_prealloc_dir_blocks; /* Blocks to preallocate for directories */
    uint16_t s_padding1;

    /* Journaling support (ext3) */
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;

    /* Directory indexing */
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_padding2[3];

    /* Other options */
    uint32_t s_default_mount_options;
    uint32_t s_first_meta_bg;
    uint32_t s_reserved[190];       /* Pad to 1024 bytes */
} __attribute__((packed));

/* ================================================================
 * Block Group Descriptor: 32 bytes
 * ================================================================ */
struct ext2_group_desc {
    uint32_t bg_block_bitmap;      /* Block bitmap block */
    uint32_t bg_inode_bitmap;      /* Inode bitmap block */
    uint32_t bg_inode_table;       /* Inode table start block */
    uint16_t bg_free_blocks_count; /* Free blocks count */
    uint16_t bg_free_inodes_count; /* Free inodes count */
    uint16_t bg_used_dirs_count;   /* Directories count */
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

/* ================================================================
 * Inode: 128 bytes
 * ================================================================ */
#define EXT2_NDIR_BLOCKS  12
#define EXT2_IND_BLOCK    12
#define EXT2_DIND_BLOCK   13
#define EXT2_TIND_BLOCK   14
#define EXT2_N_BLOCKS     15

struct ext2_inode {
    uint16_t i_mode;           /* File type and permissions */
    uint16_t i_uid;            /* Owner UID (low 16 bits) */
    uint32_t i_size;           /* Size in bytes (lower 32 bits) */
    uint32_t i_atime;          /* Access time */
    uint32_t i_ctime;          /* Creation time */
    uint32_t i_mtime;          /* Modification time */
    uint32_t i_dtime;          /* Deletion time */
    uint16_t i_gid;            /* Group ID (low 16 bits) */
    uint16_t i_links_count;    /* Hard link count */
    uint32_t i_blocks;         /* Number of 512-byte sectors */
    uint32_t i_flags;          /* File flags */
    uint32_t i_osd1;           /* OS dependent */
    uint32_t i_block[EXT2_N_BLOCKS]; /* Block pointers */
    uint32_t i_generation;     /* File version (NFS) */
    uint32_t i_file_acl;       /* File ACL */
    uint32_t i_dir_acl;        /* Directory ACL (or high 32 bits of size) */
    uint32_t i_faddr;          /* Fragment address */
    uint8_t  i_osd2[12];       /* OS dependent 2 */
} __attribute__((packed));

/* ================================================================
 * Directory entry
 * ================================================================ */
#define EXT2_FT_UNKNOWN   0
#define EXT2_FT_REG_FILE  1
#define EXT2_FT_DIR       2
#define EXT2_FT_CHRDEV    3
#define EXT2_FT_BLKDEV    4
#define EXT2_FT_FIFO      5
#define EXT2_FT_SOCK      6
#define EXT2_FT_SYMLINK   7

struct ext2_dir_entry {
    uint32_t inode;          /* Inode number */
    uint16_t rec_len;        /* Total entry size (must be multiple of 4) */
    uint8_t  name_len;       /* Name length (up to 255) */
    uint8_t  file_type;      /* EXT2_FT_* */
    char     name[];         /* File name (no null terminator) */
} __attribute__((packed));

/* ================================================================
 * ext2 filesystem creation API
 * ================================================================ */
struct block_device;
struct super_block;

/*
 * ext2_mount: Mount an ext2 filesystem from a block device.
 * @bdev: block device containing the ext2 image.
 * Returns a super_block on success, or NULL on failure.
 */
struct super_block *ext2_mount(struct block_device *bdev);

#endif /* EXT2_H */