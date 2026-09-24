#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define BLOCK_SIZE 4096U
#define INODE_SIZE 128U

#define JOURNAL_BLOCK_IDX 1U
#define JOURNAL_BLOCKS 16U
#define INODE_BLOCKS 2U
#define DATA_BLOCKS 64U

#define INODE_BMAP_IDX (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS (DATA_START_IDX + DATA_BLOCKS)

#define DIRECT_POINTERS 8U
#define NAME_LEN 28
#define DEFAULT_IMAGE "vsfs.img"

#define JOURNAL_MAGIC 0x4A524E4C
#define REC_DATA 1
#define REC_COMMIT 2

struct superblock
{
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
    uint8_t _pad[128 - 9 * 4];
};

struct inode
{
    uint16_t type; // 0=free, 1=file, 2=dir
    uint16_t links;
    uint32_t size;
    uint32_t direct[DIRECT_POINTERS];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
};

struct dirent
{
    uint32_t inode;
    char name[NAME_LEN];
};

struct journal_header
{
    uint32_t magic;
    uint32_t nbytes_used;
};

struct rec_header
{
    uint16_t type;
    uint16_t size;
};

struct data_record
{
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t data[BLOCK_SIZE];
};

struct commit_record
{
    struct rec_header hdr;
};

_Static_assert(sizeof(struct superblock) == 128, "superblock must be 128 bytes");
_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");

// Helper functions to exit
static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static void pread_block(int fd, uint32_t block_index, void *buf)
{
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pread(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE)
        die("pread");
}

static void pwrite_block(int fd, uint32_t block_index, const void *buf)
{
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE)
        die("pwrite");
}

static int bitmap_test(const uint8_t *bitmap, uint32_t index)
{
    return (bitmap[index / 8] >> (index % 8)) & 0x1;
}

static void bitmap_set(uint8_t *bitmap, uint32_t index)
{
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

static uint32_t find_free_inode(const uint8_t *inode_bitmap, uint32_t inode_count)
{
    for (uint32_t i = 1; i < inode_count; i++)
    {
        if (!bitmap_test(inode_bitmap, i))
            return i;
    }
    return UINT32_MAX;
}

static uint32_t find_free_dirent_slot(const uint8_t *root_data)
{
    const struct dirent *dirents = (const struct dirent *)root_data;
    uint32_t num_entries = BLOCK_SIZE / sizeof(struct dirent);
    for (uint32_t i = 0; i < num_entries; i++)
    {
        // A slot is free only if it has no inode AND no name. "." and ".."
        // legitimately have inode == 0 (they point at the root's own inode
        // number), so inode == 0 alone does not mean "free" -- that would
        // let a new entry silently overwrite "." or "..".
        if (dirents[i].inode == 0 && dirents[i].name[0] == '\0')
            return i;
    }
    return UINT32_MAX;
}

static int journal_append_data_record(int fd, uint32_t block_no, const uint8_t *block_data)
{
    uint8_t journal_buffer[JOURNAL_BLOCKS * BLOCK_SIZE];

    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++)
        pread_block(fd, JOURNAL_BLOCK_IDX + i, journal_buffer + i * BLOCK_SIZE);

    struct journal_header *hdr = (struct journal_header *)journal_buffer;

    // Initializing journal for first time
    if (hdr->magic != JOURNAL_MAGIC)
    {
        hdr->magic = JOURNAL_MAGIC;
        hdr->nbytes_used = sizeof(struct journal_header);
    }

    uint32_t offset = hdr->nbytes_used;
    uint32_t data_record_size = sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE;

    if (offset + data_record_size > JOURNAL_BLOCKS * BLOCK_SIZE)
        return -1; // Journal full

    // Appending DATA/Transaction
    struct rec_header *rec_hdr = (struct rec_header *)(journal_buffer + offset);
    rec_hdr->type = REC_DATA;
    rec_hdr->size = data_record_size;
    offset += sizeof(struct rec_header);

    uint32_t *blk_no = (uint32_t *)(journal_buffer + offset);
    *blk_no = block_no;
    offset += sizeof(uint32_t);

    memcpy(journal_buffer + offset, block_data, BLOCK_SIZE);
    offset += BLOCK_SIZE;

    // Updating header
    hdr->nbytes_used = offset;

    // Writing journal
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++)
        pwrite_block(fd, JOURNAL_BLOCK_IDX + i, journal_buffer + i * BLOCK_SIZE);

    return 0;
}

