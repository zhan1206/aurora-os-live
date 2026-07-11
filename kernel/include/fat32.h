/*
 * fat32.h - FAT32 filesystem on-disk structures
 *
 * All multi-byte fields are little-endian.
 */
#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

/* ================================================================
 * FAT32 file attribute constants
 * ================================================================ */
#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F  /* LFN entry: combination of READ_ONLY|HIDDEN|SYSTEM|VOLUME_ID */
#define FAT32_ATTR_LFN  0x0F  /* Alias for ATTR_LFN */
#define FAT32_LFN_LAST  0x40  /* LFN sequence number: last entry flag */

/* ================================================================
 * FAT32 cluster constants
 * ================================================================ */
#define FAT32_CLUSTER_FREE      0x00000000
#define FAT32_CLUSTER_RESERVED  0x00000001
#define FAT32_CLUSTER_BAD       0x0FFFFFF7
#define FAT32_CLUSTER_EOC_MIN   0x0FFFFFF8
#define FAT32_CLUSTER_EOC_MAX   0x0FFFFFFF
#define FAT32_CLUSTER_MASK      0x0FFFFFFF

/* ================================================================
 * FAT32 BIOS Parameter Block (BPB) — starts at sector 0, offset 0
 * ================================================================ */
struct fat32_bpb {
    uint8_t  jmp_boot[3];          /* Jump instruction to boot code */
    uint8_t  oem_name[8];          /* OEM name (e.g., "MSDOS5.0") */
    uint16_t bytes_per_sector;     /* Bytes per sector (usually 512) */
    uint8_t  sectors_per_cluster;  /* Sectors per cluster (power of 2) */
    uint16_t reserved_sectors;     /* Reserved sectors before first FAT */
    uint8_t  num_fats;             /* Number of FAT copies (usually 2) */
    uint16_t root_entries;         /* Number of root directory entries (0 for FAT32) */
    uint16_t total_sectors_16;     /* Total sectors (16-bit, 0 for FAT32) */
    uint8_t  media;                /* Media descriptor (0xF8 for fixed disk) */
    uint16_t fat_size_16;          /* FAT size in sectors (16-bit, 0 for FAT32) */
    uint16_t sectors_per_track;    /* Sectors per track */
    uint16_t num_heads;            /* Number of heads */
    uint32_t hidden_sectors;       /* Hidden sectors before partition */
    uint32_t total_sectors_32;     /* Total sectors (32-bit) */
} __attribute__((packed));

/* ================================================================
 * FAT32 Extended Boot Record (EBR) — follows BPB at offset 36
 * ================================================================ */
struct fat32_ebr {
    uint32_t sectors_per_fat;      /* FAT size in sectors (32-bit) */
    uint16_t flags;                /* Flags (active FAT, etc.) */
    uint16_t version;              /* FAT32 version (0) */
    uint32_t root_cluster;         /* First cluster of root directory (usually 2) */
    uint16_t fsinfo_sector;        /* Sector of FSInfo structure (usually 1) */
    uint16_t backup_boot_sector;   /* Backup boot sector (usually 6) */
    uint8_t  reserved[12];         /* Reserved */
    uint8_t  drive_number;         /* Drive number (0x80 for hard disk) */
    uint8_t  reserved1;            /* Reserved */
    uint8_t  boot_signature;       /* Extended boot signature (0x29) */
    uint32_t volume_id;            /* Volume serial number */
    uint8_t  volume_label[11];     /* Volume label (padded with spaces) */
    uint8_t  fs_type[8];           /* "FAT32   " */
} __attribute__((packed));

/* Combined BPB + EBR as a single boot sector structure */
struct fat32_boot_sector {
    struct fat32_bpb bpb;
    struct fat32_ebr ebr;
    /* Boot code and signature follow */
    uint8_t  boot_code[420];
    uint16_t boot_signature_2;     /* 0xAA55 */
} __attribute__((packed));

/* ================================================================
 * FAT32 Directory Entry (32 bytes, 8.3 format)
 * ================================================================ */
struct fat32_dir_entry {
    uint8_t  name[11];             /* 8.3 filename (space-padded, no null terminator) */
    uint8_t  attr;                 /* File attributes (ATTR_*) */
    uint8_t  nt_reserved;          /* Reserved for NT */
    uint8_t  create_time_tenth;    /* Creation time tenths of second (0-199) */
    uint16_t create_time;          /* Creation time (packed hh:mm:ss) */
    uint16_t create_date;          /* Creation date (packed yyyy/mm/dd) */
    uint16_t last_access_date;     /* Last access date */
    uint16_t first_cluster_high;   /* High 16 bits of first cluster */
    uint16_t write_time;           /* Last write time */
    uint16_t write_date;           /* Last write date */
    uint16_t first_cluster_low;    /* Low 16 bits of first cluster */
    uint32_t file_size;            /* File size in bytes (0 for directories) */
} __attribute__((packed));

/* ================================================================
 * FAT32 Long File Name (LFN) Entry (32 bytes)
 * ================================================================ */
struct fat32_lfn_entry {
    uint8_t  order;                /* Sequence number (1..N, last entry OR'd with 0x40) */
    uint8_t  name1[10];            /* First 5 UTF-16 characters */
    uint8_t  attr;                 /* Must be ATTR_LFN (0x0F) */
    uint8_t  type;                 /* Always 0 for LFN */
    uint8_t  checksum;             /* Checksum of the associated 8.3 name */
    uint8_t  name2[12];            /* Next 6 UTF-16 characters */
    uint16_t first_cluster_low;    /* Always 0 for LFN */
    uint8_t  name3[4];             /* Last 2 UTF-16 characters */
} __attribute__((packed));

