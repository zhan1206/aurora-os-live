/*
 * fat32.c - FAT32 filesystem driver for AuroraOS
 *
 * Implements a read/write FAT32 driver that integrates with the VFS.
 * Supports:
 *   - BPB/EBR reading and verification
 *   - FAT table cluster chain traversal
 *   - Cluster allocation and deallocation
 *   - Directory lookup with 8.3 + LFN support
 *   - File creation in directories
 *   - File read/write operations
 *
 * Block device I/O is abstracted through struct block_device.
 */
#include "include/fat32.h"
#include "fs.h"
#include "block_dev.h"
#include "include/log.h"
#include "include/errno.h"
#include "include/string.h"
#include "mem.h"

/* ================================================================
 * Block device helpers
 * ================================================================ */

/*
 * Read one or more sectors from the block device.
 */
static int read_sectors(struct block_device *bdev, uint64_t sector,
                        uint32_t count, void *buf) {
    return block_dev_read(bdev, buf, sector, count);
}

/*
 * Write one or more sectors to the block device.
 */
static int write_sectors(struct block_device *bdev, uint64_t sector,
                         uint32_t count, const void *buf) {
    return block_dev_write(bdev, buf, sector, count);
}

/*
 * Read a full cluster from the data area.
 */
static int read_cluster(struct fat32_sb_info *sbi, uint32_t cluster, void *buf) {
    uint64_t sector = (uint64_t)(cluster - 2) * sbi->sectors_per_cluster +
                      sbi->data_start;
    return read_sectors(sbi->bdev, sector, sbi->sectors_per_cluster, buf);
}

/*
 * Write a full cluster to the data area.
 */
static int write_cluster(struct fat32_sb_info *sbi, uint32_t cluster,
                         const void *buf) {
    uint64_t sector = (uint64_t)(cluster - 2) * sbi->sectors_per_cluster +
                      sbi->data_start;
    return write_sectors(sbi->bdev, sector, sbi->sectors_per_cluster, buf);
}

/* ================================================================
 * FAT table operations
 * ================================================================ */

/*
 * Read a FAT32 entry from the FAT table.
 * @sbi: filesystem private data
 * @cluster: cluster number whose FAT entry to read
 * Returns the next cluster number, or 0x0FFFFFFF on error.
 */
static uint32_t fat32_get_cluster(struct fat32_sb_info *sbi, uint32_t cluster) {
    if (cluster < 2 || cluster >= FAT32_CLUSTER_EOC_MAX)
        return FAT32_CLUSTER_EOC_MAX;

    /* Check cache first */
    if (sbi->fat_cache && cluster < sbi->total_clusters + 2) {
        return sbi->fat_cache[cluster] & FAT32_CLUSTER_MASK;
    }

    /* Read from disk */
    uint32_t fat_offset = cluster * 4;
    uint64_t fat_sector = sbi->fat_start + (fat_offset / sbi->bytes_per_sector);
    uint32_t offset_in_sector = fat_offset % sbi->bytes_per_sector;

    uint8_t *buf = (uint8_t *)kmalloc(sbi->bytes_per_sector);
    if (!buf) return FAT32_CLUSTER_EOC_MAX;

    if (read_sectors(sbi->bdev, fat_sector, 1, buf) < 0) {
        kfree(buf);
        return FAT32_CLUSTER_EOC_MAX;
    }

    uint32_t value = *(uint32_t *)(buf + offset_in_sector);
    kfree(buf);
    return value & FAT32_CLUSTER_MASK;
}

/*
 * Write a FAT32 entry to the FAT table.
 * Updates all FAT copies if multiple FATs are present.
 * @sbi: filesystem private data
 * @cluster: cluster number to write
 * @value: new FAT entry value
 * Returns 0 on success, negative on error.
 */
static int fat32_set_cluster(struct fat32_sb_info *sbi, uint32_t cluster,
                             uint32_t value) {
    if (cluster < 2 || cluster >= FAT32_CLUSTER_EOC_MAX)
        return -EINVAL;

    value &= FAT32_CLUSTER_MASK;

    /* Update cache if present */
    if (sbi->fat_cache && cluster < sbi->total_clusters + 2) {
        sbi->fat_cache[cluster] = value;
    }

    /* Write to all FAT copies */
    uint32_t fat_offset = cluster * 4;
    uint64_t fat_sector = sbi->fat_start + (fat_offset / sbi->bytes_per_sector);
    uint32_t offset_in_sector = fat_offset % sbi->bytes_per_sector;

    uint8_t *buf = (uint8_t *)kmalloc(sbi->bytes_per_sector);
    if (!buf) return -ENOMEM;

    for (uint8_t fat = 0; fat < sbi->num_fats; fat++) {
        uint64_t sec = fat_sector + (uint64_t)fat * sbi->sectors_per_fat;

        /* Read-modify-write */
        if (read_sectors(sbi->bdev, sec, 1, buf) < 0) {
            kfree(buf);
            return -EIO;
        }

        *(uint32_t *)(buf + offset_in_sector) = value;

        if (write_sectors(sbi->bdev, sec, 1, buf) < 0) {
            kfree(buf);
            return -EIO;
        }
    }

    kfree(buf);
    return 0;
}

/*
 * Find a free cluster.
 * Scans the FAT table for a cluster marked as free (0x00000000).
 * Returns the cluster number, or 0 on failure.
 */
uint32_t fat32_find_free_cluster(struct fat32_sb_info *sbi) {
    uint32_t cluster;

    for (cluster = 2; cluster < sbi->total_clusters + 2; cluster++) {
        uint32_t val = fat32_get_cluster(sbi, cluster);
        if (val == FAT32_CLUSTER_FREE) {
            /* Mark as end-of-chain */
            fat32_set_cluster(sbi, cluster, FAT32_CLUSTER_EOC_MAX);
            return cluster;
        }
    }

    return 0;
}

/*
 * Convert a cluster number to a sector number.
 * Cluster 2 maps to the first sector of the data area.
 */
static uint64_t fat32_cluster_to_sector(struct fat32_sb_info *sbi,
                                        uint32_t cluster) {
    return (uint64_t)(cluster - 2) * sbi->sectors_per_cluster + sbi->data_start;
}

/*
 * Follow the cluster chain for a given number of steps.
 * Returns the cluster number after advancing, or 0x0FFFFFFF at end-of-chain.
 */
static uint32_t fat32_get_nth_cluster(struct fat32_sb_info *sbi,
                                      uint32_t start_cluster, uint32_t steps) {
    uint32_t cluster = start_cluster;
    for (uint32_t i = 0; i < steps; i++) {
        uint32_t next = fat32_get_cluster(sbi, cluster);
        if (next >= FAT32_CLUSTER_EOC_MIN)
            return FAT32_CLUSTER_EOC_MAX;
        cluster = next;
    }
    return cluster;
}

/*
 * Extend the cluster chain by allocating a new cluster and appending it.
 * Returns the new cluster number, or 0 on failure.
 */
static uint32_t fat32_append_cluster(struct fat32_sb_info *sbi,
                                     uint32_t last_cluster) {
    uint32_t new_cluster = fat32_find_free_cluster(sbi);
    if (new_cluster == 0) return 0;

    /* Link old last cluster to new cluster */
    if (last_cluster >= 2 && last_cluster < FAT32_CLUSTER_EOC_MIN) {
        fat32_set_cluster(sbi, last_cluster, new_cluster);
    }

    return new_cluster;
}

/*
 * Free the entire cluster chain starting from the given cluster.
 */
