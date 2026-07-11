/*
 * squashfs.h - Squashfs filesystem on-disk structures
 *
 * Supports Squashfs 4.0 format (little-endian).
 * Reference: https://dr-emann.github.io/squashfs/squashfs.html
 */
#ifndef SQUASHFS_H
#define SQUASHFS_H

#include <stdint.h>

/* ================================================================
 * Magic number
 * ================================================================ */
#define SQUASHFS_MAGIC  0x73717368  /* "hsqs" in little-endian */

/* ================================================================
 * Compression types
 * ================================================================ */
#define SQUASHFS_COMP_ZLIB   1
#define SQUASHFS_COMP_LZ4    2
#define SQUASHFS_COMP_LZO    3
#define SQUASHFS_COMP_XZ     4
#define SQUASHFS_COMP_ZSTD   5

/* ================================================================
 * Inode types
 * ================================================================ */
#define SQUASHFS_DIR_TYPE       1
#define SQUASHFS_REG_TYPE       2
#define SQUASHFS_SYMLINK_TYPE   3
#define SQUASHFS_BLKDEV_TYPE    4
#define SQUASHFS_CHRDEV_TYPE    5
#define SQUASHFS_FIFO_TYPE      6
#define SQUASHFS_SOCKET_TYPE    7
#define SQUASHFS_LDIR_TYPE      8
#define SQUASHFS_LREG_TYPE      9
#define SQUASHFS_LSYMLINK_TYPE  10
#define SQUASHFS_LBLKDEV_TYPE   11
#define SQUASHFS_LCHRDEV_TYPE   12
#define SQUASHFS_LFIFO_TYPE     13
#define SQUASHFS_LSOCKET_TYPE   14

/* ================================================================
 * Superblock (96 bytes, at start of filesystem)
 * ================================================================ */
struct squashfs_superblock {
    uint32_t s_magic;               /* SQUASHFS_MAGIC */
    uint32_t inode_count;           /* Total number of inodes */
    uint32_t mtime;                 /* Last modification time */
    uint32_t block_size;            /* Block size in bytes */
    uint32_t frag_count;            /* Number of fragments */
    uint16_t compression;           /* Compression type */
    uint16_t block_log;             /* log2(block_size) */
    uint16_t flags;                 /* Flags */
    uint16_t no_ids;                /* Number of uid/gid lookup table entries */
    uint16_t major;                 /* Major version */
    uint16_t minor;                 /* Minor version */
    uint64_t root_inode;            /* Root inode reference */
    uint64_t bytes_used;            /* Total bytes used */
    uint64_t id_table_start;        /* Byte offset of id table */
    uint64_t xattr_id_table_start;  /* Byte offset of xattr id table */
    uint64_t inode_table_start;     /* Byte offset of inode table */
    uint64_t directory_table_start; /* Byte offset of directory table */
    uint64_t fragment_table_start;  /* Byte offset of fragment table */
    uint64_t export_table_start;    /* Byte offset of export table */
} __attribute__((packed));

/* ================================================================
 * Base inode header (variable-length, fields depend on type)
 * ================================================================ */
struct squashfs_base_inode {
    uint16_t inode_type;    /* SQUASHFS_*_TYPE */
    uint16_t mode;          /* File mode / permissions */
    uint16_t uid;           /* User ID index into id table */
    uint16_t gid;           /* Group ID index into id table */
    uint32_t mtime;         /* Modification time */
    uint32_t inode_number;  /* Inode number */
} __attribute__((packed));

/* ================================================================
 * Regular file inode (basic, non-extended)
 * ================================================================ */
struct squashfs_reg_inode {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t mtime;
    uint32_t inode_number;
    uint32_t start_block;   /* Offset from start of archive */
    uint32_t fragment;      /* Fragment index */
    uint32_t offset;        /* Offset within fragment */
    uint32_t file_size;     /* Uncompressed file size */
    /* Followed by block_list[]: uint32_t block_sizes[] */
} __attribute__((packed));

/* ================================================================
 * Extended regular file inode (for large files)
 * ================================================================ */
