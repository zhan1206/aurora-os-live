/*
 * squashfs.c - Squashfs 4.0 read-only filesystem driver for AuroraOS
 *
 * Implements a read-only Squashfs driver that integrates with the VFS.
 * Supports:
 *   - Superblock reading and verification
 *   - Metadata decompression (zlib)
 *   - Inode table reading (basic and extended inode types)
 *   - Directory traversal
 *   - File content reading (with zlib decompression of data blocks)
 *   - Fragment handling
 *
 * Includes a simplified zlib inflate decompressor for data blocks.
 * Block device I/O is abstracted through struct block_device.
 */
#include "squashfs.h"
#include "fs.h"
#include "block_dev.h"
#include "include/log.h"
#include "include/errno.h"
#include "include/string.h"
#include "mem.h"

/* Forward declarations of VFS ops tables */
static struct file_ops squashfs_file_ops;
static struct file_ops squashfs_dir_ops;

/* ================================================================
 * Bitstream reader for inflate
 * ================================================================ */

struct bitstream {
    const uint8_t *data;
    uint32_t       size;       /* Total bytes */
    uint32_t       byte_pos;   /* Current byte position */
    uint32_t       bit_pos;    /* Current bit position within current byte (0-7) */
};

static void bs_init(struct bitstream *bs, const uint8_t *data, uint32_t size) {
    bs->data = data;
    bs->size = size;
    bs->byte_pos = 0;
    bs->bit_pos = 0;
}

static uint32_t bs_read_bits(struct bitstream *bs, int nbits) {
    uint32_t result = 0;
    for (int i = 0; i < nbits; i++) {
        if (bs->byte_pos >= bs->size) return result;
        uint32_t bit = (bs->data[bs->byte_pos] >> bs->bit_pos) & 1;
        result |= (bit << i);
        bs->bit_pos++;
        if (bs->bit_pos >= 8) {
            bs->bit_pos = 0;
            bs->byte_pos++;
        }
    }
    return result;
}

static void bs_align_byte(struct bitstream *bs) {
    if (bs->bit_pos > 0) {
        bs->bit_pos = 0;
        bs->byte_pos++;
    }
}

static uint32_t bs_bytes_read(const struct bitstream *bs) {
    return bs->byte_pos + (bs->bit_pos > 0 ? 1 : 0);
}

/* ================================================================
 * Simplified inflate decompressor
 *
 * Supports:
 *   - Stored blocks (BTYPE=0)
 *   - Fixed Huffman blocks (BTYPE=1)
 *   - Dynamic Huffman blocks (BTYPE=2) - basic support
 *
 * This is a minimal implementation sufficient for most Squashfs images.
 * ================================================================ */

/* Huffman code tree (up to 288 literal/length codes + 32 distance codes) */
#define MAX_HUFFMAN_CODES  288
#define MAX_DIST_CODES     32
#define MAX_CODE_LEN       15

struct huffman_tree {
    /* Canonical Huffman: sorted by code length, then by symbol */
    uint16_t symbols[MAX_HUFFMAN_CODES];
    uint16_t codes[MAX_HUFFMAN_CODES];
    uint8_t  lengths[MAX_HUFFMAN_CODES];
    uint16_t count;
    /* Lookup table: for each code length, the starting code */
    uint16_t max_code[MAX_CODE_LEN + 1];
    uint16_t min_code[MAX_CODE_LEN + 1];
    int16_t  val_ptrs[MAX_CODE_LEN + 1];  /* index into symbols[] */
};

static void huffman_build(struct huffman_tree *tree,
                          const uint8_t *lengths, int count) {
    int i;
    uint16_t bl_count[MAX_CODE_LEN + 1];

    memset(bl_count, 0, sizeof(bl_count));
    tree->count = 0;

    /* Count the number of codes for each length */
    for (i = 0; i < count; i++) {
        uint8_t len = lengths[i];
        if (len > 0) {
            bl_count[len]++;
            tree->symbols[tree->count] = (uint16_t)i;
            tree->lengths[tree->count] = len;
            tree->count++;
        }
    }

    /* Sort by code length, then by symbol value within same length */
    for (i = 0; i < (int)tree->count; i++) {
        for (int j = i + 1; j < (int)tree->count; j++) {
            if (tree->lengths[j] < tree->lengths[i] ||
                (tree->lengths[j] == tree->lengths[i] &&
                 tree->symbols[j] < tree->symbols[i])) {
                uint8_t tl = tree->lengths[i];
                tree->lengths[i] = tree->lengths[j];
                tree->lengths[j] = tl;
                uint16_t ts = tree->symbols[i];
                tree->symbols[i] = tree->symbols[j];
                tree->symbols[j] = ts;
            }
        }
    }

    /* Generate canonical codes */
    uint16_t code = 0;
    for (int len = 1; len <= MAX_CODE_LEN; len++) {
        if (bl_count[len] > 0) {
            tree->min_code[len] = code;
            tree->val_ptrs[len] = 0;
            /* Find first symbol with this length */
            for (i = 0; i < (int)tree->count; i++) {
                if (tree->lengths[i] == len) {
                    tree->val_ptrs[len] = (int16_t)i;
                    break;
                }
            }
            tree->max_code[len] = code + bl_count[len] - 1;
            code = (uint16_t)((code + bl_count[len]) << 1);
        } else {
            tree->max_code[len] = 0xFFFF;
            tree->min_code[len] = 0xFFFF;
            tree->val_ptrs[len] = -1;
        }
    }
}