int fat32_free_cluster_chain(struct fat32_sb_info *sbi,
                                    uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
        uint32_t next = fat32_get_cluster(sbi, cluster);
        fat32_set_cluster(sbi, cluster, FAT32_CLUSTER_FREE);
        if (next >= FAT32_CLUSTER_EOC_MIN)
            break;
        cluster = next;
    }
    return 0;
}

/*
 * Walk the cluster chain and copy data to/from a buffer.
 * Used for both reading and writing files.
 *
 * @sbi: filesystem private data
 * @start_cluster: first cluster of the file
 * @file_size: current file size (for read bounds checking)
 * @buf: user buffer
 * @count: bytes to read/write
 * @offset: byte offset within the file
 * @is_write: 0 for read, 1 for write
 * @new_size: (out, write only) updated file size after write
 * Returns number of bytes transferred, or negative on error.
 */
static ssize_t fat32_transfer_file(struct fat32_sb_info *sbi,
                                   uint32_t start_cluster,
                                   uint32_t file_size,
                                   uint8_t *buf, size_t count,
                                   uint64_t offset, int is_write,
                                   uint32_t *new_size) {
    uint32_t cluster_size = sbi->cluster_size;
    uint32_t clusters_per_chain = 0;
    uint32_t cluster;

    /* Count clusters in chain */
    cluster = start_cluster;
    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
        clusters_per_chain++;
        uint32_t next = fat32_get_cluster(sbi, cluster);
        if (next >= FAT32_CLUSTER_EOC_MIN) break;
        cluster = next;
    }

    /* Cast to uint64_t before multiplication to prevent overflow */
    uint64_t max_offset = (uint64_t)clusters_per_chain * cluster_size;

    /* For read: bound by file_size */
    if (!is_write && offset >= file_size) return 0;
    if (!is_write && offset + count > (uint64_t)file_size)
        count = (size_t)((uint64_t)file_size - offset);

    /* For write: extend chain if needed */
    if (is_write && offset + count > (uint64_t)max_offset) {
        uint32_t needed_clusters = (uint32_t)(((uint64_t)offset + count + cluster_size - 1) /
                                              cluster_size);
        uint32_t last_cluster = start_cluster;
        /* Find last cluster in chain */
        cluster = start_cluster;
        while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
            last_cluster = cluster;
            uint32_t next = fat32_get_cluster(sbi, cluster);
            if (next >= FAT32_CLUSTER_EOC_MIN) break;
            cluster = next;
        }
        while (clusters_per_chain < needed_clusters) {
            uint32_t nc = fat32_append_cluster(sbi, last_cluster);
            if (nc == 0) {
                if (new_size) *new_size = file_size;
                return (offset > file_size) ? (ssize_t)(offset - file_size) : 0;
            }
            last_cluster = nc;
            clusters_per_chain++;
        }
    }

    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return -ENOMEM;

    size_t total_done = 0;
    size_t remaining = count;

    while (remaining > 0) {
        uint32_t cluster_idx = (uint32_t)(offset / cluster_size);
        uint32_t cluster_off = (uint32_t)(offset % cluster_size);

        uint32_t target_cluster = fat32_get_nth_cluster(sbi, start_cluster,
                                                        cluster_idx);
        if (target_cluster == FAT32_CLUSTER_EOC_MAX) {
            kfree(cluster_buf);
            return total_done > 0 ? (ssize_t)total_done : -EIO;
        }

        size_t chunk = (size_t)(cluster_size - cluster_off);
        if (chunk > remaining) chunk = remaining;

        if (is_write) {
            /* Read-modify-write for partial clusters */
            if (cluster_off != 0 || chunk < cluster_size) {
                if (read_cluster(sbi, target_cluster, cluster_buf) < 0) {
                    kfree(cluster_buf);
                    return total_done > 0 ? (ssize_t)total_done : -EIO;
                }
            }
            memcpy(cluster_buf + cluster_off, buf + total_done, chunk);
            if (write_cluster(sbi, target_cluster, cluster_buf) < 0) {
                kfree(cluster_buf);
                return total_done > 0 ? (ssize_t)total_done : -EIO;
            }
        } else {
            if (read_cluster(sbi, target_cluster, cluster_buf) < 0) {
                kfree(cluster_buf);
                return total_done > 0 ? (ssize_t)total_done : -EIO;
            }
            memcpy(buf + total_done, cluster_buf + cluster_off, chunk);
        }

        offset += chunk;
        total_done += chunk;
        remaining -= chunk;
    }

    kfree(cluster_buf);

    if (is_write && new_size) {
        if (offset > (uint64_t)file_size)
            *new_size = (uint32_t)offset;
        else
            *new_size = file_size;
    }

    return (ssize_t)total_done;
}

/* ================================================================
 * LFN helpers
 * ================================================================ */

/*
 * Compute the 8.3 checksum for LFN validation.
 * Used to verify that LFN entries belong to the following 8.3 entry.
 */
static uint8_t fat32_checksum(const uint8_t *short_name) {
    uint8_t sum = 0;
    for (int i = 11; i != 0; i--) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + *short_name++);
    }
    return sum;
}

/*
 * Convert a UTF-16LE character to ASCII (lossy).
 * Non-ASCII characters are mapped to '_'.
 */
static char utf16_to_ascii(uint16_t ch) {
    if (ch < 0x80) return (char)ch;
    return '_';
}

/*
 * Extract a long file name from an array of LFN entries.
 * LFN entries appear in reverse order before the 8.3 entry.
 * Returns the length of the name, or 0 on failure.
 */
static int fat32_parse_lfn(const struct fat32_lfn_entry *lfn_entries,
                           int num_entries, char *out, int out_max) {
    int pos = 0;
    for (int i = num_entries - 1; i >= 0; i--) {
        const struct fat32_lfn_entry *lfn = &lfn_entries[i];
        uint8_t order = lfn->order & 0x3F;

        if (order != (uint8_t)(i + 1))
            return 0;

        /* name1: 5 UTF-16 characters */
        for (int j = 0; j < 10 && pos < out_max - 1; j += 2) {
            uint16_t ch = (uint16_t)lfn->name1[j] |
                          ((uint16_t)lfn->name1[j + 1] << 8);
            if (ch == 0) break;
            out[pos++] = utf16_to_ascii(ch);
        }

        /* name2: 6 UTF-16 characters */
        for (int j = 0; j < 12 && pos < out_max - 1; j += 2) {
            uint16_t ch = (uint16_t)lfn->name2[j] |
                          ((uint16_t)lfn->name2[j + 1] << 8);
            if (ch == 0) break;
            out[pos++] = utf16_to_ascii(ch);
        }

        /* name3: 2 UTF-16 characters */
        for (int j = 0; j < 4 && pos < out_max - 1; j += 2) {
            uint16_t ch = (uint16_t)lfn->name3[j] |
                          ((uint16_t)lfn->name3[j + 1] << 8);
            if (ch == 0) break;
            out[pos++] = utf16_to_ascii(ch);
        }
    }
    out[pos] = '\0';
    return pos;
}

/*
 * Convert a fat32_dir_entry 8.3 name to a null-terminated string.
 * The name is space-padded in the directory entry.
 * Returns the length of the name (without null terminator).
 */
static int fat32_short_name_to_str(const uint8_t *raw_name, char *out) {
    int i, j = 0;

    /* Copy the 8-char name part, stopping at first space */
    for (i = 0; i < 8; i++) {
        if (raw_name[i] == ' ') break;
        out[j++] = (char)raw_name[i];
    }

    /* Add extension if present */
    if (raw_name[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11; i++) {
            if (raw_name[i] == ' ') break;
            out[j++] = (char)raw_name[i];
        }
    }

    out[j] = '\0';
    return j;
}