/* ================================================================
 * FAT32 FSInfo sector (sector 1, optional but commonly used)
 * ================================================================ */
struct fat32_fsinfo {
    uint32_t lead_signature;       /* 0x41615252 */
    uint8_t  reserved1[480];
    uint32_t struct_signature;     /* 0x61417272 */
    uint32_t free_clusters;        /* Free cluster count (0xFFFFFFFF if unknown) */
    uint32_t next_free_cluster;    /* Hint for next free cluster (0xFFFFFFFF if unknown) */
    uint8_t  reserved2[12];
    uint32_t trail_signature;      /* 0xAA550000 */
} __attribute__((packed));

/* ================================================================
 * fs-private data structures
 * ================================================================ */

/* Filesystem-private data attached to super_block */
struct fat32_sb_info {
    struct block_device *bdev;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t fat_start;            /* Starting sector of FAT area */
    uint32_t data_start;           /* Starting sector of data area */
    uint32_t cluster_size;         /* bytes_per_sector * sectors_per_cluster */
    uint32_t total_clusters;
    uint32_t *fat_cache;           /* Cached FAT table (NULL if not cached) */
};

/* Inode-private data attached to inode->priv */
struct fat32_inode_info {
    struct fat32_sb_info *sbi;     /* Pointer to filesystem-private data */
    uint32_t first_cluster;        /* First cluster of file/directory */
    uint32_t file_size;            /* File size in bytes */
    uint8_t  attributes;           /* FAT attributes */
    uint8_t  is_dir;               /* 1 if directory */
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
 * fat32_mount: Mount a FAT32 filesystem from a block device.
 * @bdev: block device containing the FAT32 image.
 * Returns a super_block on success, or NULL on failure.
 */
struct super_block *fat32_mount(struct block_device *bdev);

/* ================================================================
 * LFN (Long File Name) API
 * ================================================================ */

/*
 * fat32_read_lfn: Read a long file name from LFN entries.
 * Given an array of LFN entries (in reverse order), produce a null-terminated
 * ASCII string. Returns the length of the name, or 0 on failure.
 */
int fat32_read_lfn(const struct fat32_lfn_entry *lfn_entries, int num_entries,
                   char *out, int out_max);

/*
 * fat32_write_lfn: Build LFN entries + 8.3 short name entry for a long name.
 * Writes the entries into the directory at the given cluster and offset.
 * @sbi: filesystem private data
 * @dir_cluster: cluster of the directory
 * @dir_offset: byte offset within the directory for the first LFN entry
 * @name: the long file name
 * @attr: file attributes for the 8.3 entry
 * @first_cluster: first cluster of the file/directory
 * @file_size: size of the file
 * Returns number of entries written (LFN + 8.3), or negative on error.
 */
int fat32_write_lfn(struct fat32_sb_info *sbi, uint32_t dir_cluster,
                    uint32_t dir_offset, const char *name, uint8_t attr,
                    uint32_t first_cluster, uint32_t file_size);

/*
 * fat32_shortname_from_lfn: Generate an 8.3 short name from a long file name.
 * @lfn: the long file name (null-terminated)
 * @short_name: output buffer for the 11-byte 8.3 name (space-padded)
 * Returns 0 on success, -1 if the name cannot be represented as 8.3.
 */
int fat32_shortname_from_lfn(const char *lfn, uint8_t *short_name);

/*
 * fat32_lfn_checksum: Compute the 8.3 checksum for LFN validation.
 * @short_name: the 11-byte 8.3 short name
 * Returns the checksum byte.
 */
uint8_t fat32_lfn_checksum(const uint8_t *short_name);

/* ================================================================
 * Directory operations API
 * ================================================================ */

/*
 * fat32_mkdir: Create a subdirectory in the given parent directory.
 * @sbi: filesystem private data
 * @parent_cluster: first cluster of the parent directory
 * @name: name of the new directory
 * Returns 0 on success, negative on error.
 */
int fat32_mkdir(struct fat32_sb_info *sbi, uint32_t parent_cluster,
                const char *name);

/*
 * fat32_rmdir: Remove an empty subdirectory from the given parent directory.
 * @sbi: filesystem private data
 * @parent_cluster: first cluster of the parent directory
 * @name: name of the directory to remove
 * Returns 0 on success, negative on error.
 */
int fat32_rmdir(struct fat32_sb_info *sbi, uint32_t parent_cluster,
                const char *name);

/* ================================================================
 * Cluster allocation API
 * ================================================================ */

/*
 * fat32_alloc_cluster: Allocate a free cluster, mark it as end-of-chain.
 * @sbi: filesystem private data
 * Returns the cluster number, or 0 on failure.
 */
uint32_t fat32_alloc_cluster(struct fat32_sb_info *sbi);

/*
 * fat32_free_cluster_chain: Free an entire cluster chain.
 * @sbi: filesystem private data
 * @start_cluster: first cluster in the chain to free
 * Returns 0 on success, negative on error.
 */
int fat32_free_cluster_chain(struct fat32_sb_info *sbi, uint32_t start_cluster);

/*
 * fat32_find_free_cluster: Find a free cluster in the FAT table.
 * @sbi: filesystem private data
 * Returns the cluster number, or 0 on failure.
 */
uint32_t fat32_find_free_cluster(struct fat32_sb_info *sbi);

#endif /* FAT32_H */