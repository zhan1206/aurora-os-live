/*
 * journal.c - Write-Ahead Logging (WAL) journal implementation
 *
 * Provides transactional write-ahead logging for filesystem metadata.
 * All writes to journal-protected blocks go through the journal first.
 * On crash recovery, the journal is scanned and committed transactions
 * are replayed to restore filesystem consistency.
 *
 * Key design decisions:
 *   - Physical journaling: full block data is journaled (not just deltas)
 *   - Metadata-only: only filesystem metadata blocks are journaled
 *   - Circular buffer: journal wraps around when full
 *   - Checksums: every block has a simple XOR checksum for integrity
 *   - Atomic commits: commit record is the final write in a transaction
 *
 * Recovery process:
 *   1. Read journal superblock
 *   2. If flags == dirty, scan from tail to head
 *   3. For each transaction with a valid commit record, replay blocks
 *   4. Mark journal as clean
 */
#include "journal.h"
#include "block_dev.h"
#include "include/log.h"
#include "include/print.h"
#include "mem.h"
#include <string.h>

/* ================================================================
 * Static state (single journal instance for simplicity)
 * ================================================================ */
static struct journal_handle g_journal;
static int g_journal_initialized = 0;

/* ================================================================
 * Internal helpers
 * ================================================================ */

/* Calculate a simple XOR checksum over a buffer */
static uint32_t calc_checksum(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 1) | (sum >> 31);
        sum ^= p[i];
    }
    return sum;
}

/* Convert journal-relative offset to device-absolute block number */
static uint64_t journal_to_device(uint64_t offset) {
    return g_journal.journal_start + offset;
}

/* Write a single block to the journal area at the given offset */
static int journal_write_block(uint64_t offset, const void *data) {
    uint32_t sectors_per_block = g_journal.block_size / g_journal.bdev->block_size;
    uint64_t sector = journal_to_device(offset) * sectors_per_block;
    return block_dev_write(g_journal.bdev, data, sector, (int)sectors_per_block);
}

/* Read a single block from the journal area at the given offset */
static int journal_read_block(uint64_t offset, void *buf) {
    uint32_t sectors_per_block = g_journal.block_size / g_journal.bdev->block_size;
    uint64_t sector = journal_to_device(offset) * sectors_per_block;
    return block_dev_read(g_journal.bdev, buf, sector, (int)sectors_per_block);
}

/* Write the journal superblock to disk */
static int write_jsb(void) {
    uint8_t *io_buf = (uint8_t *)kmalloc(g_journal.block_size);
    if (!io_buf) return -1;
    memset(io_buf, 0, g_journal.block_size);

    struct journal_superblock *jsb = (struct journal_superblock *)io_buf;
    jsb->jsb_magic        = JOURNAL_MAGIC_SB;
    jsb->jsb_block_size   = g_journal.block_size;
    jsb->jsb_start_block  = g_journal.journal_start;
    jsb->jsb_num_blocks   = g_journal.journal_blocks;
    jsb->jsb_head         = g_journal.head;
    jsb->jsb_tail         = g_journal.tail;
    jsb->jsb_sequence     = g_journal.sequence;
    jsb->jsb_flags        = g_journal.dirty ? 1 : 0;
    jsb->jsb_checksum     = calc_checksum(jsb, sizeof(struct journal_superblock) - sizeof(uint32_t) * 8 - sizeof(uint32_t));

    int ret = journal_write_block(0, io_buf);
    kfree(io_buf);
    return ret;
}

/* Free resources allocated for the current transaction */
static void free_transaction(void) {
    if (g_journal.txn_data) {
        for (uint32_t i = 0; i < g_journal.txn_blocks; i++) {
            if (g_journal.txn_data[i]) kfree(g_journal.txn_data[i]);
        }
        kfree(g_journal.txn_data);
        g_journal.txn_data = NULL;
    }
    if (g_journal.txn_fs_blocks) {
        kfree(g_journal.txn_fs_blocks);
        g_journal.txn_fs_blocks = NULL;
    }
    g_journal.txn_blocks = 0;
    g_journal.txn_capacity = 0;
    g_journal.in_transaction = 0;
}

/*
 * Advance head pointer by n blocks, wrapping around if needed.
 * If the journal is full, advance tail (evict oldest transactions).
 */