/*
 * Convert a string to a FAT32 8.3 short name.
 * Returns 0 on success, -1 if the name cannot be represented as 8.3.
 */
static int fat32_str_to_short_name(const char *name, uint8_t *short_name) {
    int name_len = 0;
    const char *dot = NULL;
    int i;

    /* Find the dot and compute length */
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] == '.') {
            if (dot != NULL) return -1; /* multiple dots */
            dot = &name[i];
        }
    }
    name_len = i;

    if (dot) {
        int base_len = (int)(dot - name);
        int ext_len = (int)(name_len - base_len - 1);

        if (base_len > 8 || ext_len > 3)
            return -1;

        /* Base name */
        for (i = 0; i < 8; i++) {
            if (i < base_len) {
                char c = name[i];
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                short_name[i] = (uint8_t)c;
            } else {
                short_name[i] = ' ';
            }
        }

        /* Extension */
        for (i = 0; i < 3; i++) {
            if (i < ext_len) {
                char c = dot[i + 1];
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                short_name[8 + i] = (uint8_t)c;
            } else {
                short_name[8 + i] = ' ';
            }
        }
    } else {
        if (name_len > 8) return -1;

        for (i = 0; i < 8; i++) {
            if (i < name_len) {
                char c = name[i];
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                short_name[i] = (uint8_t)c;
            } else {
                short_name[i] = ' ';
            }
        }
        for (i = 8; i < 11; i++) short_name[i] = ' ';
    }

    return 0;
}

/* ================================================================
 * Directory operations
 * ================================================================ */

/*
 * Find a directory entry by name within a directory cluster chain.
 * Supports both 8.3 short names and LFN entries.
 *
 * @sbi: filesystem private data
 * @dir_cluster: first cluster of the directory
 * @dir_size: size of the directory in bytes
 * @name: name to search for
 * @out_entry: output buffer for the found 8.3 entry (may be NULL)
 * @out_first_cluster: output for the first cluster of the found file
 * @out_file_size: output for the file size
 * @out_attr: output for the file attributes
 * @out_name: output for the resolved name (LFN or 8.3, caller frees)
 * Returns 0 on success, -ENOENT if not found, negative on error.
 */
static int fat32_find_entry(struct fat32_sb_info *sbi,
                            uint32_t dir_cluster, uint32_t dir_size,
                            const char *name,
                            struct fat32_dir_entry *out_entry,
                            uint32_t *out_first_cluster,
                            uint32_t *out_file_size,
                            uint8_t *out_attr,
                            char **out_name,
                            uint32_t *out_dir_cluster,
                            uint32_t *out_dir_offset) {
    uint32_t cluster_size = sbi->cluster_size;
    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat32_dir_entry);
    size_t name_len = strlen(name);

    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return -ENOMEM;

    /* LFN buffer: accumulate LFN entries while scanning */
    struct fat32_lfn_entry lfn_buf[20];
    int lfn_count = 0;

    uint32_t cluster = dir_cluster;
    uint32_t entry_idx = 0;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {

        if (read_cluster(sbi, cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -EIO;
        }

        for (uint32_t i = 0; i < entries_per_cluster; i++, entry_idx++) {
            uint8_t *raw = cluster_buf + i * sizeof(struct fat32_dir_entry);

            /* Check for end of directory (first byte is 0) */
            if (raw[0] == 0x00) {
                kfree(cluster_buf);
                return -ENOENT;
            }

            /* Check for deleted entry (first byte is 0xE5) */
            if (raw[0] == 0xE5) {
                lfn_count = 0;
                continue;
            }

            uint8_t attr = raw[11];

            /* LFN entry */
            if (attr == ATTR_LFN) {
                if (lfn_count < 20) {
                    memcpy(&lfn_buf[lfn_count], raw,
                           sizeof(struct fat32_lfn_entry));
                    lfn_count++;
                }
                continue;
            }

            /* 8.3 entry */
            struct fat32_dir_entry *de = (struct fat32_dir_entry *)raw;

            /* Skip volume label */
            if (de->attr & ATTR_VOLUME_ID) {
                lfn_count = 0;
                continue;
            }

            /* Try to match using LFN first */
            int matched = 0;
            char resolved_name[256];

            if (lfn_count > 0) {
                int lfn_len = fat32_parse_lfn(lfn_buf, lfn_count,
                                              resolved_name, 256);
                if (lfn_len > 0 && (int)name_len == lfn_len &&
                    strncmp(resolved_name, name, name_len) == 0) {
                    matched = 1;
                }
            }

            /* Fall back to 8.3 name matching */
            if (!matched) {
                int short_len = fat32_short_name_to_str(de->name, resolved_name);
                if ((int)name_len == short_len &&
                    strncmp(resolved_name, name, name_len) == 0) {
                    matched = 1;
                }
            }

            if (matched) {
                uint32_t fc = ((uint32_t)de->first_cluster_high << 16) |
                              de->first_cluster_low;

                if (out_entry)
                    memcpy(out_entry, de, sizeof(*out_entry));
                if (out_first_cluster)
                    *out_first_cluster = fc;
                if (out_file_size)
                    *out_file_size = de->file_size;
                if (out_attr)
                    *out_attr = de->attr;
                if (out_name) {
                    size_t rlen = strlen(resolved_name);
                    char *nm = (char *)kmalloc(rlen + 1);
                    if (nm) {
                        memcpy(nm, resolved_name, rlen);
                        nm[rlen] = '\0';
                    }
                    *out_name = nm;
                }
                /* Bug #20: Return the directory entry position for file size updates */
                if (out_dir_cluster)
                    *out_dir_cluster = cluster;
                if (out_dir_offset)
                    *out_dir_offset = i * sizeof(struct fat32_dir_entry);

                kfree(cluster_buf);
                return 0;
            }

            lfn_count = 0;
        }

        /* Move to next cluster */
        uint32_t next = fat32_get_cluster(sbi, cluster);
        if (next >= FAT32_CLUSTER_EOC_MIN) break;
        cluster = next;
    }

    kfree(cluster_buf);
    return -ENOENT;
}

/*
 * Find a free directory entry slot (either an unused entry or a deleted one).
 * Returns 0 on success, negative on error.
 */
static int fat32_find_free_slot(struct fat32_sb_info *sbi,
                                uint32_t dir_cluster,
                                uint32_t *out_cluster,
                                uint32_t *out_offset) {
    uint32_t cluster_size = sbi->cluster_size;
    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat32_dir_entry);

    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return -ENOMEM;

    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
        if (read_cluster(sbi, cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -EIO;
        }

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t *raw = cluster_buf + i * sizeof(struct fat32_dir_entry);

            /* Free entry (first byte 0x00 or 0xE5) */
            if (raw[0] == 0x00 || raw[0] == 0xE5) {
                *out_cluster = cluster;
                *out_offset = i * sizeof(struct fat32_dir_entry);
                kfree(cluster_buf);
                return 0;
            }
        }

        uint32_t next = fat32_get_cluster(sbi, cluster);
        if (next >= FAT32_CLUSTER_EOC_MIN) {
            break;
        }
        cluster = next;
    }

    /* Need to allocate a new cluster */
    uint32_t new_cluster = fat32_append_cluster(sbi, cluster);
    if (new_cluster == 0) {
        kfree(cluster_buf);
        return -ENOSPC;
    }

    /* Zero the new cluster */
    memset(cluster_buf, 0, cluster_size);
    if (write_cluster(sbi, new_cluster, cluster_buf) < 0) {
        kfree(cluster_buf);
        return -EIO;
    }

    *out_cluster = new_cluster;
    *out_offset = 0;
    kfree(cluster_buf);
    return 0;
}