struct squashfs_lreg_inode {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t mtime;
    uint32_t inode_number;
    uint64_t start_block;
    uint64_t file_size;
    uint32_t sparse;        /* Number of sparse bytes */
    uint32_t nlink;         /* Number of hard links */
    uint32_t fragment;
    uint32_t offset;
    uint64_t xattr;
    /* Followed by block_list[]: uint32_t block_sizes[] */
} __attribute__((packed));

/* ================================================================
 * Directory inode (basic)
 * ================================================================ */
struct squashfs_dir_inode {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t mtime;
    uint32_t inode_number;
    uint32_t start_block;       /* Byte offset of directory table */
    uint32_t nlink;
    uint16_t file_size;         /* Uncompressed directory listing size */
    uint16_t offset;            /* Offset within block */
    uint32_t parent_inode;      /* Parent directory inode number */
} __attribute__((packed));

/* ================================================================
 * Extended directory inode
 * ================================================================ */
struct squashfs_ldir_inode {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t mtime;
    uint32_t inode_number;
    uint32_t nlink;
    uint32_t file_size;
    uint32_t start_block;
    uint32_t parent_inode;
    uint16_t i_count;           /* Directory index count */
    uint16_t offset;
    uint64_t xattr;
    /* Followed by directory index */
} __attribute__((packed));

/* ================================================================
 * Directory header (at the start of each directory block)
 * ================================================================ */
struct squashfs_dir_header {
    uint32_t count;         /* Number of entries + 1 */
    uint32_t start_block;   /* First inode start block */
    uint32_t inode_number;  /* First inode number */
} __attribute__((packed));

/* ================================================================
 * Directory entry (follows directory header)
 * ================================================================ */
struct squashfs_dir_entry {
    uint16_t offset;        /* Offset from start of metadata block */
    uint16_t inode_offset;  /* Inode number difference from header */
    uint16_t type;          /* Inode type */
    uint16_t name_size;     /* Size of name + 1 */
    /* Followed by name (name_size bytes) */
} __attribute__((packed));

/* ================================================================
 * Block list entry for compressed data
 * Bit 24 (0x01000000) set = uncompressed block
 * Lower 24 bits = block size (0 = sparse block, 4K zeros)
 * ================================================================ */
#define SQUASHFS_COMPRESSED_BIT     (1 << 24)
#define SQUASHFS_COMPRESSED_BIT_BLOCK (1 << 24)
#define SQUASHFS_COMPRESSED_SIZE(A)  ((A) & 0x00FFFFFF)

/* ================================================================
 * FS-private data structures
 * ================================================================ */

/* Filesystem-private data attached to super_block */
struct squashfs_sb_info {
    struct block_device *bdev;
    struct squashfs_superblock sblk;
    uint64_t *fragment_table;       /* Parsed fragment table entries */
    uint32_t *uid_table;            /* Parsed uid/gid table */
    uint32_t *guid_table;           /* Number of uid/gid entries */
};

/* Inode-private data attached to inode->priv */
struct squashfs_inode_info {
    struct squashfs_sb_info *sbi;
    uint32_t inode_number;
    uint32_t start_block;           /* Block offset in archive */
    uint32_t file_size;             /* Uncompressed file size */
    uint16_t inode_type;
    uint16_t mode;
    int      is_dir;
    /* For regular files: cached block list */
    uint32_t *block_list;           /* Array of block sizes */
    uint32_t  block_count;          /* Number of blocks */
    /* Fragment info */
    uint32_t fragment_index;
    uint32_t fragment_offset;
};

/* ================================================================
 * Function declarations
 * ================================================================ */
struct block_device;
struct super_block;
struct inode;
struct dentry;
struct file;

/*
 * squashfs_mount: Mount a Squashfs filesystem from a block device.
 * @bdev: block device containing the Squashfs image.
 * Returns a super_block on success, or NULL on failure.
 */
struct super_block *squashfs_mount(struct block_device *bdev);

#endif /* SQUASHFS_H */