static void advance_head(uint64_t n) {
    uint64_t usable = g_journal.journal_blocks - 1; /* reserve 1 block for safety */
    g_journal.head += n;
    /* If head would overtake tail, advance tail */
    while (g_journal.head >= g_journal.tail + usable) {
        /* Skip the oldest transaction */
        uint8_t *io_buf = (uint8_t *)kmalloc(g_journal.block_size);
        if (!io_buf) break;
        if (journal_read_block(g_journal.tail, io_buf) == 0 &&
            ((struct journal_block_header *)io_buf)->jbh_magic == JOURNAL_MAGIC_DESC) {
            struct journal_block_header *hdr = (struct journal_block_header *)io_buf;
            uint64_t txn_blocks = 1 + hdr->jbh_num_blocks + 1; /* desc + data + commit */
            g_journal.tail += txn_blocks;
        } else {
            g_journal.tail++;
        }
        kfree(io_buf);
    }
    if (g_journal.head >= g_journal.journal_blocks) {
        g_journal.head -= g_journal.journal_blocks;
        g_journal.tail -= g_journal.journal_blocks;
    }
    write_jsb();
}

/* ================================================================
 * Public API
 * ================================================================ */

int journal_init(struct block_device *bdev, uint64_t journal_start,
                 uint64_t journal_blocks, uint32_t block_size) {
    if (!bdev || journal_blocks < 8 || block_size < 512) return -1;

    memset(&g_journal, 0, sizeof(g_journal));
    g_journal.bdev           = bdev;
    g_journal.block_size     = block_size;
    g_journal.journal_start  = journal_start;
    g_journal.journal_blocks = journal_blocks;

    /* Allocate block-sized buffer for I/O (stack-allocated structs are too small) */
    uint8_t *io_buf = (uint8_t *)kmalloc(block_size);
    if (!io_buf) return -1;

    /* Try to read existing journal superblock */
    if (journal_read_block(0, io_buf) == 0 &&
        ((struct journal_superblock *)io_buf)->jsb_magic == JOURNAL_MAGIC_SB) {
        struct journal_superblock *jsb = (struct journal_superblock *)io_buf;
        /* Existing journal found */
        g_journal.head     = jsb->jsb_head;
        g_journal.tail     = jsb->jsb_tail;
        g_journal.sequence = jsb->jsb_sequence;
        g_journal.dirty    = (jsb->jsb_flags != 0) ? 1 : 0;

        log_printf(LOG_LEVEL_INFO, "journal: found existing journal (seq=%llu, head=%llu, tail=%llu, dirty=%d)\n",
                   (unsigned long long)jsb->jsb_sequence,
                   (unsigned long long)g_journal.head,
                   (unsigned long long)g_journal.tail,
                   g_journal.dirty);

        if (g_journal.dirty) {
            log_printf(LOG_LEVEL_WARN, "journal: dirty — attempting recovery\n");
            kfree(io_buf);
            if (journal_recover() < 0) {
                log_printf(LOG_LEVEL_ERR, "journal: recovery failed\n");
                return -1;
            }
            log_printf(LOG_LEVEL_INFO, "journal: recovery complete\n");
        } else {
            kfree(io_buf);
        }
    } else {
        kfree(io_buf);
        /* Create new journal */
        g_journal.head     = 1; /* block 0 is the superblock */
        g_journal.tail     = 1;
        g_journal.sequence = 1;
        g_journal.dirty    = 0;

        if (write_jsb() < 0) {
            log_printf(LOG_LEVEL_ERR, "journal: failed to write superblock\n");
            return -1;
        }
        log_printf(LOG_LEVEL_INFO, "journal: created new journal\n");
    }

    g_journal_initialized = 1;
    return 0;
}

int journal_begin(uint32_t max_blocks) {
    if (!g_journal_initialized) return -1;
    if (g_journal.in_transaction) return -1;

    if (max_blocks == 0) max_blocks = 1;
    if (max_blocks > 64) max_blocks = 64; /* practical limit */

    g_journal.txn_fs_blocks = (uint32_t *)kmalloc(max_blocks * sizeof(uint32_t));
    g_journal.txn_data = (uint8_t **)kmalloc(max_blocks * sizeof(uint8_t *));
    if (!g_journal.txn_fs_blocks || !g_journal.txn_data) {
        if (g_journal.txn_fs_blocks) kfree(g_journal.txn_fs_blocks);
        if (g_journal.txn_data) kfree(g_journal.txn_data);
        g_journal.txn_fs_blocks = NULL;
        g_journal.txn_data = NULL;
        return -1;
    }

    memset(g_journal.txn_fs_blocks, 0, max_blocks * sizeof(uint32_t));
    memset(g_journal.txn_data, 0, max_blocks * sizeof(uint8_t *));
    g_journal.txn_blocks = 0;
    g_journal.txn_capacity = max_blocks;
    g_journal.in_transaction = 1;

    return 0;
}