/*
 * Compute the total size of a directory by walking the cluster chain.
 */
static uint64_t fat32_get_dir_size(struct fat32_sb_info *sbi,
                                   uint32_t dir_cluster) {
    uint32_t cluster_size = sbi->cluster_size;
    uint64_t total_size = 0;
    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
        total_size += cluster_size;
        uint32_t next = fat32_get_cluster(sbi, cluster);
        if (next >= FAT32_CLUSTER_EOC_MIN) break;
        cluster = next;
    }
    return total_size;
}

/*
 * Create a new directory entry in the given directory.
 * Uses LFN writing if the name cannot be represented as 8.3.
 * Returns 0 on success, negative on error.
 */
static int fat32_create_entry(struct fat32_sb_info *sbi,
                              uint32_t dir_cluster,
                              const char *name, uint8_t attr,
                              uint32_t first_cluster, uint32_t file_size) {
    uint32_t cluster_size = sbi->cluster_size;

    /* Check if the name fits in 8.3 format */
    uint8_t short_name[11];
    int needs_lfn = (fat32_str_to_short_name(name, short_name) != 0);

    /* Find a free slot */
    uint32_t target_cluster, target_offset;
    int ret = fat32_find_free_slot(sbi, dir_cluster,
                                   &target_cluster, &target_offset);
    if (ret < 0) return ret;

    if (needs_lfn) {
        /* Use LFN writing path */
        return fat32_write_lfn(sbi, target_cluster, target_offset,
                               name, attr, first_cluster, file_size);
    }

    /* Simple 8.3 path */
    /* Read the cluster containing the free slot */
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return -ENOMEM;

    if (read_cluster(sbi, target_cluster, cluster_buf) < 0) {
        kfree(cluster_buf);
        return -EIO;
    }

    /* Build the 8.3 directory entry */
    struct fat32_dir_entry *de =
        (struct fat32_dir_entry *)(cluster_buf + target_offset);
    memset(de, 0, sizeof(*de));
    memcpy(de->name, short_name, 11);

    de->attr = attr;
    de->nt_reserved = 0;
    de->create_time_tenth = 0;
    de->create_time = 0;
    de->create_date = 0;
    de->last_access_date = 0;
    de->first_cluster_high = (uint16_t)(first_cluster >> 16);
    de->write_time = 0;
    de->write_date = 0;
    de->first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
    de->file_size = file_size;

    /* Write the cluster back */
    if (write_cluster(sbi, target_cluster, cluster_buf) < 0) {
        kfree(cluster_buf);
        return -EIO;
    }

    kfree(cluster_buf);
    return 0;
}

/* ================================================================
 * Public LFN API wrappers
 * ================================================================ */

/*
 * fat32_read_lfn: Public wrapper for fat32_parse_lfn.
 * Reads a long file name from an array of LFN entries.
 */
int fat32_read_lfn(const struct fat32_lfn_entry *lfn_entries, int num_entries,
                   char *out, int out_max) {
    return fat32_parse_lfn(lfn_entries, num_entries, out, out_max);
}

/*
 * fat32_lfn_checksum: Public wrapper for fat32_checksum.
 * Computes the 8.3 checksum for LFN validation.
 */
uint8_t fat32_lfn_checksum(const uint8_t *short_name) {
    return fat32_checksum(short_name);
}

/*
 * fat32_shortname_from_lfn: Public wrapper for fat32_str_to_short_name.
 * Generates an 8.3 short name from a long file name.
 */
int fat32_shortname_from_lfn(const char *lfn, uint8_t *short_name) {
    return fat32_str_to_short_name(lfn, short_name);
}

/*
 * fat32_write_lfn: Build and write LFN entries + 8.3 short name entry.
 *
 * The LFN entries are stored in reverse order (last entry first) before
 * the 8.3 short name entry. Each LFN entry can hold up to 13 UTF-16LE
 * characters (5 + 6 + 2).
 *
 * Returns the number of entries written (LFN + 8.3), or negative on error.
 */
int fat32_write_lfn(struct fat32_sb_info *sbi, uint32_t dir_cluster,
                    uint32_t dir_offset, const char *name, uint8_t attr,
                    uint32_t first_cluster, uint32_t file_size) {
    uint32_t cluster_size = sbi->cluster_size;
    int name_len = (int)strlen(name);
    if (name_len == 0) return -EINVAL;

    /* Calculate number of LFN entries needed: ceil(name_len / 13) */
    int lfn_entries = (name_len + 12) / 13;
    if (lfn_entries > 20) return -ENAMETOOLONG;

    /* Generate the 8.3 short name */
    uint8_t short_name[11];
    if (fat32_str_to_short_name(name, short_name) != 0) {
        /* Generate a fallback short name */
        int i;
        for (i = 0; i < 6 && name[i] != '\0' && name[i] != '.'; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            short_name[i] = (uint8_t)c;
        }
        short_name[i++] = '~';
        short_name[i++] = '1';
        while (i < 8) short_name[i++] = ' ';
        short_name[8] = 'T'; short_name[9] = 'X'; short_name[10] = 'T';
    }

    uint8_t checksum = fat32_checksum(short_name);

    /* Total entries = LFN entries + 1 (8.3 entry) */
    int total_entries = lfn_entries + 1;

    /* Allocate buffer for the entries */
    uint32_t entry_size = sizeof(struct fat32_dir_entry);
    uint32_t needed = (uint32_t)total_entries * entry_size;

    uint8_t *entry_buf = (uint8_t *)kmalloc(needed + cluster_size);
    if (!entry_buf) return -ENOMEM;
    memset(entry_buf, 0, needed + cluster_size);

    /* Build LFN entries in reverse order */
    int name_pos = 0;
    for (int e = 0; e < lfn_entries; e++) {
        int entry_idx = lfn_entries - 1 - e; /* stored first-to-last */
        struct fat32_lfn_entry *lfn =
            (struct fat32_lfn_entry *)(entry_buf + entry_idx * 32);

        uint8_t seq = (uint8_t)(e + 1);
        if (e == lfn_entries - 1) seq |= FAT32_LFN_LAST;

        lfn->order = seq;
        lfn->attr = ATTR_LFN;
        lfn->type = 0;
        lfn->checksum = checksum;
        lfn->first_cluster_low = 0;

        /* Fill name1 (5 chars = 10 bytes UTF-16LE) */
        for (int j = 0; j < 5; j++) {
            if (name_pos < name_len) {
                uint16_t ch = (uint16_t)(unsigned char)name[name_pos++];
                lfn->name1[j * 2] = (uint8_t)(ch & 0xFF);
                lfn->name1[j * 2 + 1] = (uint8_t)(ch >> 8);
            } else {
                lfn->name1[j * 2] = 0;
                lfn->name1[j * 2 + 1] = 0;
            }
        }

        /* Fill name2 (6 chars = 12 bytes UTF-16LE) */
        for (int j = 0; j < 6; j++) {
            if (name_pos < name_len) {
                uint16_t ch = (uint16_t)(unsigned char)name[name_pos++];
                lfn->name2[j * 2] = (uint8_t)(ch & 0xFF);
                lfn->name2[j * 2 + 1] = (uint8_t)(ch >> 8);
            } else {
                lfn->name2[j * 2] = 0;
                lfn->name2[j * 2 + 1] = 0;
            }
        }

        /* Fill name3 (2 chars = 4 bytes UTF-16LE) */
        for (int j = 0; j < 2; j++) {
            if (name_pos < name_len) {
                uint16_t ch = (uint16_t)(unsigned char)name[name_pos++];
                lfn->name3[j * 2] = (uint8_t)(ch & 0xFF);
                lfn->name3[j * 2 + 1] = (uint8_t)(ch >> 8);
            } else {
                lfn->name3[j * 2] = 0;
                lfn->name3[j * 2 + 1] = 0;
            }
        }
    }

    /* Build the 8.3 short name entry (last entry) */
    {
        struct fat32_dir_entry *de =
            (struct fat32_dir_entry *)(entry_buf + lfn_entries * 32);
        memset(de, 0, sizeof(*de));
        memcpy(de->name, short_name, 11);
        de->attr = attr;
        de->nt_reserved = 0;
        de->create_time_tenth = 0;
        de->create_time = 0;
        de->create_date = 0;
        de->last_access_date = 0;
        de->first_cluster_high = (uint16_t)(first_cluster >> 16);
        de->write_time = 0;
        de->write_date = 0;
        de->first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
        de->file_size = file_size;
    }

    /* Write the entries into the directory.
     * The entries may span multiple clusters. */
    uint32_t cluster = dir_cluster;
    uint32_t offset = dir_offset;
    uint32_t written = 0;

    while (written < needed) {
        /* Find the cluster for this offset */
        uint32_t cluster_idx = offset / cluster_size;
        uint32_t cluster_off = offset % cluster_size;

        uint32_t target_cluster = cluster;
        for (uint32_t s = 0; s < cluster_idx; s++) {
            uint32_t next = fat32_get_cluster(sbi, target_cluster);
            if (next >= FAT32_CLUSTER_EOC_MIN) {
                kfree(entry_buf);
                return -EIO;
            }
            target_cluster = next;
        }

        uint32_t chunk = cluster_size - cluster_off;
        if (chunk > needed - written) chunk = needed - written;

        /* Read-modify-write */
        uint8_t *cluster_buf = entry_buf + needed;
        if (read_cluster(sbi, target_cluster, cluster_buf) < 0) {
            kfree(entry_buf);
            return -EIO;
        }
        memcpy(cluster_buf + cluster_off, entry_buf + written, chunk);
        if (write_cluster(sbi, target_cluster, cluster_buf) < 0) {
            kfree(entry_buf);
            return -EIO;
        }

        written += chunk;
        offset += chunk;
    }

    kfree(entry_buf);
    return total_entries;
}