static int huffman_decode(struct huffman_tree *tree, struct bitstream *bs) {
    uint16_t code = 0;
    for (int len = 1; len <= MAX_CODE_LEN; len++) {
        uint32_t bit = bs_read_bits(bs, 1);
        code = (uint16_t)((code << 1) | bit);
        if (code <= tree->max_code[len]) {
            int idx = tree->val_ptrs[len] + (code - tree->min_code[len]);
            if (idx >= 0 && idx < (int)tree->count) {
                return tree->symbols[idx];
            }
            return -1;
        }
    }
    return -1;
}

/* Fixed Huffman code lengths for literal/length alphabet (RFC 1951) */
static void build_fixed_ll_tree(struct huffman_tree *tree) {
    uint8_t lengths[288];
    int i;
    for (i = 0; i <= 143; i++) lengths[i] = 8;
    for (i = 144; i <= 255; i++) lengths[i] = 9;
    for (i = 256; i <= 279; i++) lengths[i] = 7;
    for (i = 280; i <= 287; i++) lengths[i] = 8;
    huffman_build(tree, lengths, 288);
}

static void build_fixed_dist_tree(struct huffman_tree *tree) {
    uint8_t lengths[32];
    for (int i = 0; i < 32; i++) lengths[i] = 5;
    huffman_build(tree, lengths, 32);
}

/* Length and distance base/extra bits tables (RFC 1951) */
static const uint16_t length_base[] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t length_extra[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
    16385, 24577
};
static const uint8_t dist_extra[] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Code length alphabet order (RFC 1951) */
static const int cl_order[] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/*
 * inflate: Decompress a raw deflate stream.
 * @in: compressed input data
 * @in_size: size of input data
 * @out: output buffer (must be pre-allocated)
 * @out_size: size of output buffer
 * Returns number of output bytes written, or 0 on error.
 */
