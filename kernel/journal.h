/*
 * journal.h - Write-Ahead Logging (WAL) journal for filesystem metadata
 *
 * Implements a circular journal on a block device to ensure filesystem
 * consistency across crashes.  Uses a write-ahead protocol:
 *   1. Write data blocks to journal (WAL)
 *   2. Write commit record
 *   3. Write data to actual filesystem locations (checkpoint)
 *   4. Mark journal entry as checkpointed
 *
 * On recovery, committed-but-not-checkpointed transactions are replayed.
 *
 * Journal layout on disk:
 *   [Journal Superblock]  (1 block)
 *   [Transaction 0]       (variable size)
 *   [Transaction 1]
 *   ...
 *
 * Each transaction:
 *   [Descriptor block]    (journal_block_header)
 *   [Data block 0]        (journal_data_block)
 *   [Data block 1]
 *   ...
 *   [Commit block]        (journal_commit_record)
 */
#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * Magic numbers
 * ================================================================ */
#define JOURNAL_MAGIC_SB     0x4A524E4C  /* "JRNL" */
#define JOURNAL_MAGIC_DESC   0x5452414E  /* "TRAN" */
#define JOURNAL_MAGIC_COMMIT 0x434F4D54  /* "COMT" */

/* ================================================================
 * Journal superblock (first block of journal area)
 * ================================================================ */
struct journal_superblock {
    uint32_t jsb_magic;          /* JOURNAL_MAGIC_SB */
    uint32_t jsb_block_size;     /* must match filesystem block size */
    uint32_t jsb_max_transactions;
    uint64_t jsb_start_block;    /* first journal data block (relative to device) */
    uint64_t jsb_num_blocks;     /* total journal blocks */
    uint64_t jsb_head;           /* next write position (offset from start_block) */
    uint64_t jsb_tail;           /* oldest valid transaction */
    uint64_t jsb_sequence;       /* monotonically increasing transaction ID */
    uint32_t jsb_flags;          /* 0 = clean, 1 = dirty (recovery needed) */
    uint32_t jsb_checksum;       /* simple XOR checksum of previous fields */
    uint32_t jsb_reserved[8];
} __attribute__((packed));

/* ================================================================
 * Transaction descriptor block
 * ================================================================ */
struct journal_block_header {
    uint32_t jbh_magic;          /* JOURNAL_MAGIC_DESC */
    uint64_t jbh_sequence;       /* transaction sequence number */
    uint32_t jbh_num_blocks;     /* number of data blocks in this transaction */
    uint32_t jbh_checksum;       /* checksum of this header */
    uint32_t jbh_reserved[4];
    /*
     * Followed by jbh_num_blocks entries of journal_data_block
     * describing which fs blocks are being journaled.
     */
} __attribute__((packed));

/* ================================================================
 * Data block descriptor
 * ================================================================ */
struct journal_data_block {
    uint64_t jdb_fs_block;       /* target filesystem block number */
    uint32_t jdb_checksum;       /* checksum of the data */
    uint32_t jdb_reserved[2];
    /* Followed by the actual block data (block_size bytes) */
} __attribute__((packed));

/* ================================================================
 * Commit record
 * ================================================================ */
struct journal_commit_record {
    uint32_t jcr_magic;          /* JOURNAL_MAGIC_COMMIT */
    uint64_t jcr_sequence;       /* matches transaction sequence */
    uint32_t jcr_checksum;       /* checksum of this record */
    uint32_t jcr_reserved[4];
} __attribute__((packed));

/* ================================================================
 * In-memory journal handle
 * ================================================================ */
struct journal_handle {
    struct block_device *bdev;
    uint32_t              block_size;
    uint64_t              journal_start;   /* first block of journal area */
    uint64_t              journal_blocks;  /* total journal blocks */
    uint64_t              head;            /* next write position */
    uint64_t              tail;            /* oldest valid */
    uint64_t              sequence;        /* next transaction ID */
    int                   in_transaction;  /* 1 if transaction is active */
    uint32_t              txn_blocks;      /* blocks in current transaction */
    uint32_t              txn_capacity;    /* max blocks in current transaction */
    uint32_t             *txn_fs_blocks;   /* fs block numbers in current txn */
    uint8_t            **txn_data;        /* data buffers for current txn */
    int                   dirty;           /* journal needs recovery */
};

/* ================================================================
 * Journal API
 * ================================================================ */

/*
 * journal_init: Initialize or recover a journal on a block device.
 * @bdev:           block device containing the journal
 * @journal_start:  first block of the journal area (device-relative)
 * @journal_blocks: number of blocks reserved for the journal
 * @block_size:     filesystem block size (must match bdev usage)
 * Returns 0 on success, -1 on failure.
 *
 * If the journal superblock exists and is dirty, recovery is attempted.
 * If the journal superblock does not exist, a new one is created.
 */
int journal_init(struct block_device *bdev, uint64_t journal_start,
                 uint64_t journal_blocks, uint32_t block_size);

/*
 * journal_begin: Start a new transaction.
 * @max_blocks: maximum number of blocks expected in this transaction
 * Returns 0 on success, -1 if a transaction is already active.
 */
int journal_begin(uint32_t max_blocks);

/*
 * journal_write: Add a block to the current transaction.
 * @fs_block: target filesystem block number
 * @data:     block data (must be block_size bytes)
 * Returns 0 on success, -1 on failure.
 *
 * The data is NOT written to the filesystem yet — only to the journal.
 * Actual filesystem writes happen during journal_commit (checkpoint).
 */
int journal_write(uint64_t fs_block, const void *data);

/*
 * journal_commit: Commit the current transaction.
 * Writes the commit record to the journal, then checkpoints
 * all data blocks to their actual filesystem locations.
 * Returns 0 on success, -1 on failure.
 */
int journal_commit(void);

/*
 * journal_rollback: Abort the current transaction.
 * Discards all blocks written since journal_begin.
 * No data is written to the filesystem.
 * Returns 0 on success, -1 if no transaction is active.
 */
int journal_rollback(void);

/*
 * journal_recover: Replay all committed transactions from the journal.
 * Called automatically by journal_init if the journal is dirty.
 * Returns 0 on success, -1 on failure.
 */
int journal_recover(void);

/*
 * journal_sync: Force all journaled data to the block device.
 * Returns 0 on success.
 */
int journal_sync(void);

/*
 * journal_get_stats: Query journal statistics.
 * @total_blocks:    output: total journal blocks
 * @used_blocks:     output: blocks currently in use
 * @num_transactions: output: number of transactions in journal
 * @dirty:           output: 1 if journal needs recovery
 */
void journal_get_stats(uint64_t *total_blocks, uint64_t *used_blocks,
                       uint64_t *num_transactions, int *dirty);

/*
 * journal_is_clean: Returns 1 if no recovery is needed, 0 otherwise.
 */
int journal_is_clean(void);

#endif /* JOURNAL_H */