/* ================================================================
 * Directory creation and removal
 * ================================================================ */

/*
 * fat32_mkdir: Create a subdirectory.
 *
 * 1. Allocates a new cluster for the directory.
 * 2. Writes "." and ".." entries in the new cluster.
 * 3. Creates a directory entry in the parent directory using LFN.
 */
int fat32_mkdir(struct fat32_sb_info *sbi, uint32_t parent_cluster,
                const char *name) {
    uint32_t cluster_size = sbi->cluster_size;

    /* Allocate a new cluster for the directory */
    uint32_t new_cluster = fat32_find_free_cluster(sbi);
    if (new_cluster == 0) return -ENOSPC;

    /* Zero the new cluster */
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) {
        fat32_free_cluster_chain(sbi, new_cluster);
        return -ENOMEM;
    }
    memset(cluster_buf, 0, cluster_size);

    /* Write "." entry (self-reference) */
    {
        struct fat32_dir_entry *dot =
            (struct fat32_dir_entry *)cluster_buf;
        memset(dot, 0, sizeof(*dot));
        dot->name[0] = '.';
        for (int i = 1; i < 11; i++) dot->name[i] = ' ';
        dot->attr = ATTR_DIRECTORY;
        dot->first_cluster_high = (uint16_t)(new_cluster >> 16);
        dot->first_cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    }

    /* Write ".." entry (parent reference) */
    {
        struct fat32_dir_entry *dotdot =
            (struct fat32_dir_entry *)(cluster_buf + sizeof(struct fat32_dir_entry));
        memset(dotdot, 0, sizeof(*dotdot));
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';
        for (int i = 2; i < 11; i++) dotdot->name[i] = ' ';
        dotdot->attr = ATTR_DIRECTORY;
        dotdot->first_cluster_high = (uint16_t)(parent_cluster >> 16);
        dotdot->first_cluster_low = (uint16_t)(parent_cluster & 0xFFFF);
    }

    if (write_cluster(sbi, new_cluster, cluster_buf) < 0) {
        kfree(cluster_buf);
        fat32_free_cluster_chain(sbi, new_cluster);
        return -EIO;
    }
    kfree(cluster_buf);

    /* Find a free slot in the parent directory */
    uint32_t target_cluster, target_offset;
    int ret = fat32_find_free_slot(sbi, parent_cluster,
                                   &target_cluster, &target_offset);
    if (ret < 0) {
        fat32_free_cluster_chain(sbi, new_cluster);
        return ret;
    }

    /* Write the LFN + 8.3 entry in the parent */
    ret = fat32_write_lfn(sbi, parent_cluster, target_offset, name,
                          ATTR_DIRECTORY, new_cluster, 0);
    if (ret < 0) {
        fat32_free_cluster_chain(sbi, new_cluster);
        return ret;
    }

    return 0;
}

/*
 * fat32_rmdir: Remove an empty subdirectory.
 *
 * 1. Looks up the directory entry in the parent.
 * 2. Verifies the directory is empty (only "." and "..").
 * 3. Marks the directory entry as deleted (0xE5).
 * 4. Frees the cluster chain.
 */