static int journal_append_commit(int fd)
{
    uint8_t journal_buffer[JOURNAL_BLOCKS * BLOCK_SIZE];

    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++)
        pread_block(fd, JOURNAL_BLOCK_IDX + i, journal_buffer + i * BLOCK_SIZE);

    struct journal_header *hdr = (struct journal_header *)journal_buffer;

    // Checking if journal is initialized
    if (hdr->magic != JOURNAL_MAGIC)
    {
        fprintf(stderr, "Error: journal does not exist\n");
        return -1;
    }

    uint32_t offset = hdr->nbytes_used;
    uint32_t commit_record_size = sizeof(struct rec_header);

    if (offset + commit_record_size > JOURNAL_BLOCKS * BLOCK_SIZE)
        return -1; // Journal full

    // Appending COMMIT record
    struct rec_header *rec_hdr = (struct rec_header *)(journal_buffer + offset);
    rec_hdr->type = REC_COMMIT;
    rec_hdr->size = commit_record_size;
    offset += commit_record_size;

    // Updating header
    hdr->nbytes_used = offset;

    // Writing journal
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++)
        pwrite_block(fd, JOURNAL_BLOCK_IDX + i, journal_buffer + i * BLOCK_SIZE);

    return 0;
}

static void journal_clear(int fd)
{
    uint8_t block[BLOCK_SIZE];
    memset(block, 0, BLOCK_SIZE);

    struct journal_header *hdr = (struct journal_header *)block;
    hdr->magic = JOURNAL_MAGIC;
    hdr->nbytes_used = sizeof(struct journal_header);

    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++)
        pwrite_block(fd, JOURNAL_BLOCK_IDX + i, block);

    memset(block, 0, BLOCK_SIZE);
}

// A transaction can hold at most a handful of DATA records before the
// 16-block journal region runs out of space (each DATA record is a full
// block image, ~4KB), so a small fixed-size pending list is plenty.
#define MAX_PENDING_RECORDS 32

// The spec explicitly allows multiple create() transactions to be queued
// in the journal before install() is ever run ("You may append multiple
// transactions into the single journal block until it fills"). So before
// computing a new transaction, we must fold in any transactions that are
// already committed in the journal but not yet installed -- otherwise two
// queued create() calls would both think the same inode/dirent slot is
// free and the second would silently clobber the first when install runs.
//
// This mirrors cmd_install's walk exactly, except it applies each
// committed transaction's block images into the caller's in-memory
// buffers instead of writing them to disk. Uncommitted trailing records
// (if any) are ignored, matching install's semantics.
static void replay_pending_journal(int fd,
                                    uint8_t *inode_bitmap,
                                    uint8_t *inode_blocks,
                                    uint32_t inode_start,
                                    uint8_t *root_data,
                                    uint32_t root_block_no)
{
    uint8_t journal_buffer[JOURNAL_BLOCKS * BLOCK_SIZE];
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++)
        pread_block(fd, JOURNAL_BLOCK_IDX + i, journal_buffer + i * BLOCK_SIZE);

    struct journal_header *hdr = (struct journal_header *)journal_buffer;
    if (hdr->magic != JOURNAL_MAGIC)
        return; // journal not initialized yet; nothing pending

    uint32_t offset = sizeof(struct journal_header);
    uint32_t total_size = hdr->nbytes_used;

    uint32_t pending_block_no[MAX_PENDING_RECORDS];
    uint8_t *pending_data[MAX_PENDING_RECORDS];
    uint32_t pending_count = 0;

    while (offset < total_size)
    {
        struct rec_header *rec_hdr = (struct rec_header *)(journal_buffer + offset);
        if (offset + rec_hdr->size > total_size)
            break; // malformed tail; ignore, same as install

        if (rec_hdr->type == REC_DATA)
        {
            if (pending_count >= MAX_PENDING_RECORDS)
                break;
            uint32_t *block_no = (uint32_t *)(journal_buffer + offset + sizeof(struct rec_header));
            uint8_t *block_data = journal_buffer + offset + sizeof(struct rec_header) + sizeof(uint32_t);
            pending_block_no[pending_count] = *block_no;
            pending_data[pending_count] = block_data;
            pending_count++;
        }
        else if (rec_hdr->type == REC_COMMIT)
        {
            for (uint32_t i = 0; i < pending_count; i++)
            {
                uint32_t blk = pending_block_no[i];
                if (blk == INODE_BMAP_IDX)
                {
                    memcpy(inode_bitmap, pending_data[i], BLOCK_SIZE);
                }
                else if (blk >= inode_start && blk < inode_start + INODE_BLOCKS)
                {
                    memcpy(inode_blocks + (blk - inode_start) * BLOCK_SIZE, pending_data[i], BLOCK_SIZE);
                }
                else if (blk == root_block_no)
                {
                    memcpy(root_data, pending_data[i], BLOCK_SIZE);
                }
            }
            pending_count = 0;
        }
        else
        {
            break;
        }

        offset += rec_hdr->size;
    }
    // Any leftover uncommitted pending records are ignored (not applied),
    // consistent with what install() will do to them.
}