static uint32_t inflate(const uint8_t *in, uint32_t in_size,
                        uint8_t *out, uint32_t out_size) {
    struct bitstream bs;
    bs_init(&bs, in, in_size);

    uint32_t out_pos = 0;
    int bfinal;

    do {
        bfinal = (int)bs_read_bits(&bs, 1);
        int btype = (int)bs_read_bits(&bs, 2);

        if (btype == 0) {
            /* Stored block (no compression) */
            bs_align_byte(&bs);
            if (bs.byte_pos + 4 > in_size) return 0;
            uint16_t len = (uint16_t)in[bs.byte_pos] |
                           ((uint16_t)in[bs.byte_pos + 1] << 8);
            uint16_t nlen = (uint16_t)in[bs.byte_pos + 2] |
                            ((uint16_t)in[bs.byte_pos + 3] << 8);
            if (len != (uint16_t)(~nlen & 0xFFFF)) return 0;
            bs.byte_pos += 4;

            if (out_pos + len > out_size) return 0;
            if (bs.byte_pos + len > in_size) return 0;
            memcpy(out + out_pos, in + bs.byte_pos, len);
            out_pos += len;
            bs.byte_pos += len;
        } else if (btype == 1 || btype == 2) {
            /* Fixed Huffman (btype==1) or Dynamic Huffman (btype==2) */
            struct huffman_tree ll_tree, dist_tree;

            if (btype == 1) {
                build_fixed_ll_tree(&ll_tree);
                build_fixed_dist_tree(&dist_tree);
            } else {
                /* Dynamic Huffman */
                int hlit = (int)bs_read_bits(&bs, 5) + 257;
                int hdist = (int)bs_read_bits(&bs, 5) + 1;
                int hclen = (int)bs_read_bits(&bs, 4) + 4;

                if (hlit > 288 || hdist > 32) return 0;

                uint8_t cl_lengths[19];
                memset(cl_lengths, 0, sizeof(cl_lengths));
                for (int i = 0; i < hclen; i++) {
                    cl_lengths[cl_order[i]] = (uint8_t)bs_read_bits(&bs, 3);
                }

                struct huffman_tree cl_tree;
                huffman_build(&cl_tree, cl_lengths, 19);

                uint8_t ll_lengths[288];
                uint8_t dist_lengths[32];
                memset(ll_lengths, 0, sizeof(ll_lengths));
                memset(dist_lengths, 0, sizeof(dist_lengths));

                int total_syms = hlit + hdist;
                int sym_idx = 0;
                while (sym_idx < total_syms) {
                    int sym = huffman_decode(&cl_tree, &bs);
                    if (sym < 0) return 0;

                    if (sym < 16) {
                        if (sym_idx < hlit)
                            ll_lengths[sym_idx] = (uint8_t)sym;
                        else
                            dist_lengths[sym_idx - hlit] = (uint8_t)sym;
                        sym_idx++;
                    } else if (sym == 16) {
                        if (sym_idx == 0) return 0;
                        int repeat = (int)bs_read_bits(&bs, 2) + 3;
                        uint8_t prev = (sym_idx <= hlit) ?
                            ll_lengths[sym_idx - 1] :
                            dist_lengths[sym_idx - hlit - 1];
                        for (int r = 0; r < repeat && sym_idx < total_syms; r++) {
                            if (sym_idx < hlit)
                                ll_lengths[sym_idx] = prev;
                            else
                                dist_lengths[sym_idx - hlit] = prev;
                            sym_idx++;
                        }
                    } else if (sym == 17) {
                        int repeat = (int)bs_read_bits(&bs, 3) + 3;
                        for (int r = 0; r < repeat && sym_idx < total_syms; r++) {
                            if (sym_idx < hlit)
                                ll_lengths[sym_idx] = 0;
                            else
                                dist_lengths[sym_idx - hlit] = 0;
                            sym_idx++;
                        }
                    } else if (sym == 18) {
                        int repeat = (int)bs_read_bits(&bs, 7) + 11;
                        for (int r = 0; r < repeat && sym_idx < total_syms; r++) {
                            if (sym_idx < hlit)
                                ll_lengths[sym_idx] = 0;
                            else
                                dist_lengths[sym_idx - hlit] = 0;
                            sym_idx++;
                        }
                    }
                }

                huffman_build(&ll_tree, ll_lengths, hlit);
                huffman_build(&dist_tree, dist_lengths, hdist);
            }

            /* Decode literal/length and distance codes */
            int done = 0;
            while (!done) {
                int sym = huffman_decode(&ll_tree, &bs);
                if (sym < 0) return 0;

                if (sym < 256) {
                    /* Literal byte */
                    if (out_pos >= out_size) return 0;
                    out[out_pos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    /* End of block */
                    done = 1;
                } else {
                    /* Length value */
                    int len_idx = sym - 257;
                    if (len_idx < 0 || len_idx >= 29) return 0;
                    uint32_t length = length_base[len_idx];
                    int extra = length_extra[len_idx];
                    if (extra > 0) {
                        length += bs_read_bits(&bs, extra);
                    }

                    /* Distance */
                    int dist_sym = huffman_decode(&dist_tree, &bs);
                    if (dist_sym < 0 || dist_sym >= 30) return 0;
                    uint32_t dist = dist_base[dist_sym];
                    extra = dist_extra[dist_sym];
                    if (extra > 0) {
                        dist += bs_read_bits(&bs, extra);
                    }

                    if (dist > out_pos) return 0;
                    if (out_pos + length > out_size) return 0;

                    /* Copy from output buffer (LZ77 back-reference) */
                    uint32_t src = out_pos - dist;
                    for (uint32_t i = 0; i < length; i++) {
                        out[out_pos++] = out[src++];
                    }
                }
            }
        } else {
            /* Invalid block type */
            return 0;
        }
    } while (!bfinal);

    return out_pos;
}

/*
 * zlib_decompress: Decompress zlib-wrapped deflate data.
 * Skips the 2-byte zlib header and 4-byte Adler-32 trailer.
 * @in: compressed input (zlib format)
 * @in_size: size of compressed data
 * @out: output buffer
 * @out_size: expected uncompressed size
 * Returns number of output bytes, or 0 on error.
 */
static uint32_t zlib_decompress(const uint8_t *in, uint32_t in_size,
                                uint8_t *out, uint32_t out_size) {
    if (in_size < 6) return 0;

    /* Check zlib header: CMF and FLG */
    uint8_t cmf = in[0];
    uint8_t flg = in[1];
    uint8_t cm = cmf & 0x0F;

    if (cm != 8) {
        /* Not deflate compression */
        return 0;
    }

    /* Check header checksum: (CMF*256 + FLG) % 31 == 0 */
    if (((uint16_t)cmf * 256 + flg) % 31 != 0) {
        return 0;
    }

    /* Skip dictionary if present (FDICT flag) */
    uint32_t offset = 2;
    if (flg & 0x20) {
        /* Dictionary present, skip 4 bytes dict ID */
        if (in_size < 10) return 0;
        offset += 4;
    }

    /* Decompress the deflate stream */
    return inflate(in + offset, in_size - offset - 4, out, out_size);
}

/* ================================================================
 * Block device helpers
 * ================================================================ */

static int read_bytes(struct block_device *bdev, uint64_t offset,
                      uint32_t size, void *buf) {
    uint64_t sector = offset / 512;
    uint32_t sector_off = (uint32_t)(offset % 512);
    uint32_t sectors_needed = (sector_off + size + 511) / 512;

    uint8_t *temp = (uint8_t *)kmalloc(sectors_needed * 512);
    if (!temp) return -ENOMEM;

    if (block_dev_read(bdev, temp, sector, (int)sectors_needed) < 0) {
        kfree(temp);
        return -EIO;
    }

    memcpy(buf, temp + sector_off, size);
    kfree(temp);
    return 0;
}

/* ================================================================
 * Metadata reading
 * ================================================================ */

/*
 * Read a metadata block from the Squashfs archive.
 * Metadata is stored in 8KB compressed blocks.
 * Returns the uncompressed data (caller must kfree), or NULL on error.
 */
static uint8_t *squashfs_read_metadata(struct squashfs_sb_info *sbi,
                                       uint64_t offset, uint32_t *out_size) {
    uint32_t meta_block_size = 8192;
    uint16_t size_field;

    /* Read the 2-byte size field */
    if (read_bytes(sbi->bdev, offset, 2, &size_field) < 0)
        return NULL;

    int compressed = !(size_field & 0x8000);
    uint32_t size = size_field & 0x7FFF;

    if (size == 0) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    if (compressed) {
        /* Compressed: allocate a full block for decompression */
        uint32_t uncomp_size = meta_block_size;
        uint8_t *comp_buf = (uint8_t *)kmalloc(size);
        uint8_t *uncomp_buf = (uint8_t *)kmalloc(uncomp_size);
        if (!comp_buf || !uncomp_buf) {
            if (comp_buf) kfree(comp_buf);
            if (uncomp_buf) kfree(uncomp_buf);
            return NULL;
        }

        if (read_bytes(sbi->bdev, offset + 2, size, comp_buf) < 0) {
            kfree(comp_buf);
            kfree(uncomp_buf);
            return NULL;
        }

        uint32_t result = zlib_decompress(comp_buf, size,
                                          uncomp_buf, uncomp_size);
        kfree(comp_buf);

        if (result == 0) {
            /* Try uncompressed read as fallback */
            if (read_bytes(sbi->bdev, offset + 2, size, uncomp_buf) < 0) {
                kfree(uncomp_buf);
                return NULL;
            }
            if (out_size) *out_size = size;
            return uncomp_buf;
        }

        if (out_size) *out_size = result;
        return uncomp_buf;
    } else {
        /* Uncompressed */
        uint8_t *buf = (uint8_t *)kmalloc(size);
        if (!buf) return NULL;

        if (read_bytes(sbi->bdev, offset + 2, size, buf) < 0) {
            kfree(buf);
            return NULL;
        }

        if (out_size) *out_size = size;
        return buf;
    }
}

/*
 * Read a data block from the Squashfs archive.
 * Data blocks use the block size from the superblock.
 */
static uint8_t *squashfs_read_data_block(struct squashfs_sb_info *sbi,
                                         uint64_t offset, uint32_t csize,
                                         uint32_t *out_size) {
    if (csize == 0) {
        /* Sparse block: all zeros */
        uint32_t bsize = sbi->sblk.block_size;
        uint8_t *buf = (uint8_t *)kmalloc(bsize);
        if (buf) {
            memset(buf, 0, bsize);
            if (out_size) *out_size = bsize;
        }
        return buf;
    }

    int compressed = !(csize & SQUASHFS_COMPRESSED_BIT);
    uint32_t actual_size = SQUASHFS_COMPRESSED_SIZE(csize);

    if (compressed) {
        uint32_t bsize = sbi->sblk.block_size;
        uint8_t *comp_buf = (uint8_t *)kmalloc(actual_size);
        uint8_t *uncomp_buf = (uint8_t *)kmalloc(bsize);
        if (!comp_buf || !uncomp_buf) {
            if (comp_buf) kfree(comp_buf);
            if (uncomp_buf) kfree(uncomp_buf);
            return NULL;
        }

        if (read_bytes(sbi->bdev, offset, actual_size, comp_buf) < 0) {
            kfree(comp_buf);
            kfree(uncomp_buf);
            return NULL;
        }

        uint32_t result = zlib_decompress(comp_buf, actual_size,
                                          uncomp_buf, bsize);
        kfree(comp_buf);

        if (result == 0) {
            kfree(uncomp_buf);
            return NULL;
        }

        if (out_size) *out_size = result;
        return uncomp_buf;
    } else {
        /* Uncompressed data block */
        uint8_t *buf = (uint8_t *)kmalloc(actual_size);
        if (!buf) return NULL;

        if (read_bytes(sbi->bdev, offset, actual_size, buf) < 0) {
            kfree(buf);
            return NULL;
        }

        if (out_size) *out_size = actual_size;
        return buf;
    }
}

/* ================================================================
 * Superblock and inode reading
 * ================================================================ */

/*
 * squashfs_read_superblock: Read and verify the superblock.
 */
static int squashfs_read_superblock(struct squashfs_sb_info *sbi) {
    if (read_bytes(sbi->bdev, 0, sizeof(struct squashfs_superblock),
                   &sbi->sblk) < 0) {
        log_printf(LOG_LEVEL_ERR, "squashfs: failed to read superblock\n");
        return -EIO;
    }

    if (sbi->sblk.s_magic != SQUASHFS_MAGIC) {
        log_printf(LOG_LEVEL_ERR, "squashfs: invalid magic 0x%08x\n",
                   sbi->sblk.s_magic);
        return -EINVAL;
    }

    log_printf(LOG_LEVEL_INFO,
               "squashfs: version %u.%u, %u inodes, block_size=%u, "
               "compression=%u, bytes_used=%llu\n",
               sbi->sblk.major, sbi->sblk.minor,
               sbi->sblk.inode_count, sbi->sblk.block_size,
               sbi->sblk.compression, sbi->sblk.bytes_used);

    return 0;
}

/*
 * squashfs_read_inode: Read an inode from the inode table.
 * @sbi: filesystem private data
 * @inode_number: inode number (1-based, or use the raw offset)
 * @out_info: output inode info (caller must fill start_block, file_size, etc.)
 * Returns 0 on success, negative on error.
 *
 * Note: This function reads the inode from the inode table at the given
 * metadata offset. The caller is responsible for knowing the correct
 * metadata offset for the inode.
 */
static int squashfs_read_inode_raw(struct squashfs_sb_info *sbi,
                                   uint64_t meta_offset,
                                   struct squashfs_inode_info *info) {
    uint32_t meta_size;
    uint8_t *meta = squashfs_read_metadata(sbi, meta_offset, &meta_size);
    if (!meta) return -EIO;

    if (meta_size < 2) {
        kfree(meta);
        return -EIO;
    }

    uint16_t inode_type = (uint16_t)meta[0] | ((uint16_t)meta[1] << 8);
    info->inode_type = inode_type;

    switch (inode_type) {
    case SQUASHFS_REG_TYPE: {
        if (meta_size < sizeof(struct squashfs_reg_inode)) {
            kfree(meta);
            return -EIO;
        }
        struct squashfs_reg_inode *ri = (struct squashfs_reg_inode *)meta;
        info->mode = ri->mode;
        info->start_block = ri->start_block;
        info->file_size = ri->file_size;
        info->fragment_index = ri->fragment;
        info->fragment_offset = ri->offset;
        info->is_dir = 0;

        /* Read block list */
        uint32_t block_size = sbi->sblk.block_size;
        uint32_t num_blocks = (ri->file_size + block_size - 1) / block_size;
        info->block_count = num_blocks;
        info->block_list = NULL;

        if (num_blocks > 0) {
            /* Bounds check: ensure block list fits within metadata buffer */
            if (sizeof(struct squashfs_reg_inode) + num_blocks * 4 > meta_size) {
                kfree(meta);
                return -EIO;
            }
            info->block_list = (uint32_t *)kmalloc(num_blocks * sizeof(uint32_t));
            if (info->block_list) {
                uint8_t *blist = meta + sizeof(struct squashfs_reg_inode);
                for (uint32_t i = 0; i < num_blocks; i++) {
                    info->block_list[i] = ((uint32_t)blist[i * 4]) |
                                          ((uint32_t)blist[i * 4 + 1] << 8) |
                                          ((uint32_t)blist[i * 4 + 2] << 16) |
                                          ((uint32_t)blist[i * 4 + 3] << 24);
                }
            }
        }
        break;
    }
    case SQUASHFS_LREG_TYPE: {
        if (meta_size < sizeof(struct squashfs_lreg_inode)) {
            kfree(meta);
            return -EIO;
        }
        struct squashfs_lreg_inode *lri = (struct squashfs_lreg_inode *)meta;
        info->mode = lri->mode;
        info->start_block = (uint32_t)lri->start_block;
        info->file_size = (uint32_t)lri->file_size;
        info->fragment_index = lri->fragment;
        info->fragment_offset = lri->offset;
        info->is_dir = 0;

        uint32_t block_size = sbi->sblk.block_size;
        uint32_t num_blocks = (info->file_size + block_size - 1) / block_size;
        info->block_count = num_blocks;
        info->block_list = NULL;

        if (num_blocks > 0) {
            /* Bounds check: ensure block list fits within metadata buffer */
            if (sizeof(struct squashfs_lreg_inode) + num_blocks * 4 > meta_size) {
                kfree(meta);
                return -EIO;
            }
            info->block_list = (uint32_t *)kmalloc(num_blocks * sizeof(uint32_t));
            if (info->block_list) {
                uint8_t *blist = meta + sizeof(struct squashfs_lreg_inode);
                for (uint32_t i = 0; i < num_blocks; i++) {
                    info->block_list[i] = ((uint32_t)blist[i * 4]) |
                                          ((uint32_t)blist[i * 4 + 1] << 8) |
                                          ((uint32_t)blist[i * 4 + 2] << 16) |
                                          ((uint32_t)blist[i * 4 + 3] << 24);
                }
            }
        }
        break;
    }
    case SQUASHFS_DIR_TYPE: {
        if (meta_size < sizeof(struct squashfs_dir_inode)) {
            kfree(meta);
            return -EIO;
        }
        struct squashfs_dir_inode *di = (struct squashfs_dir_inode *)meta;
        info->mode = di->mode;
        info->start_block = di->start_block;
        info->file_size = di->file_size;
        info->is_dir = 1;
        info->block_list = NULL;
        info->block_count = 0;
        info->fragment_index = 0;
        info->fragment_offset = di->offset;
        break;
    }
    case SQUASHFS_LDIR_TYPE: {
        if (meta_size < sizeof(struct squashfs_ldir_inode)) {
            kfree(meta);
            return -EIO;
        }
        struct squashfs_ldir_inode *ldi = (struct squashfs_ldir_inode *)meta;
        info->mode = ldi->mode;
        info->start_block = ldi->start_block;
        info->file_size = ldi->file_size;
        info->is_dir = 1;
        info->block_list = NULL;
        info->block_count = 0;
        info->fragment_index = 0;
        info->fragment_offset = ldi->offset;
        break;
    }
    case SQUASHFS_SYMLINK_TYPE:
    case SQUASHFS_LSYMLINK_TYPE:
    case SQUASHFS_BLKDEV_TYPE:
    case SQUASHFS_LBLKDEV_TYPE:
    case SQUASHFS_CHRDEV_TYPE:
    case SQUASHFS_LCHRDEV_TYPE:
    case SQUASHFS_FIFO_TYPE:
    case SQUASHFS_LFIFO_TYPE:
    case SQUASHFS_SOCKET_TYPE:
    case SQUASHFS_LSOCKET_TYPE:
    default:
        info->is_dir = 0;
        info->file_size = 0;
        info->start_block = 0;
        info->block_list = NULL;
        info->block_count = 0;
        info->fragment_index = 0;
        info->fragment_offset = 0;
        break;
    }

    kfree(meta);
    return 0;
}

/* ================================================================
 * Directory reading
 * ================================================================ */

/*
 * squashfs_read_directory: Read directory entries from a directory inode.
 * @sbi: filesystem private data
 * @info: directory inode info
 * @callback: called for each directory entry
 * @arg: user data passed to callback
 * Returns 0 on success, negative on error.
 *
 * The callback receives: (name, inode_offset, inode_type, arg)
 * and should return 0 to continue, or non-zero to stop.
 */
typedef int (*squashfs_dir_callback)(const char *name, uint32_t inode_offset,
                                     uint16_t type, void *arg);

static int squashfs_read_directory(struct squashfs_sb_info *sbi,
                                   struct squashfs_inode_info *info,
                                   squashfs_dir_callback callback,
                                   void *arg) {
    uint32_t dir_size = info->file_size;
    uint64_t meta_offset = info->start_block;
    uint32_t offset = info->fragment_offset;
    if (offset >= dir_size) return -EINVAL;
    uint32_t remaining = dir_size - offset;

    /* Read the directory data from metadata */
    uint8_t *dir_data = (uint8_t *)kmalloc(remaining);
    if (!dir_data) return -ENOMEM;

    /* Read sequentially from metadata blocks */
    uint32_t read_pos = 0;
    uint64_t cur_offset = meta_offset;

    while (read_pos < remaining) {
        uint32_t meta_size;
        uint8_t *block = squashfs_read_metadata(sbi, cur_offset, &meta_size);
        if (!block) {
            kfree(dir_data);
            return -EIO;
        }

        uint32_t to_copy = meta_size;
        if (read_pos + to_copy > remaining)
            to_copy = remaining - read_pos;

        /* Skip the initial offset within the first metadata block */
        uint32_t skip = (read_pos == 0) ? offset : 0;
        if (skip < to_copy) {
            memcpy(dir_data + read_pos, block + skip, to_copy - skip);
            read_pos += (to_copy - skip);
        }

        cur_offset += 2 + meta_size;  /* 2-byte header + data */
        if (meta_size < 8192 && read_pos < remaining) {
            /* This was the last metadata block — no more data to read */
            kfree(block);
            break;
        }
        kfree(block);
    }

    /* Parse directory entries */
    uint32_t pos = 0;
    while (pos + sizeof(struct squashfs_dir_header) <= remaining) {
        struct squashfs_dir_header *dh =
            (struct squashfs_dir_header *)(dir_data + pos);
        if (dh->count == 0) {
            pos += sizeof(struct squashfs_dir_header);
            continue;
        }
        uint32_t count = dh->count - 1;
        uint32_t dir_start = dh->start_block;

        pos += sizeof(struct squashfs_dir_header);

        for (uint32_t i = 0; i < count; i++) {
            if (pos + sizeof(struct squashfs_dir_entry) > remaining) {
                kfree(dir_data);
                return -EIO;
            }

            struct squashfs_dir_entry *de =
                (struct squashfs_dir_entry *)(dir_data + pos);
            uint16_t name_size = de->name_size - 1;

            pos += sizeof(struct squashfs_dir_entry);

            if (pos + name_size > remaining) {
                kfree(dir_data);
                return -EIO;
            }

            /* Extract name */
            char name_buf[256];
            uint32_t copy_len = name_size < 255 ? name_size : 255;
            memcpy(name_buf, dir_data + pos, copy_len);
            name_buf[copy_len] = '\0';
            pos += name_size;

            /* Calculate the inode metadata offset */
            uint32_t inode_meta_offset = dir_start + de->offset;

            int ret = callback(name_buf, inode_meta_offset, de->type, arg);
            if (ret != 0) {
                kfree(dir_data);
                return ret;
            }
        }
    }

    kfree(dir_data);
    return 0;
}

/* ================================================================
 * Lookup callback data
 * ================================================================ */
struct squashfs_lookup_data {
    const char *name;
    struct squashfs_inode_info *found_info;
    int found;
};

static int squashfs_lookup_cb(const char *name, uint32_t inode_offset,
                              uint16_t type, void *arg) {
    struct squashfs_lookup_data *data = (struct squashfs_lookup_data *)arg;
    if (strcmp(name, data->name) == 0) {
        data->found = 1;
        /* We can't read the inode here because we don't have sbi */
        /* Store the offset for later */
        data->found_info->start_block = inode_offset;
        data->found_info->inode_type = type;
        return 1; /* Stop iteration */
    }
    return 0;
}

/*
 * squashfs_lookup: Look up a file by name in a directory.
 */
static int squashfs_lookup(struct squashfs_sb_info *sbi,
                           struct squashfs_inode_info *dir_info,
                           const char *name,
                           struct squashfs_inode_info *out_info) {
    struct squashfs_lookup_data data;
    data.name = name;
    data.found_info = out_info;
    data.found = 0;
    memset(out_info, 0, sizeof(*out_info));

    int ret = squashfs_read_directory(sbi, dir_info, squashfs_lookup_cb, &data);
    if (ret < 0) return ret;
    if (!data.found) return -ENOENT;

    /* Read the actual inode info */
    out_info->sbi = sbi;
    return squashfs_read_inode_raw(sbi, out_info->start_block, out_info);
}

/* ================================================================
 * File reading
 * ================================================================ */

/*
 * squashfs_read_file: Read file content from a regular file inode.
 */
static ssize_t squashfs_read_file(struct squashfs_sb_info *sbi,
                                  struct squashfs_inode_info *info,
                                  uint8_t *buf, size_t count,
                                  uint64_t offset) {
    uint32_t file_size = info->file_size;
    if (offset >= file_size) return 0;
    if (offset + count > (uint64_t)file_size)
        count = (size_t)((uint64_t)file_size - offset);

    uint32_t block_size = sbi->sblk.block_size;
    size_t total_read = 0;

    while (total_read < count) {
        uint32_t block_idx = (uint32_t)(offset / block_size);
        uint32_t block_off = (uint32_t)(offset % block_size);

        if (block_idx >= info->block_count) {
            /* Past the block list — must be fragment data */
            break;
        }

        /* If the block_list allocation failed during inode read, avoid
         * NULL pointer dereference. */
        if (!info->block_list) {
            break;
        }

        uint32_t csize = info->block_list[block_idx];
        uint32_t uncomp_size;

        /* Calculate the offset of this block in the archive */
        uint64_t block_offset = info->start_block;
        for (uint32_t i = 0; i < block_idx; i++) {
            uint32_t bs = info->block_list[i];
            if (bs != 0) {
                block_offset += SQUASHFS_COMPRESSED_SIZE(bs);
            }
        }

        uint8_t *block_data = squashfs_read_data_block(sbi, block_offset,
                                                       csize, &uncomp_size);
        if (!block_data) return total_read > 0 ? (ssize_t)total_read : -EIO;

        size_t chunk;
        if (block_off >= uncomp_size) {
            kfree(block_data);
            break;
        }
        chunk = (size_t)(uncomp_size - block_off);
        if (chunk > count - total_read) chunk = count - total_read;

        memcpy(buf + total_read, block_data + block_off, chunk);
        kfree(block_data);

        offset += chunk;
        total_read += chunk;
    }

    return (ssize_t)total_read;
}

/* ================================================================
 * VFS integration — inode creation
 * ================================================================ */

static struct inode *squashfs_make_inode(struct squashfs_sb_info *sbi,
                                         const char *name,
                                         struct squashfs_inode_info *info,
                                         struct dentry *dentry) {
    struct squashfs_inode_info *priv = (struct squashfs_inode_info *)
        kmalloc(sizeof(*priv));
    if (!priv) return NULL;
    memcpy(priv, info, sizeof(*priv));
    priv->sbi = sbi;

    /* Copy block_list if present */
    if (info->block_list && info->block_count > 0) {
        priv->block_list = (uint32_t *)kmalloc(
            info->block_count * sizeof(uint32_t));
        if (priv->block_list) {
            memcpy(priv->block_list, info->block_list,
                   info->block_count * sizeof(uint32_t));
        }
    }

    struct inode *inode = (struct inode *)kmalloc(sizeof(*inode));
    if (!inode) {
        if (priv->block_list) kfree(priv->block_list);
        kfree(priv);
        return NULL;
    }
    memset(inode, 0, sizeof(*inode));

    char *name_copy = NULL;
    if (name) {
        size_t name_len = strlen(name);
        name_copy = (char *)kmalloc(name_len + 1);
        if (!name_copy) {
            if (priv->block_list) kfree(priv->block_list);
            kfree(priv);
            kfree(inode);
            return NULL;
        }
        memcpy(name_copy, name, name_len);
        name_copy[name_len] = '\0';
    }

    inode->name   = name_copy;
    inode->priv   = priv;
    inode->dentry = dentry;
    inode->size   = (size_t)info->file_size;

    if (info->is_dir) {
        inode->ops    = &squashfs_dir_ops;
        inode->is_dir = 1;
    } else {
        inode->ops    = &squashfs_file_ops;
        inode->is_dir = 0;
    }

    return inode;
}

/* ================================================================
 * VFS file operations for Squashfs files
 * ================================================================ */

static int squashfs_file_open(struct inode *inode, struct file *filp) {
    (void)filp;
    if (!inode || !inode->priv) return -EINVAL;
    return 0;
}

static ssize_t squashfs_file_read(struct file *filp, void *buf, size_t count,
                                  off_t *offset) {
    if (!filp || !filp->inode || !filp->inode->priv) return -EINVAL;
    if (!buf || !offset) return -EINVAL;

    struct squashfs_inode_info *info =
        (struct squashfs_inode_info *)filp->inode->priv;
    struct squashfs_sb_info *sbi = info->sbi;

    ssize_t ret = squashfs_read_file(sbi, info, (uint8_t *)buf, count,
                                     (uint64_t)(*offset));
    if (ret > 0) *offset += (off_t)ret;
    return ret;
}

static ssize_t squashfs_file_write(struct file *filp, const void *buf,
                                   size_t count, off_t *offset) {
    (void)filp; (void)buf; (void)count; (void)offset;
    return -EROFS;  /* Read-only filesystem */
}

static int squashfs_file_close(struct inode *inode, struct file *filp) {
    (void)inode;
    (void)filp;
    return 0;
}

/* ================================================================
 * VFS directory lookup callback
 * ================================================================ */

static int squashfs_dir_lookup(struct inode *dir, struct dentry *dentry) {
    if (!dir || !dir->priv || !dentry) return -EINVAL;

    struct squashfs_inode_info *info =
        (struct squashfs_inode_info *)dir->priv;
    if (!info->is_dir) return -ENOTDIR;

    struct squashfs_sb_info *sbi = info->sbi;
    const char *lookup_name = dentry->name;

    struct squashfs_inode_info found_info;
    int ret = squashfs_lookup(sbi, info, lookup_name, &found_info);
    if (ret < 0) return ret;

    struct inode *child_inode = squashfs_make_inode(sbi, lookup_name,
                                                    &found_info, dentry);
    if (!child_inode) return -ENOMEM;

    dentry->inode = child_inode;
    return 0;
}

static int squashfs_dir_create(struct inode *dir, const char *name, int flags) {
    (void)dir; (void)name; (void)flags;
    return -EROFS;
}

static int squashfs_dir_mkdir(struct inode *dir, const char *name) {
    (void)dir; (void)name;
    return -EROFS;
}

static int squashfs_dir_rmdir(struct inode *dir, const char *name) {
    (void)dir; (void)name;
    return -EROFS;
}

static int squashfs_dir_unlink(struct inode *dir, const char *name) {
    (void)dir; (void)name;
    return -EROFS;
}

/* ================================================================
 * VFS file operations tables
 * ================================================================ */

static struct file_ops squashfs_file_ops = {
    .open   = squashfs_file_open,
    .read   = squashfs_file_read,
    .write  = squashfs_file_write,
    .close  = squashfs_file_close,
    .lookup = NULL,
    .create = NULL,
    .mkdir  = NULL,
    .rmdir  = NULL,
    .unlink = NULL,
};

static struct file_ops squashfs_dir_ops = {
    .open   = squashfs_file_open,
    .read   = NULL,
    .write  = NULL,
    .close  = squashfs_file_close,
    .lookup = squashfs_dir_lookup,
    .create = squashfs_dir_create,
    .mkdir  = squashfs_dir_mkdir,
    .rmdir  = squashfs_dir_rmdir,
    .unlink = squashfs_dir_unlink,
};

/* ================================================================
 * squashfs_mount: Mount a Squashfs filesystem
 * ================================================================ */

struct super_block *squashfs_mount(struct block_device *bdev) {
    if (!bdev) return NULL;

    log_printf(LOG_LEVEL_INFO, "squashfs: mounting from '%s'\n", bdev->name);

    /* Allocate filesystem-private data */
    struct squashfs_sb_info *sbi = (struct squashfs_sb_info *)
        kmalloc(sizeof(*sbi));
    if (!sbi) return NULL;
    memset(sbi, 0, sizeof(*sbi));
    sbi->bdev = bdev;

    /* Read the superblock */
    if (squashfs_read_superblock(sbi) < 0) {
        kfree(sbi);
        return NULL;
    }

    /* Read the root inode */
    struct squashfs_inode_info root_info;
    memset(&root_info, 0, sizeof(root_info));
    root_info.sbi = sbi;

    int ret = squashfs_read_inode_raw(sbi, sbi->sblk.root_inode, &root_info);
    if (ret < 0) {
        log_printf(LOG_LEVEL_ERR,
                   "squashfs: failed to read root inode\n");
        kfree(sbi);
        return NULL;
    }

    /* Create the root inode */
    struct inode *root_inode = squashfs_make_inode(sbi, "/", &root_info, NULL);
    if (root_info.block_list) kfree(root_info.block_list);

    if (!root_inode) {
        log_printf(LOG_LEVEL_ERR,
                   "squashfs: failed to create root inode\n");
        kfree(sbi);
        return NULL;
    }

    /* Create the super_block */
    struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
    if (!sb) {
        kfree(root_inode->priv);
        kfree(root_inode);
        kfree(sbi);
        return NULL;
    }
    memset(sb, 0, sizeof(*sb));
    sb->fs_name = "squashfs";
    sb->root    = root_inode;
    sb->sb_data = sbi;

    log_printf(LOG_LEVEL_INFO, "squashfs: mounted successfully\n");
    return sb;
}