int fat32_rmdir(struct fat32_sb_info *sbi, uint32_t parent_cluster,
                const char *name) {
    uint32_t cluster_size = sbi->cluster_size;

    /* Find the directory entry */
    struct fat32_dir_entry found_entry;
    uint32_t found_cluster = 0;
    uint32_t found_size = 0;
    uint8_t  found_attr = 0;

    int ret = fat32_find_entry(sbi, parent_cluster, 0, name,
                               &found_entry, &found_cluster,
                               &found_size, &found_attr, NULL,
                               NULL, NULL);
    if (ret < 0) return ret;

    /* Must be a directory */
    if (!(found_attr & ATTR_DIRECTORY)) return -ENOTDIR;

    /* Verify the directory is empty (only "." and "..").
     * Walk the entire FAT cluster chain to check all clusters. */
    uint8_t *dir_buf = (uint8_t *)kmalloc(cluster_size);
    if (!dir_buf) return -ENOMEM;

    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat32_dir_entry);
    int is_empty = 1;
    uint32_t cluster = found_cluster;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
        if (read_cluster(sbi, cluster, dir_buf) < 0) {
            kfree(dir_buf);
            return -EIO;
        }

        /* Start from entry 0 for the first cluster (contains "." and ".."),
         * start from entry 0 for subsequent clusters (no "." or ".." there). */
        uint32_t start_entry = (cluster == found_cluster) ? 2 : 0;

        for (uint32_t i = start_entry; i < entries_per_cluster; i++) {
            uint8_t *raw = dir_buf + i * sizeof(struct fat32_dir_entry);
            if (raw[0] != 0x00 && raw[0] != 0xE5) {
                uint8_t a = raw[11];
                /* Skip LFN entries */
                if (a == ATTR_LFN) continue;
                is_empty = 0;
                break;
            }
            if (raw[0] == 0x00) break;
        }

        if (!is_empty) break;

        /* Move to next cluster in the FAT chain */
        uint32_t next = fat32_get_cluster(sbi, cluster);
        if (next >= FAT32_CLUSTER_EOC_MIN) break;
        cluster = next;
    }
    kfree(dir_buf);

    if (!is_empty) return -ENOTEMPTY;

    /* Now locate the directory entry in the parent and mark it deleted.
     * We need to walk the parent directory to find the exact entry position. */
    {
        uint32_t cluster = parent_cluster;

        while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
            uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
            if (!cluster_buf) return -ENOMEM;

            if (read_cluster(sbi, cluster, cluster_buf) < 0) {
                kfree(cluster_buf);
                return -EIO;
            }

            for (uint32_t i = 0; i < entries_per_cluster; i++) {
                uint8_t *raw = cluster_buf + i * sizeof(struct fat32_dir_entry);

                if (raw[0] == 0x00) {
                    kfree(cluster_buf);
                    return -ENOENT;
                }

                if (raw[0] == 0xE5 || raw[11] == ATTR_LFN) continue;

                struct fat32_dir_entry *de = (struct fat32_dir_entry *)raw;
                uint32_t fc = ((uint32_t)de->first_cluster_high << 16) |
                              de->first_cluster_low;

                if (fc == found_cluster && (de->attr & ATTR_DIRECTORY)) {
                    /* Mark as deleted */
                    /* NOTE: Known TOCTOU limitation — the LFN entries
                     * preceding this entry are not cleared. This is safe
                     * for single-threaded operations but may leave stale
                     * LFN entries visible in multi-threaded scenarios. */
                    raw[0] = 0xE5;
                    if (write_cluster(sbi, cluster, cluster_buf) < 0) {
                        kfree(cluster_buf);
                        return -EIO;
                    }
                    kfree(cluster_buf);

                    /* Also clear any preceding LFN entries */
                    /* Walk backwards within the same cluster to clear LFNs */
                    {
                        int lfn_cleared = 0;
                        uint8_t *lfn_buf = (uint8_t *)kmalloc(cluster_size);
                        if (lfn_buf) {
                            if (read_cluster(sbi, cluster, lfn_buf) >= 0) {
                                for (int j = (int)i - 1; j >= 0 && lfn_cleared < 20; j--) {
                                    uint8_t *lr = lfn_buf + j * sizeof(struct fat32_dir_entry);
                                    if (lr[11] == ATTR_LFN) {
                                        lr[0] = 0xE5;
                                        lfn_cleared++;
                                    } else {
                                        break;
                                    }
                                }
                                if (lfn_cleared > 0) {
                                    write_cluster(sbi, cluster, lfn_buf);
                                }
                            }
                            kfree(lfn_buf);
                        }
                    }

                    /* Free the directory's cluster chain */
                    fat32_free_cluster_chain(sbi, found_cluster);
                    return 0;
                }
            }

            kfree(cluster_buf);

            uint32_t next = fat32_get_cluster(sbi, cluster);
            if (next >= FAT32_CLUSTER_EOC_MIN) break;
            cluster = next;
        }
    }

    return -ENOENT;
}

/* ================================================================
 * Public cluster allocation API wrappers
 * ================================================================ */

/*
 * fat32_alloc_cluster: Allocate a free cluster and mark it as end-of-chain.
 */
uint32_t fat32_alloc_cluster(struct fat32_sb_info *sbi) {
    return fat32_find_free_cluster(sbi);
}

/* ================================================================
 * Internal: create a VFS inode from FAT32 entry data
 * ================================================================ */

static struct inode *fat32_make_inode(struct fat32_sb_info *sbi,
                                      const char *name,
                                      uint32_t first_cluster,
                                      uint32_t file_size,
                                      uint8_t attr, int is_dir,
                                      struct dentry *dentry) {
    struct fat32_inode_info *info = (struct fat32_inode_info *)
        kmalloc(sizeof(*info));
    if (!info) return NULL;
    memset(info, 0, sizeof(*info));
    info->sbi = sbi;
    info->first_cluster = first_cluster;
    info->file_size = file_size;
    info->attributes = attr;
    info->is_dir = (uint8_t)is_dir;

    struct inode *inode = (struct inode *)kmalloc(sizeof(*inode));
    if (!inode) {
        kfree(info);
        return NULL;
    }
    memset(inode, 0, sizeof(*inode));

    /* Copy the name */
    char *name_copy = NULL;
    if (name) {
        size_t name_len = strlen(name);
        name_copy = (char *)kmalloc(name_len + 1);
        if (!name_copy) {
            kfree(inode);
            kfree(info);
            return NULL;
        }
        memcpy(name_copy, name, name_len);
        name_copy[name_len] = '\0';
    }

    inode->name   = name_copy;
    inode->priv   = info;
    inode->dentry = dentry;
    inode->size   = (size_t)file_size;

    if (is_dir) {
        inode->ops    = &fat32_dir_ops;
        inode->is_dir = 1;
    } else {
        inode->ops    = &fat32_file_ops;
        inode->is_dir = 0;
    }

    return inode;
}

/* ================================================================
 * VFS file operations for FAT32 files
 * ================================================================ */

static int fat32_file_open(struct inode *inode, struct file *filp) {
    (void)filp;
    if (!inode || !inode->priv) return -EINVAL;
    return 0;
}

static ssize_t fat32_file_read(struct file *filp, void *buf, size_t count,
                               off_t *offset) {
    if (!filp || !filp->inode || !filp->inode->priv) return -EINVAL;
    if (!buf || !offset) return -EINVAL;

    struct fat32_inode_info *info = (struct fat32_inode_info *)filp->inode->priv;
    struct fat32_sb_info *sbi = info->sbi;

    ssize_t ret = fat32_transfer_file(sbi, info->first_cluster, info->file_size,
                                      (uint8_t *)buf, count, (uint64_t)(*offset),
                                      0, NULL);
    if (ret > 0) *offset += (off_t)ret;
    return ret;
}

static ssize_t fat32_file_write(struct file *filp, const void *buf, size_t count,
                                off_t *offset) {
    if (!filp || !filp->inode || !filp->inode->priv) return -EINVAL;
    if (!buf || !offset) return -EINVAL;

    struct fat32_inode_info *info = (struct fat32_inode_info *)filp->inode->priv;
    struct fat32_sb_info *sbi = info->sbi;

    uint32_t new_size = info->file_size;
    ssize_t ret = fat32_transfer_file(sbi, info->first_cluster, info->file_size,
                                      (uint8_t *)buf, count, (uint64_t)(*offset),
                                      1, &new_size);
    if (ret > 0) {
        *offset += (off_t)ret;
        info->file_size = new_size;
        filp->inode->size = (size_t)new_size;

        /* Bug #20: Update the directory entry with the new file size.
         * We use the parent_cluster and dir_entry_offset tracked in
         * fat32_inode_info to locate the directory entry on disk. */
        if (info->parent_cluster >= 2 && info->parent_cluster < FAT32_CLUSTER_EOC_MIN) {
            uint32_t cluster_size = sbi->cluster_size;
            uint8_t *dir_buf = (uint8_t *)kmalloc(cluster_size);
            if (dir_buf) {
                if (read_cluster(sbi, info->parent_cluster, dir_buf) >= 0) {
                    struct fat32_dir_entry *de =
                        (struct fat32_dir_entry *)(dir_buf + info->dir_entry_offset);
                    de->file_size = new_size;
                    write_cluster(sbi, info->parent_cluster, dir_buf);
                }
                kfree(dir_buf);
            }
        }
    }
    return ret;
}

static int fat32_file_close(struct inode *inode, struct file *filp) {
    (void)inode;
    (void)filp;
    return 0;
}

/* ================================================================
 * VFS directory lookup callback
 * ================================================================ */