static uint32_t journal_current_size(int fd)
{
    uint8_t block[BLOCK_SIZE];
    pread_block(fd, JOURNAL_BLOCK_IDX, block);
    struct journal_header *hdr = (struct journal_header *)block;
    if (hdr->magic != JOURNAL_MAGIC)
        return sizeof(struct journal_header); // will be initialized on first append
    return hdr->nbytes_used;
}

static int cmd_create(int fd, const char *name)
{
    // Reading current state
    struct superblock sb;
    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t data_bitmap[BLOCK_SIZE];
    uint8_t inode_blocks[INODE_BLOCKS * BLOCK_SIZE];
    uint8_t root_data[BLOCK_SIZE];

    pread_block(fd, 0, &sb);
    pread_block(fd, INODE_BMAP_IDX, inode_bitmap);
    pread_block(fd, DATA_BMAP_IDX, data_bitmap);
    for (uint32_t i = 0; i < INODE_BLOCKS; i++)
        pread_block(fd, INODE_START_IDX + i, inode_blocks + i * BLOCK_SIZE);
    pread_block(fd, sb.data_start, root_data);

    // Fold in any transactions already committed in the journal but not
    // yet installed, so a second queued create() doesn't pick the same
    // "free" inode/slot as a first one still waiting for install.
    replay_pending_journal(fd, inode_bitmap, inode_blocks, sb.inode_start,
                           root_data, sb.data_start);

    // Finding free inode
    uint32_t new_inum = find_free_inode(inode_bitmap, sb.inode_count);
    if (new_inum == UINT32_MAX)
    {
        fprintf(stderr, "Error: no free inodes\n");
        return -1;
    }

    // Finding free directory entry slot in root
    uint32_t slot = find_free_dirent_slot(root_data);
    if (slot == UINT32_MAX)
    {
        fprintf(stderr, "Error: root directory is full\n");
        return -1;
    }

    // Computing new metadata blocks in memory
    uint8_t new_inode_bitmap[BLOCK_SIZE];
    uint8_t new_inode_blocks[INODE_BLOCKS * BLOCK_SIZE];
    uint8_t new_root_data[BLOCK_SIZE];

    memcpy(new_inode_bitmap, inode_bitmap, BLOCK_SIZE);
    memcpy(new_inode_blocks, inode_blocks, INODE_BLOCKS * BLOCK_SIZE);
    memcpy(new_root_data, root_data, BLOCK_SIZE);

    // Updating inode bitmap
    bitmap_set(new_inode_bitmap, new_inum);

    // Creating new inode
    struct inode *inodes = (struct inode *)new_inode_blocks;
    struct inode *new_inode = &inodes[new_inum];
    memset(new_inode, 0, sizeof(struct inode));
    new_inode->type = 1;
    new_inode->links = 1;
    new_inode->size = 0;
    new_inode->ctime = (uint32_t)time(NULL);
    new_inode->mtime = new_inode->ctime;

    // Adding directory entry
    struct dirent *root_dirents = (struct dirent *)new_root_data;
    root_dirents[slot].inode = new_inum;
    strncpy(root_dirents[slot].name, name, NAME_LEN - 1);
    root_dirents[slot].name[NAME_LEN - 1] = '\0';

    // Root directory now holds one more entry: keep its recorded size in
    // sync so a later validator/install pass sees the right byte count.
    struct inode *root_inode = &inodes[0];
    if (root_inode->size < (slot + 1) * sizeof(struct dirent))
        root_inode->size = (slot + 1) * (uint32_t)sizeof(struct dirent);

    // Determining which inode block was modified. Root (inode 0) always
    // lives in the first inode block; if the new inode lands in a
    // different block, root's updated size (set above) lives in a block
    // that is otherwise never journaled, and would be silently lost. So we
    // must journal root's block too whenever it differs from the new
    // inode's block.
    uint32_t inodes_per_block = BLOCK_SIZE / INODE_SIZE;
    uint32_t inode_block_index = new_inum / inodes_per_block;
    uint32_t modified_inode_block = INODE_START_IDX + inode_block_index;
    uint32_t root_inode_block = INODE_START_IDX; // inode 0 is always in block 0
    int root_shares_block = (modified_inode_block == root_inode_block);
    uint32_t num_inode_block_records = root_shares_block ? 1u : 2u;

    // Checking there is room for the WHOLE transaction before writing any
    // part of it. Appending record-by-record and only then discovering the
    // journal is full would leave earlier records in this transaction
    // dangling without a COMMIT -- refuse atomically instead, per spec.
    uint32_t data_record_size = (uint32_t)(sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE);
    uint32_t commit_record_size = (uint32_t)sizeof(struct rec_header);
    // Records: inode bitmap + one or two inode blocks + root directory data
    uint32_t num_data_records = 1 + num_inode_block_records + 1;
    uint32_t transaction_size = num_data_records * data_record_size + commit_record_size;
    uint32_t used = journal_current_size(fd);
    if (used + transaction_size > JOURNAL_BLOCKS * BLOCK_SIZE)
    {
        fprintf(stderr, "Error: not enough space left in the journal for this transaction, "
                        "run './journal install' first\n");
        return -1;
    }

    // Appending the transaction to the journal: one DATA record per
    // modified metadata block, sealed with a COMMIT record. Nothing is
    // written to the home locations here -- 'install' is responsible for
    // that.
    if (journal_append_data_record(fd, INODE_BMAP_IDX, new_inode_bitmap) < 0)
    {
        fprintf(stderr, "Error: journal is full, run './journal install' first\n");
        return -1;
    }

    if (!root_shares_block)
    {
        // Root's own inode block, carrying the updated directory size.
        if (journal_append_data_record(fd, root_inode_block, new_inode_blocks) < 0)
        {
            fprintf(stderr, "Error: journal is full, run './journal install' first\n");
            return -1;
        }
    }

    if (journal_append_data_record(fd,
                                   modified_inode_block,
                                   new_inode_blocks + inode_block_index * BLOCK_SIZE) < 0)
    {
        fprintf(stderr, "Error: journal is full, run './journal install' first\n");
        return -1;
    }

    if (journal_append_data_record(fd, sb.data_start, new_root_data) < 0)
    {
        fprintf(stderr, "Error: journal is full, run './journal install' first\n");
        return -1;
    }

    if (journal_append_commit(fd) < 0)
    {
        fprintf(stderr, "Error: journal is full, run './journal install' first\n");
        return -1;
    }

    printf("Logged and committed creation of '%s' (inode %u) to the journal.\n"
           "Run './journal install' to apply it to disk.\n",
           name, new_inum);
    return 0;
}