int journal_write(uint64_t fs_block, const void *data) {
    if (!g_journal_initialized || !g_journal.in_transaction) return -1;
    if (g_journal.txn_blocks >= g_journal.txn_capacity) return -1;

    uint32_t idx = g_journal.txn_blocks;
    g_journal.txn_fs_blocks[idx] = (uint32_t)fs_block;

    g_journal.txn_data[idx] = (uint8_t *)kmalloc(g_journal.block_size);
    if (!g_journal.txn_data[idx]) return -1;

    memcpy(g_journal.txn_data[idx], data, g_journal.block_size);
    g_journal.txn_blocks++;

    return 0;
}

int journal_commit(void) {
    if (!g_journal_initialized || !g_journal.in_transaction) return -1;
    if (g_journal.txn_blocks == 0) {
        free_transaction();
        return 0;
    }

    uint32_t block_size = g_journal.block_size;
    uint64_t seq = g_journal.sequence++;

    /* Calculate total blocks needed: 1 descriptor + N data + 1 commit */
    uint64_t total_blocks = 1 + g_journal.txn_blocks + 1;

    /* Allocate buffer for descriptor + data blocks */
    uint8_t *desc_buf = (uint8_t *)kmalloc(block_size);
    if (!desc_buf) { free_transaction(); return -1; }
    memset(desc_buf, 0, block_size);

    struct journal_block_header *hdr = (struct journal_block_header *)desc_buf;
    hdr->jbh_magic     = JOURNAL_MAGIC_DESC;
    hdr->jbh_sequence  = seq;
    hdr->jbh_num_blocks = g_journal.txn_blocks;

    /* Fill in data block descriptors */
    struct journal_data_block *jdbs = (struct journal_data_block *)(desc_buf + sizeof(struct journal_block_header));
    for (uint32_t i = 0; i < g_journal.txn_blocks; i++) {
        jdbs[i].jdb_fs_block = g_journal.txn_fs_blocks[i];
        jdbs[i].jdb_checksum = calc_checksum(g_journal.txn_data[i], block_size);
    }

    hdr->jbh_checksum = calc_checksum(hdr, sizeof(struct journal_block_header) - sizeof(uint32_t));

    /* Mark journal dirty */
    g_journal.dirty = 1;
    write_jsb();

    /* Phase 1: Write descriptor block to journal */
    if (journal_write_block(g_journal.head, desc_buf) < 0) {
        kfree(desc_buf);
        free_transaction();
        return -1;
    }
    kfree(desc_buf);

    /* Phase 2: Write data blocks to journal */
    for (uint32_t i = 0; i < g_journal.txn_blocks; i++) {
        if (journal_write_block(g_journal.head + 1 + i, g_journal.txn_data[i]) < 0) {
            free_transaction();
            return -1;
        }
    }

    /* Phase 3: Write commit record */
    struct journal_commit_record commit;
    memset(&commit, 0, sizeof(commit));
    commit.jcr_magic    = JOURNAL_MAGIC_COMMIT;
    commit.jcr_sequence = seq;
    commit.jcr_checksum = calc_checksum(&commit, sizeof(commit) - sizeof(uint32_t));

    if (journal_write_block(g_journal.head + 1 + g_journal.txn_blocks, &commit) < 0) {
        free_transaction();
        return -1;
    }

    /* Phase 4: Checkpoint — write data to actual filesystem locations */
    uint32_t sectors_per_block = block_size / g_journal.bdev->block_size;
    for (uint32_t i = 0; i < g_journal.txn_blocks; i++) {
        uint64_t sector = g_journal.txn_fs_blocks[i] * sectors_per_block;
        if (block_dev_write(g_journal.bdev, g_journal.txn_data[i],
                            sector, (int)sectors_per_block) < 0) {
            log_printf(LOG_LEVEL_ERR, "journal: checkpoint write failed for block %llu\n",
                       (unsigned long long)g_journal.txn_fs_blocks[i]);
            free_transaction();
            return -1;
        }
    }

    /* Phase 5: Advance head and mark clean */
    advance_head(total_blocks);
    g_journal.dirty = 0;
    write_jsb();

    log_printf(LOG_LEVEL_DEBUG, "journal: committed transaction %llu (%u blocks)\n",
               (unsigned long long)seq, g_journal.txn_blocks);

    free_transaction();
    return 0;
}

int journal_rollback(void) {
    if (!g_journal_initialized || !g_journal.in_transaction) return -1;

    log_printf(LOG_LEVEL_DEBUG, "journal: rolling back transaction (%u blocks)\n",
               g_journal.txn_blocks);

    free_transaction();
    return 0;
}