static int fat32_dir_lookup(struct inode *dir, struct dentry *dentry) {
    if (!dir || !dir->priv || !dentry) return -EINVAL;

    struct fat32_inode_info *info = (struct fat32_inode_info *)dir->priv;
    if (!info->is_dir) return -ENOTDIR;

    struct fat32_sb_info *sbi = info->sbi;
    const char *lookup_name = dentry->name;

    uint32_t found_cluster = 0;
    uint32_t found_size = 0;
    uint8_t  found_attr = 0;
    char *resolved_name = NULL;
    uint32_t dir_entry_cluster = 0;
    uint32_t dir_entry_offset = 0;

    int ret = fat32_find_entry(sbi, info->first_cluster, info->file_size,
                               lookup_name, NULL, &found_cluster,
                               &found_size, &found_attr, &resolved_name,
                               &dir_entry_cluster, &dir_entry_offset);
    if (ret < 0) return ret;

    int is_dir = (found_attr & ATTR_DIRECTORY) ? 1 : 0;

    /* Use the resolved name (LFN or 8.3) for the inode */
    const char *inode_name = resolved_name ? resolved_name : lookup_name;

    struct inode *child_inode = fat32_make_inode(sbi, inode_name,
                                                 found_cluster, found_size,
                                                 found_attr, is_dir, dentry);
    if (resolved_name) kfree(resolved_name);

    if (!child_inode) return -ENOMEM;

    /* Bug #20: Track parent directory entry position for file size updates */
    {
        struct fat32_inode_info *child_info = (struct fat32_inode_info *)child_inode->priv;
        if (child_info) {
            child_info->parent_cluster = dir_entry_cluster;
            child_info->dir_entry_offset = dir_entry_offset;
        }
    }

    /* For directories, compute the actual directory size */
    if (is_dir) {
        child_inode->size = fat32_get_dir_size(sbi, found_cluster);
        ((struct fat32_inode_info *)child_inode->priv)->file_size =
            (uint32_t)child_inode->size;
    }

    dentry->inode = child_inode;
    return 0;
}

/* ================================================================
 * VFS directory create / mkdir / rmdir callbacks
 * ================================================================ */

static int fat32_dir_create(struct inode *dir, const char *name, int flags) {
    if (!dir || !dir->priv || !name) return -EINVAL;

    struct fat32_inode_info *info = (struct fat32_inode_info *)dir->priv;
    if (!info->is_dir) return -ENOTDIR;

    struct fat32_sb_info *sbi = info->sbi;

    /* Check if file already exists */
    uint32_t dummy_cluster = 0;
    int ret = fat32_find_entry(sbi, info->first_cluster, info->file_size,
                               name, NULL, &dummy_cluster, NULL, NULL, NULL,
                               NULL, NULL);
    if (ret == 0) return -EEXIST;

    /* Allocate a new cluster for the file */
    uint32_t new_cluster = fat32_find_free_cluster(sbi);
    if (new_cluster == 0) return -ENOSPC;

    /* Find a free slot in the directory */
    uint32_t target_cluster, target_offset;
    ret = fat32_find_free_slot(sbi, info->first_cluster,
                               &target_cluster, &target_offset);
    if (ret < 0) {
        fat32_free_cluster_chain(sbi, new_cluster);
        return ret;
    }

    /* Write the LFN + 8.3 entry */
    uint8_t entry_attr = ATTR_ARCHIVE;
    ret = fat32_write_lfn(sbi, target_cluster, target_offset, name,
                          entry_attr, new_cluster, 0);
    if (ret < 0) {
        fat32_free_cluster_chain(sbi, new_cluster);
        return ret;
    }

    (void)flags;
    return 0;
}

static int fat32_dir_mkdir(struct inode *dir, const char *name) {
    if (!dir || !dir->priv || !name) return -EINVAL;

    struct fat32_inode_info *info = (struct fat32_inode_info *)dir->priv;
    if (!info->is_dir) return -ENOTDIR;

    struct fat32_sb_info *sbi = info->sbi;
    return fat32_mkdir(sbi, info->first_cluster, name);
}

static int fat32_dir_rmdir(struct inode *dir, const char *name) {
    if (!dir || !dir->priv || !name) return -EINVAL;

    struct fat32_inode_info *info = (struct fat32_inode_info *)dir->priv;
    if (!info->is_dir) return -ENOTDIR;

    struct fat32_sb_info *sbi = info->sbi;
    return fat32_rmdir(sbi, info->first_cluster, name);
}

static int fat32_dir_unlink(struct inode *dir, const char *name) {
    if (!dir || !dir->priv || !name) return -EINVAL;

    struct fat32_inode_info *info = (struct fat32_inode_info *)dir->priv;
    if (!info->is_dir) return -ENOTDIR;

    struct fat32_sb_info *sbi = info->sbi;

    /* Find the entry */
    struct fat32_dir_entry found_entry;
    uint32_t found_cluster = 0;
    uint8_t  found_attr = 0;

    int ret = fat32_find_entry(sbi, info->first_cluster, info->file_size, name,
                               &found_entry, &found_cluster,
                               NULL, &found_attr, NULL,
                               NULL, NULL);
    if (ret < 0) return ret;

    /* Cannot unlink a directory */
    if (found_attr & ATTR_DIRECTORY) return -EISDIR;

    /* NOTE: Known TOCTOU limitation — the LFN entries preceding the
     * target entry are not cleared. This is safe for single-threaded
     * operations but may leave stale LFN entries in multi-threaded
     * scenarios. */

    /* Walk the parent directory to find and mark the entry deleted */
    uint32_t cluster_size = sbi->cluster_size;
    uint32_t entries_per_cluster = cluster_size / sizeof(struct fat32_dir_entry);
    uint32_t cluster = info->first_cluster;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
        uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf) return -ENOMEM;

        if (read_cluster(sbi, cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -EIO;
        }

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t *raw = cluster_buf + i * sizeof(struct fat32_dir_entry);

            if (raw[0] == 0x00) {
                kfree(cluster_buf);
                return -ENOENT;
            }

            if (raw[0] == 0xE5 || raw[11] == ATTR_LFN) continue;

            struct fat32_dir_entry *de = (struct fat32_dir_entry *)raw;
            uint32_t fc = ((uint32_t)de->first_cluster_high << 16) |
                          de->first_cluster_low;

            if (fc == found_cluster) {
                /* Mark as deleted */
                raw[0] = 0xE5;
                if (write_cluster(sbi, cluster, cluster_buf) < 0) {
                    kfree(cluster_buf);
                    return -EIO;
                }
                kfree(cluster_buf);

                /* Clear preceding LFN entries */
                {
                    int lfn_cleared = 0;
                    uint8_t *lfn_buf = (uint8_t *)kmalloc(cluster_size);
                    if (lfn_buf) {
                        if (read_cluster(sbi, cluster, lfn_buf) >= 0) {
                            for (int j = (int)i - 1; j >= 0 && lfn_cleared < 20; j--) {
                                uint8_t *lr = lfn_buf + j * sizeof(struct fat32_dir_entry);
                                if (lr[11] == ATTR_LFN) {
                                    lr[0] = 0xE5;
                                    lfn_cleared++;
                                } else {
                                    break;
                                }
                            }
                            if (lfn_cleared > 0) {
                                write_cluster(sbi, cluster, lfn_buf);
                            }
                        }
                        kfree(lfn_buf);
                    }
                }

                /* Free the cluster chain */
                fat32_free_cluster_chain(sbi, found_cluster);
                return 0;
            }
        }

        kfree(cluster_buf);

        uint32_t next = fat32_get_cluster(sbi, cluster);
        if (next >= FAT32_CLUSTER_EOC_MIN) break;
        cluster = next;
    }

    return -ENOENT;
}

