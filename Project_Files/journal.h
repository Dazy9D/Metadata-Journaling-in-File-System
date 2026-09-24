#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>


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

#define FS_MAGIC 0x56534653U      
#define JOURNAL_MAGIC 0x4A524E4CU 

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
    uint32_t inode; // inode number (0 = unused)
    char name[NAME_LEN];
};

/*
 * Journal header lives at offset 0 of the journal region (block
 * JOURNAL_BLOCK_IDX). It is NOT padded to BLOCK_SIZE: journal.c always
 * reads/writes the journal through a full JOURNAL_BLOCKS*BLOCK_SIZE
 * scratch buffer and only ever casts a pointer into that buffer to this
 * type. Callers must never pread/pwrite directly into a bare
 * `struct journal_header` variable, since that would only touch
 * sizeof(struct journal_header) bytes instead of a full block.
 */
struct journal_header
{
    uint32_t magic;       // JOURNAL_MAGIC when initialized
    uint32_t nbytes_used; // total bytes used, including this header
};

struct rec_header
{
    uint16_t type; // REC_DATA or REC_COMMIT
    uint16_t size; // total size of this record in bytes
};

struct data_record
{
    struct rec_header hdr; // type = REC_DATA
    uint32_t block_no;     // home block number on disk
    uint8_t data[BLOCK_SIZE]; // full block image
};

struct commit_record
{
    struct rec_header hdr; // type = REC_COMMIT
};

_Static_assert(sizeof(struct superblock) == 128, "superblock must be 128 bytes");
_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");
_Static_assert(sizeof(struct data_record) == sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE,
               "data_record must be tightly packed (no padding)");

#endif /* JOURNAL_H */