int journal_recover(void) {
    if (!g_journal_initialized) return -1;

    uint32_t block_size = g_journal.block_size;
    uint64_t pos = g_journal.tail;
    uint64_t end = g_journal.head;
    int recovered = 0;

    log_printf(LOG_LEVEL_INFO, "journal: scanning for transactions (tail=%llu, head=%llu)\n",
               (unsigned long long)g_journal.tail, (unsigned long long)g_journal.head);

    uint8_t *block_buf = (uint8_t *)kmalloc(block_size);
    if (!block_buf) return -1;

    while (pos < end) {
        /* Read descriptor block header into the heap buffer */
        if (journal_read_block(pos, block_buf) < 0) break;
        struct journal_block_header *hdr = (struct journal_block_header *)block_buf;
        if (hdr->jbh_magic != JOURNAL_MAGIC_DESC) {
            pos++;
            continue;
        }

        uint64_t commit_pos = pos + 1 + hdr->jbh_num_blocks;
        if (commit_pos >= end) break;

        if (journal_read_block(commit_pos, block_buf) < 0) break;
        struct journal_commit_record *cmt = (struct journal_commit_record *)block_buf;
        if (cmt->jcr_magic != JOURNAL_MAGIC_COMMIT ||
            cmt->jcr_sequence != hdr->jbh_sequence) {
            /* No valid commit — transaction was interrupted, skip it */
            pos = commit_pos + 1;
            continue;
        }

        /* Valid committed transaction — replay it */
        log_printf(LOG_LEVEL_INFO, "journal: replaying transaction %llu (%u blocks)\n",
                   (unsigned long long)hdr.jbh_sequence, hdr.jbh_num_blocks);

        /* Read the descriptor block again to get the full block descriptor area */
        if (journal_read_block(pos, block_buf) < 0) break;

        struct journal_data_block *jdbs = (struct journal_data_block *)
            (block_buf + sizeof(struct journal_block_header));

        uint32_t sectors_per_block = block_size / g_journal.bdev->block_size;

        for (uint32_t i = 0; i < hdr.jbh_num_blocks; i++) {
            /* Read the data block from the journal */
            uint8_t *data_buf = (uint8_t *)kmalloc(block_size);
            if (!data_buf) continue;

            if (journal_read_block(pos + 1 + i, data_buf) == 0) {
                /* Verify checksum */
                uint32_t csum = calc_checksum(data_buf, block_size);
                if (csum == jdbs[i].jdb_checksum) {
                    /* Write to the actual filesystem location */
                    uint64_t sector = jdbs[i].jdb_fs_block * sectors_per_block;
                    if (block_dev_write(g_journal.bdev, data_buf, sector,
                                        (int)sectors_per_block) < 0) {
                        log_printf(LOG_LEVEL_ERR, "journal: replay write failed for block %llu\n",
                                   (unsigned long long)jdbs[i].jdb_fs_block);
                    }
                } else {
                    log_printf(LOG_LEVEL_WARN, "journal: checksum mismatch in replay (block %llu)\n",
                               (unsigned long long)jdbs[i].jdb_fs_block);
                }
            }
            kfree(data_buf);
        }

        recovered++;
        pos = commit_pos + 1;
    }

    kfree(block_buf);

    /* Reset journal pointers */
    g_journal.tail = g_journal.head;
    g_journal.dirty = 0;
    write_jsb();

    if (recovered > 0) {
        log_printf(LOG_LEVEL_INFO, "journal: recovered %d transactions\n", recovered);
    } else {
        log_printf(LOG_LEVEL_INFO, "journal: no transactions to recover\n");
    }

    return 0;
}

int journal_sync(void) {
    if (!g_journal_initialized) return -1;
    return write_jsb();
}

void journal_get_stats(uint64_t *total_blocks, uint64_t *used_blocks,
                       uint64_t *num_transactions, int *dirty) {
    if (!g_journal_initialized) {
        if (total_blocks) *total_blocks = 0;
        if (used_blocks) *used_blocks = 0;
        if (num_transactions) *num_transactions = 0;
        if (dirty) *dirty = 0;
        return;
    }
    if (total_blocks) *total_blocks = g_journal.journal_blocks;
    if (used_blocks) *used_blocks = (g_journal.head >= g_journal.tail) ?
        (g_journal.head - g_journal.tail) : 0;
    if (num_transactions) *num_transactions = g_journal.sequence - 1;
    if (dirty) *dirty = g_journal.dirty;
}

int journal_is_clean(void) {
    if (!g_journal_initialized) return 1;
    return !g_journal.dirty;
}