static int cmd_commit(int fd)
{
    if (journal_append_commit(fd) < 0)
    {
        fprintf(stderr, "Error: journal is full or not initialized\n");
        return -1;
    }
    printf("Committed current journal transaction\n");
    return 0;
}

static int cmd_install(int fd)
{
    uint8_t journal_buffer[JOURNAL_BLOCKS * BLOCK_SIZE];

    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++)
    {
        pread_block(fd, JOURNAL_BLOCK_IDX + i,
                    journal_buffer + i * BLOCK_SIZE);
    }

    struct journal_header *hdr = (struct journal_header *)journal_buffer;

    if (hdr->magic != JOURNAL_MAGIC)
    {
        fprintf(stderr, "Error: journal does not exist\n");
        return -1;
    }

    uint32_t offset = sizeof(struct journal_header);
    uint32_t total_size = hdr->nbytes_used;

    // Pending DATA records for the transaction currently being scanned.
    // They are only written to their home locations once a COMMIT record
    // for that transaction is found -- a transaction with no COMMIT is not
    // installed at all, per spec.
    uint32_t pending_block_no[MAX_PENDING_RECORDS];
    uint8_t *pending_data[MAX_PENDING_RECORDS];
    uint32_t pending_count = 0;
    uint32_t transactions_installed = 0;

    while (offset < total_size)
    {
        struct rec_header *rec_hdr =
            (struct rec_header *)(journal_buffer + offset);

        if (offset + rec_hdr->size > total_size)
        {
            fprintf(stderr,
                    "Error: incomplete record, discarding tail\n");
            break;
        }

        if (rec_hdr->type == REC_DATA)
        {
            if (pending_count >= MAX_PENDING_RECORDS)
            {
                fprintf(stderr, "Error: too many DATA records in one transaction\n");
                break;
            }

            uint32_t *block_no =
                (uint32_t *)(journal_buffer + offset +
                             sizeof(struct rec_header));
            uint8_t *block_data =
                journal_buffer + offset +
                sizeof(struct rec_header) + sizeof(uint32_t);

            pending_block_no[pending_count] = *block_no;
            pending_data[pending_count] = block_data;
            pending_count++;
        }
        else if (rec_hdr->type == REC_COMMIT)
        {
            // Transaction is valid: replay every DATA record collected
            // since the last commit, verbatim, to its home block.
            for (uint32_t i = 0; i < pending_count; i++)
                pwrite_block(fd, pending_block_no[i], pending_data[i]);

            transactions_installed++;
            pending_count = 0;
        }
        else
        {
            fprintf(stderr, "Error: unknown record type %u\n", rec_hdr->type);
            break;
        }

        offset += rec_hdr->size;
    }

    if (pending_count > 0)
        fprintf(stderr,
                "Warning: discarding %u uncommitted DATA record(s)\n", pending_count);

    journal_clear(fd);
    printf("Installed %u committed transaction(s) and cleared journal\n", transactions_installed);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <command> [args]\n", argv[0]);
        fprintf(stderr, "Commands: create <name>, commit, install\n");
        return EXIT_FAILURE;
    }

    int fd = open(DEFAULT_IMAGE, O_RDWR);
    if (fd < 0)
        die("open");

    int result = EXIT_FAILURE;

    if (strcmp(argv[1], "create") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Usage: %s create <name>\n", argv[0]);
        }
        else
        {
            result = (cmd_create(fd, argv[2]) == 0)
                         ? EXIT_SUCCESS
                         : EXIT_FAILURE;
        }
    }
    else if (strcmp(argv[1], "commit") == 0)
    {
        result = (cmd_commit(fd) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    else if (strcmp(argv[1], "install") == 0)
    {
        result = (cmd_install(fd) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    else
    {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
    }

    if (close(fd) < 0)
        die("close");

    return result;
}