/* ================================================================
 * VFS file operations tables
 * ================================================================ */

static struct file_ops fat32_file_ops = {
    .open   = fat32_file_open,
    .read   = fat32_file_read,
    .write  = fat32_file_write,
    .close  = fat32_file_close,
    .lookup = NULL,
    .create = NULL,
    .mkdir  = NULL,
    .rmdir  = NULL,
    .unlink = NULL,
};

static struct file_ops fat32_dir_ops = {
    .open   = fat32_file_open,
    .read   = NULL,
    .write  = NULL,
    .close  = fat32_file_close,
    .lookup = fat32_dir_lookup,
    .create = fat32_dir_create,
    .mkdir  = fat32_dir_mkdir,
    .rmdir  = fat32_dir_rmdir,
    .unlink = fat32_dir_unlink,
};

/* ================================================================
 * fat32_mount: Mount a FAT32 filesystem
 * ================================================================ */

struct super_block *fat32_mount(struct block_device *bdev) {
    if (!bdev) return NULL;

    log_printf(LOG_LEVEL_INFO, "fat32: mounting from '%s'\n", bdev->name);

    /* Allocate filesystem-private data */
    struct fat32_sb_info *sbi = (struct fat32_sb_info *)kmalloc(sizeof(*sbi));
    if (!sbi) return NULL;
    memset(sbi, 0, sizeof(*sbi));
    sbi->bdev = bdev;

    /* Read the boot sector (sector 0) */
    uint8_t *boot_buf = (uint8_t *)kmalloc(512);
    if (!boot_buf) {
        kfree(sbi);
        return NULL;
    }

    if (read_sectors(bdev, 0, 1, boot_buf) < 0) {
        log_printf(LOG_LEVEL_ERR, "fat32: failed to read boot sector\n");
        kfree(boot_buf);
        kfree(sbi);
        return NULL;
    }

    struct fat32_boot_sector *bs = (struct fat32_boot_sector *)boot_buf;

    /* Verify boot signature */
    if (bs->boot_signature_2 != 0xAA55) {
        log_printf(LOG_LEVEL_ERR, "fat32: invalid boot signature 0x%04x\n",
                   bs->boot_signature_2);
        kfree(boot_buf);
        kfree(sbi);
        return NULL;
    }

    /* Parse BPB */
    sbi->bytes_per_sector    = bs->bpb.bytes_per_sector;
    sbi->sectors_per_cluster = bs->bpb.sectors_per_cluster;
    sbi->reserved_sectors    = bs->bpb.reserved_sectors;
    sbi->num_fats            = bs->bpb.num_fats;
    sbi->sectors_per_fat     = bs->ebr.sectors_per_fat;
    sbi->root_cluster        = bs->ebr.root_cluster;

    /* Validate basic geometry */
    if (sbi->bytes_per_sector < 512 || sbi->bytes_per_sector > 4096) {
        log_printf(LOG_LEVEL_ERR, "fat32: unsupported bytes_per_sector %u\n",
                   sbi->bytes_per_sector);
        kfree(boot_buf);
        kfree(sbi);
        return NULL;
    }

    if (sbi->sectors_per_cluster == 0) {
        log_printf(LOG_LEVEL_ERR, "fat32: invalid sectors_per_cluster\n");
        kfree(boot_buf);
        kfree(sbi);
        return NULL;
    }

    /* Verify it's FAT32: sectors_per_fat must be non-zero */
    if (sbi->sectors_per_fat == 0) {
        log_printf(LOG_LEVEL_ERR, "fat32: sectors_per_fat is 0 (not FAT32?)\n");
        kfree(boot_buf);
        kfree(sbi);
        return NULL;
    }

    /* Calculate layout */
    sbi->fat_start = sbi->reserved_sectors;
    sbi->data_start = sbi->fat_start + sbi->num_fats * sbi->sectors_per_fat;
    sbi->cluster_size = (uint32_t)sbi->bytes_per_sector *
                        sbi->sectors_per_cluster;

    /* Calculate total clusters */
    uint32_t total_sectors = bs->bpb.total_sectors_16 != 0 ?
                             bs->bpb.total_sectors_16 : bs->bpb.total_sectors_32;
    {
        /* Guard against underflow: data_start must be less than total_sectors */
        if (sbi->data_start >= total_sectors) {
            log_printf(LOG_LEVEL_ERR, "fat32: data_start (%u) >= total_sectors (%u)\n",
                       sbi->data_start, total_sectors);
            kfree(boot_buf);
            kfree(sbi);
            return NULL;
        }
        uint32_t data_sectors = total_sectors - sbi->data_start;
        sbi->total_clusters = data_sectors / sbi->sectors_per_cluster;
    }

    kfree(boot_buf);

    log_printf(LOG_LEVEL_INFO,
               "fat32: BPB parsed (bps=%u, spc=%u, reserved=%u, fats=%u, "
               "spf=%u, root_cluster=%u, total_clusters=%u)\n",
               sbi->bytes_per_sector, sbi->sectors_per_cluster,
               sbi->reserved_sectors, sbi->num_fats, sbi->sectors_per_fat,
               sbi->root_cluster, sbi->total_clusters);

    /* Optionally cache the FAT into memory */
    {
        uint32_t fat_bytes = sbi->sectors_per_fat * sbi->bytes_per_sector;
        sbi->fat_cache = (uint32_t *)kmalloc(fat_bytes);
        if (sbi->fat_cache) {
            uint8_t *fat_buf = (uint8_t *)sbi->fat_cache;
            uint32_t sectors_per_read = sbi->bytes_per_sector;
            for (uint32_t i = 0; i < sbi->sectors_per_fat; i++) {
                if (read_sectors(bdev, sbi->fat_start + i, 1,
                                 fat_buf + i * sectors_per_read) < 0) {
                    log_printf(LOG_LEVEL_WARN,
                               "fat32: failed to cache FAT at sector %u\n",
                               (uint32_t)(sbi->fat_start + i));
                    kfree(sbi->fat_cache);
                    sbi->fat_cache = NULL;
                    break;
                }
            }
            if (sbi->fat_cache)
                log_printf(LOG_LEVEL_INFO, "fat32: FAT cached (%u bytes)\n",
                           fat_bytes);
        }
    }

    /* Create the root inode */
    {
        struct inode *root_inode = fat32_make_inode(sbi, "/", sbi->root_cluster,
                                                     0, ATTR_DIRECTORY, 1, NULL);
        if (!root_inode) {
            log_printf(LOG_LEVEL_ERR, "fat32: failed to create root inode\n");
            if (sbi->fat_cache) kfree(sbi->fat_cache);
            kfree(sbi);
            return NULL;
        }

        /* Compute root directory size */
        uint64_t root_size = fat32_get_dir_size(sbi, sbi->root_cluster);
        root_inode->size = (size_t)root_size;
        ((struct fat32_inode_info *)root_inode->priv)->file_size = root_size;

        /* Create the super_block */
        struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
        if (!sb) {
            kfree(root_inode->priv);
            kfree(root_inode);
            if (sbi->fat_cache) kfree(sbi->fat_cache);
            kfree(sbi);
            return NULL;
        }
        memset(sb, 0, sizeof(*sb));
        sb->fs_name = "fat32";
        sb->root    = root_inode;
        sb->sb_data = sbi;

        log_printf(LOG_LEVEL_INFO, "fat32: mounted successfully\n");
        return sb;
    }
}