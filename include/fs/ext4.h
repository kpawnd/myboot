/*
 * Read-only ext2/ext3/ext4 driver (src/fs/ext4.c). Firmware-independent:
 * it reads through a struct blockdev and keeps all state in the structures
 * below plus static block buffers, so it never allocates.
 *
 * Supported: block sizes 1-64 KiB, 32- and 64-bit group descriptors,
 * meta_bg, extent trees of any depth, ext2/3 block maps, unwritten extents
 * and holes (read as zeros), htree directories (read linearly), fast and
 * slow symlinks, inline data that fits in the inode (60 bytes).
 * Not supported: encrypted files and directories (fscrypt), inline data
 * larger than the inode's block area, compression and external journals.
 * The journal is not replayed: a filesystem that was not cleanly unmounted
 * may show the state of its last checkpoint.
 */
#ifndef FS_EXT4_H
#define FS_EXT4_H

#include <stdbool.h>
#include <stdint.h>
#include "fs/blockdev.h"
#include "fs/fs.h"

#define EXT4_MAX_BLOCK  65536u
#define EXT4_PATH_MAX   1024u /* longest path accepted, symlinks expanded */

struct ext4_fs {
    struct blockdev *dev;
    uint32_t block_size, log_block;
    uint64_t blocks;
    uint32_t groups;
    uint32_t blocks_per_group, inodes_per_group;
    uint32_t inode_size, desc_size;
    uint32_t first_data_block, first_meta_bg;
    uint32_t incompat, ro_compat;
    uint8_t  uuid[16];
    char     label[17];
};

struct ext4_inode {
    uint32_t ino;
    uint16_t mode;
    uint32_t flags;
    uint64_t size;
    uint32_t blocks_lo, file_acl;
    uint8_t  block[60]; /* i_block: extent root, block map, or inline data */
};

struct ext4_file {
    struct ext4_fs   *fs;
    struct ext4_inode inode;
    uint64_t          size;
};

/* Is this an ext2/3/4 superblock? `sb` is the 1024 bytes at offset 1024. */
bool ext4_probe(const uint8_t *sb);
enum fs_error ext4_mount(struct ext4_fs *fs, struct blockdev *dev);
/* `path` is absolute; '/' and '\' both separate components; names are
 * case-sensitive. Symlinks are followed. */
enum fs_error ext4_open(struct ext4_fs *fs, const char *path, struct ext4_file *f);
enum fs_error ext4_read(struct ext4_file *f, uint64_t offset, void *dst, uint64_t len);

#endif
