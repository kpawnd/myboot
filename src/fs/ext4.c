/*
 * Read-only ext2/3/4 (interface and limits in include/fs/ext4.h).
 *
 * Layout, as far as reading files needs it (kernel.org, "ext4 Data
 * Structures and Algorithms"):
 *   superblock     1024 bytes at byte 1024 of the volume
 *   block groups   blocks_per_group blocks each, starting at first_data_block;
 *                  each group's descriptor gives its inode table
 *   inodes         inode N is entry (N-1) % inodes_per_group of the table
 *                  of group (N-1) / inodes_per_group; inode 2 is the root
 *   file data      i_block holds either an extent tree root (EXTENTS_FL) or
 *                  12 direct + single, double, triple indirect pointers
 *   directories    files of variable-length entries {inode, rec_len,
 *                  name_len, type, name}; htree indexes hide inside entries
 *                  with inode 0, so a linear scan stays correct
 *
 * Every number read from disk is checked before it is used as an index or
 * an address: a corrupt or hostile filesystem gives FS_CORRUPT, never a
 * read outside the device or a buffer.
 *
 * Large reads go straight from the device into the caller's buffer, one
 * read per contiguous run of blocks, so a kernel stored in a few extents
 * costs a few device reads.
 */
#include "fs/ext4.h"
#include "core/util.h"
#include "lib/mem.h"

#define SB_OFFSET   1024
#define EXT4_MAGIC  0xEF53
#define ROOT_INO    2
#define MAX_LINKS   8
#define NAME_MAX    255
#define DIR_MAX     MiB(256) /* millions of entries; larger is corruption */

#define INCOMPAT_COMPRESSION 0x0001u
#define INCOMPAT_FILETYPE    0x0002u
#define INCOMPAT_JOURNAL_DEV 0x0008u
#define INCOMPAT_META_BG     0x0010u
#define INCOMPAT_64BIT       0x0080u
#define INCOMPAT_DIRDATA     0x1000u
#define INCOMPAT_REFUSED     (INCOMPAT_COMPRESSION | INCOMPAT_JOURNAL_DEV | INCOMPAT_DIRDATA)
#define RO_COMPAT_SPARSE_SUPER 0x0001u

#define FL_ENCRYPT 0x00000800u
#define FL_EXTENTS 0x00080000u
#define FL_INLINE  0x10000000u

#define S_IFMT  0xF000u
#define S_IFDIR 0x4000u
#define S_IFREG 0x8000u
#define S_IFLNK 0xA000u

#define EXTENT_MAGIC     0xF30A
#define EXTENT_MAX_DEPTH 5
#define EXTENT_INIT_MAX  32768u /* longer lengths mark unwritten extents */

static uint8_t  g_node[EXT4_MAX_BLOCK];   /* extent tree node below the root */
static uint8_t  g_ind[3][EXT4_MAX_BLOCK]; /* block maps: one indirect block per level */
static uint64_t g_ind_blk[3];             /* block each g_ind holds; 0: none */
static const struct ext4_fs *g_ind_fs;    /* filesystem g_ind belongs to */
static uint8_t  g_dir[EXT4_MAX_BLOCK];    /* directory block being searched */
static char     g_path[2][EXT4_PATH_MAX]; /* path being walked, and its successor */

static size_t str_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

bool ext4_probe(const uint8_t *sb)
{
    return rd16(sb + 0x38) == EXT4_MAGIC && rd32(sb + 0x18) <= 6 && rd32(sb + 0x20) &&
           rd32(sb + 0x28);
}

enum fs_error ext4_mount(struct ext4_fs *fs, struct blockdev *dev)
{
    uint8_t sb[1024];
    uint32_t rev;

    if (bdev_read(dev, SB_OFFSET, sb, sizeof(sb)))
        return FS_IO;
    if (!ext4_probe(sb))
        return FS_CORRUPT;
    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;
    fs->log_block = 10 + rd32(sb + 0x18);
    fs->block_size = 1u << fs->log_block;
    fs->first_data_block = rd32(sb + 0x14);
    fs->blocks_per_group = rd32(sb + 0x20);
    fs->inodes_per_group = rd32(sb + 0x28);
    fs->incompat = rd32(sb + 0x60);
    fs->ro_compat = rd32(sb + 0x64);
    fs->first_meta_bg = rd32(sb + 0x104);
    fs->blocks = rd32(sb + 0x04);
    if (fs->incompat & INCOMPAT_64BIT)
        fs->blocks |= (uint64_t)rd32(sb + 0x150) << 32;
    rev = rd32(sb + 0x4C);
    fs->inode_size = rev == 0 ? 128 : rd16(sb + 0x58);
    fs->desc_size = (fs->incompat & INCOMPAT_64BIT) ? rd16(sb + 0xFE) : 32;
    memcpy(fs->uuid, sb + 0x68, 16);
    memcpy(fs->label, sb + 0x78, 16);
    fs->label[16] = 0;

    if (fs->incompat & INCOMPAT_REFUSED)
        return FS_UNSUPPORTED;
    if (fs->inode_size < 128 || !is_pow2(fs->inode_size) || fs->inode_size > fs->block_size ||
        fs->desc_size < 32 || !is_pow2(fs->desc_size) || fs->desc_size > fs->block_size ||
        ((fs->incompat & INCOMPAT_64BIT) && fs->desc_size < 64))
        return FS_CORRUPT;
    /* The filesystem must fit in the partition: every block address below
     * `blocks` is then a valid device offset. */
    if (fs->blocks <= fs->first_data_block || fs->blocks > (dev->size >> fs->log_block))
        return FS_CORRUPT;
    uint64_t groups = (fs->blocks - fs->first_data_block + fs->blocks_per_group - 1) /
                      fs->blocks_per_group;
    if (groups > UINT32_MAX / fs->inodes_per_group)
        return FS_CORRUPT;
    fs->groups = (uint32_t)groups;
    return FS_OK;
}

/* sparse_super keeps superblock (and descriptor table) backups only in
 * groups 0, 1 and powers of 3, 5 and 7. */
static bool has_super(const struct ext4_fs *fs, uint32_t g)
{
    if (!(fs->ro_compat & RO_COMPAT_SPARSE_SUPER) || g <= 1)
        return true;
    for (uint32_t p = 3; p <= 7; p += 2) {
        uint64_t x = p;
        while (x < g)
            x *= p;
        if (x == g)
            return true;
    }
    return false;
}

static enum fs_error inode_table(const struct ext4_fs *fs, uint32_t g, uint64_t *blk)
{
    uint32_t per_block = fs->block_size / fs->desc_size;
    uint64_t off;
    uint8_t d[64];

    /* meta_bg: from first_meta_bg on, each run of per_block groups keeps
     * its descriptors in one block at the start of the run's first group. */
    if ((fs->incompat & INCOMPAT_META_BG) && g / per_block >= fs->first_meta_bg) {
        uint32_t first = g - g % per_block;
        uint64_t b = fs->first_data_block + (uint64_t)first * fs->blocks_per_group +
                     (has_super(fs, first) ? 1 : 0);
        if (b >= fs->blocks)
            return FS_CORRUPT;
        off = (b << fs->log_block) + (uint64_t)(g % per_block) * fs->desc_size;
    } else {
        off = ((uint64_t)(fs->first_data_block + 1) << fs->log_block) +
              (uint64_t)g * fs->desc_size;
    }
    if (bdev_read(fs->dev, off, d, fs->desc_size < 64 ? fs->desc_size : 64))
        return FS_IO;
    *blk = rd32(d + 0x08);
    if (fs->desc_size >= 64)
        *blk |= (uint64_t)rd32(d + 0x28) << 32;
    return *blk && *blk < fs->blocks ? FS_OK : FS_CORRUPT;
}

static enum fs_error read_inode(const struct ext4_fs *fs, uint32_t ino, struct ext4_inode *in)
{
    uint8_t raw[128];
    uint32_t g, idx;
    uint64_t tbl, off;
    enum fs_error e;

    if (ino == 0 || (ino - 1) / fs->inodes_per_group >= fs->groups)
        return FS_CORRUPT;
    g = (ino - 1) / fs->inodes_per_group;
    idx = (ino - 1) % fs->inodes_per_group;
    if ((e = inode_table(fs, g, &tbl)) != FS_OK)
        return e;
    off = (tbl << fs->log_block) + (uint64_t)idx * fs->inode_size;
    if (bdev_read(fs->dev, off, raw, sizeof(raw)))
        return FS_IO;
    in->ino = ino;
    in->mode = rd16(raw + 0x00);
    in->size = rd32(raw + 0x04) | (uint64_t)rd32(raw + 0x6C) << 32;
    in->blocks_lo = rd32(raw + 0x1C);
    in->flags = rd32(raw + 0x20);
    in->file_acl = rd32(raw + 0x68);
    memcpy(in->block, raw + 0x28, sizeof(in->block));
    return FS_OK;
}

/* ---- logical -> physical block mapping ------------------------------------
 * Each mapper returns the run starting at `lblk`: *pblk is its first
 * physical block (0: a hole, read as zeros) and *count its length. */

static enum fs_error map_extent(const struct ext4_fs *fs, const struct ext4_inode *in, uint32_t lblk,
                                uint64_t *pblk, uint64_t *count)
{
    const uint8_t *node = in->block;
    uint32_t node_size = sizeof(in->block);
    uint64_t limit = 1ull << 32; /* first logical block past this subtree */
    int depth = EXTENT_MAX_DEPTH + 1;

    for (;;) {
        uint32_t n = rd16(node + 2), max = rd16(node + 4), d = rd16(node + 6);
        const uint8_t *e = node + 12;

        if (rd16(node) != EXTENT_MAGIC || n > max || 12 + 12ull * max > node_size ||
            (int)d >= depth)
            return FS_CORRUPT;
        depth = (int)d;
        if (d == 0) {
            for (uint32_t i = 0; i < n; i++, e += 12) {
                uint32_t first = rd32(e), len = rd16(e + 4);
                bool unwritten = len > EXTENT_INIT_MAX;

                if (unwritten)
                    len -= EXTENT_INIT_MAX;
                if (lblk < first) {
                    *pblk = 0;
                    *count = first - lblk;
                    return FS_OK;
                }
                if (lblk - first < len) {
                    uint64_t start = (uint64_t)rd16(e + 6) << 32 | rd32(e + 8);
                    *count = len - (lblk - first);
                    *pblk = unwritten ? 0 : start + (lblk - first);
                    return !unwritten && !start ? FS_CORRUPT : FS_OK;
                }
            }
            *pblk = 0;
            *count = limit - lblk;
            return FS_OK;
        }

        /* Index node: the last child that starts at or before lblk. */
        uint32_t i = 0;
        uint64_t child;
        if (n == 0)
            return FS_CORRUPT;
        while (i + 1 < n && rd32(e + 12 * (i + 1)) <= lblk)
            i++;
        e += 12 * i;
        if (rd32(e) > lblk) {
            *pblk = 0;
            *count = rd32(e) - lblk;
            return FS_OK;
        }
        if (i + 1 < n)
            limit = rd32(e + 12);
        child = rd32(e + 4) | (uint64_t)rd16(e + 8) << 32;
        if (!child || child >= fs->blocks)
            return FS_CORRUPT;
        if (bdev_read(fs->dev, child << fs->log_block, g_node, fs->block_size))
            return FS_IO;
        node = g_node;
        node_size = fs->block_size;
    }
}

static enum fs_error ind_block(const struct ext4_fs *fs, int level, uint64_t blk, const uint8_t **out)
{
    if (blk >= fs->blocks)
        return FS_CORRUPT;
    if (g_ind_fs != fs) {
        g_ind_fs = fs;
        memset(g_ind_blk, 0, sizeof(g_ind_blk));
    }
    if (g_ind_blk[level] != blk) {
        g_ind_blk[level] = 0;
        if (bdev_read(fs->dev, blk << fs->log_block, g_ind[level], fs->block_size))
            return FS_IO;
        g_ind_blk[level] = blk;
    }
    *out = g_ind[level];
    return FS_OK;
}

/* The run of consecutive block numbers (or of zeros) from table[idx]. */
static void ptr_run(const uint8_t *table, uint32_t idx, uint32_t n, uint64_t *pblk, uint64_t *count)
{
    uint32_t p = rd32(table + 4 * idx), k = 1;

    while (idx + k < n && rd32(table + 4 * (idx + k)) == (p ? p + k : 0))
        k++;
    *pblk = p;
    *count = k;
}

static enum fs_error map_blockmap(const struct ext4_fs *fs, const struct ext4_inode *in, uint32_t lblk,
                                  uint64_t *pblk, uint64_t *count)
{
    uint32_t apb = fs->block_size / 4; /* pointers per block */
    uint64_t off = lblk, cover = apb;
    uint32_t ptr;
    int levels = 1;

    if (off < 12) {
        ptr_run(in->block, (uint32_t)off, 12, pblk, count);
        return FS_OK;
    }
    off -= 12;
    while (off >= cover) {
        off -= cover;
        if (++levels > 3)
            return FS_CORRUPT;
        cover *= apb;
    }
    ptr = rd32(in->block + 4 * (11 + levels));
    for (int l = 0; l < levels; l++) {
        const uint8_t *t;
        uint64_t each = cover / apb;
        uint32_t idx;
        enum fs_error e;

        if (!ptr) {
            *pblk = 0;
            *count = cover - off;
            return FS_OK;
        }
        if ((e = ind_block(fs, l, ptr, &t)) != FS_OK)
            return e;
        idx = (uint32_t)(off / each);
        if (each == 1) {
            ptr_run(t, idx, apb, pblk, count);
            return FS_OK;
        }
        ptr = rd32(t + 4 * idx);
        off -= idx * each;
        cover = each;
    }
    return FS_CORRUPT; /* not reached: the last level has each == 1 */
}

static enum fs_error map(const struct ext4_fs *fs, const struct ext4_inode *in, uint64_t lblk,
                         uint64_t *pblk, uint64_t *count)
{
    enum fs_error e;

    if (lblk >> 32)
        return FS_CORRUPT;
    e = (in->flags & FL_EXTENTS) ? map_extent(fs, in, (uint32_t)lblk, pblk, count)
                                 : map_blockmap(fs, in, (uint32_t)lblk, pblk, count);
    if (e == FS_OK && *pblk && (*pblk >= fs->blocks || *count > fs->blocks - *pblk))
        return FS_CORRUPT;
    return e;
}

/* [off, off+len) of the inode's data; the caller has checked the bounds. */
static enum fs_error read_data(const struct ext4_fs *fs, const struct ext4_inode *in, uint64_t off,
                               uint8_t *dst, uint64_t len)
{
    if (in->flags & FL_ENCRYPT)
        return FS_UNSUPPORTED;
    if (in->flags & FL_INLINE) {
        /* Data past the 60 bytes of i_block lives in an extended attribute. */
        if (in->size > sizeof(in->block))
            return FS_UNSUPPORTED;
        memcpy(dst, in->block + off, len);
        return FS_OK;
    }
    while (len) {
        uint64_t within = off & (fs->block_size - 1), pblk, count, n;
        enum fs_error e;

        if ((e = map(fs, in, off >> fs->log_block, &pblk, &count)) != FS_OK)
            return e;
        n = (count << fs->log_block) - within;
        if (n > len)
            n = len;
        if (!pblk)
            memset(dst, 0, n);
        else if (bdev_read(fs->dev, (pblk << fs->log_block) + within, dst, n))
            return FS_IO;
        dst += n;
        off += n;
        len -= n;
    }
    return FS_OK;
}

enum fs_error ext4_read(struct ext4_file *f, uint64_t offset, void *dst, uint64_t len)
{
    if (offset > f->size || len > f->size - offset)
        return FS_CORRUPT;
    return read_data(f->fs, &f->inode, offset, dst, len);
}

/* ---- directories and paths ----------------------------------------------- */

static enum fs_error scan_entries(const struct ext4_fs *fs, const uint8_t *b, uint32_t len,
                                  const char *name, uint32_t nlen, uint32_t *ino)
{
    bool filetype = fs->incompat & INCOMPAT_FILETYPE;

    for (uint32_t off = 0; off + 8 <= len;) {
        uint32_t rec = rd16(b + off + 4), elen = b[off + 6];

        if (!filetype)
            elen |= (uint32_t)b[off + 7] << 8;
        if (fs->block_size == 65536 && (rec == 0 || rec == 65535))
            rec = 65536; /* the one rec_len that does not fit in 16 bits */
        if (rec < 8 || rec % 4 || rec > len - off || elen + 8 > rec)
            return FS_CORRUPT;
        if (rd32(b + off) && elen == nlen && !memcmp(b + off + 8, name, nlen)) {
            *ino = rd32(b + off);
            return FS_OK;
        }
        off += rec;
    }
    return FS_NOT_FOUND;
}

static enum fs_error dir_lookup(const struct ext4_fs *fs, const struct ext4_inode *dir,
                                const char *name, uint32_t nlen, uint32_t *ino)
{
    enum fs_error e;

    if ((dir->mode & S_IFMT) != S_IFDIR)
        return FS_NOT_DIR;
    if (dir->flags & FL_ENCRYPT)
        return FS_UNSUPPORTED;
    if (dir->flags & FL_INLINE) {
        /* i_block: the parent's inode number, then 56 bytes of entries. */
        if (nlen == 2 && name[0] == '.' && name[1] == '.') {
            *ino = rd32(dir->block);
            return FS_OK;
        }
        e = scan_entries(fs, dir->block + 4, sizeof(dir->block) - 4, name, nlen, ino);
        return e == FS_NOT_FOUND && dir->size > sizeof(dir->block) ? FS_UNSUPPORTED : e;
    }
    /* Bounded so a corrupt size cannot keep the loader reading for hours. */
    if (dir->size > DIR_MAX)
        return FS_CORRUPT;
    for (uint64_t off = 0; off < dir->size; off += fs->block_size) {
        uint64_t n = dir->size - off < fs->block_size ? dir->size - off : fs->block_size;
        uint64_t pblk, count;

        if ((e = map(fs, dir, off >> fs->log_block, &pblk, &count)) != FS_OK)
            return e;
        if (!pblk) /* directories have no holes */
            return FS_CORRUPT;
        if (bdev_read(fs->dev, pblk << fs->log_block, g_dir, n))
            return FS_IO;
        if ((e = scan_entries(fs, g_dir, (uint32_t)n, name, nlen, ino)) != FS_NOT_FOUND)
            return e;
    }
    return FS_NOT_FOUND;
}

/* A symlink's target, `in->size` bytes, into dst. */
static enum fs_error read_link(const struct ext4_fs *fs, const struct ext4_inode *in, char *dst)
{
    uint32_t acl_blocks = in->file_acl ? fs->block_size / 512 : 0;

    if (in->flags & FL_ENCRYPT)
        return FS_UNSUPPORTED;
    /* Fast symlink: the target sits in i_block and the inode owns no
     * data blocks (an extended attribute block may still be counted). */
    if (in->size < sizeof(in->block) && in->blocks_lo == acl_blocks && !(in->flags & FL_INLINE)) {
        memcpy(dst, in->block, in->size);
        return FS_OK;
    }
    return read_data(fs, in, 0, (uint8_t *)dst, in->size);
}

enum fs_error ext4_open(struct ext4_fs *fs, const char *path, struct ext4_file *f)
{
    struct ext4_inode cur, next;
    uint32_t links = 0, buf = 0;
    size_t n = str_len(path);
    char *p = g_path[0];
    enum fs_error e;

    if (n >= EXT4_PATH_MAX)
        return FS_LOOP;
    memcpy(p, path, n + 1);
    if ((e = read_inode(fs, ROOT_INO, &cur)) != FS_OK)
        return e;
    for (;;) {
        const char *name;
        uint32_t nlen = 0, ino;

        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        name = p;
        while (p[nlen] && p[nlen] != '/' && p[nlen] != '\\')
            nlen++;
        p += nlen;
        if (nlen == 1 && name[0] == '.')
            continue;
        if (nlen > NAME_MAX)
            return FS_NOT_FOUND;
        if ((e = dir_lookup(fs, &cur, name, nlen, &ino)) != FS_OK ||
            (e = read_inode(fs, ino, &next)) != FS_OK)
            return e;
        if ((next.mode & S_IFMT) != S_IFLNK) {
            cur = next;
            continue;
        }

        /* Symlink: continue with target + "/" + the rest of the path, from
         * the root if the target is absolute, else from this directory. */
        char *np = g_path[buf ^ 1];
        size_t rest = str_len(p);
        if (++links > MAX_LINKS || next.size == 0 || next.size + 1 + rest >= EXT4_PATH_MAX)
            return FS_LOOP;
        if ((e = read_link(fs, &next, np)) != FS_OK)
            return e;
        np[next.size] = '/';
        memcpy(np + next.size + 1, p, rest + 1);
        buf ^= 1;
        p = np;
        if ((*p == '/' || *p == '\\') && (e = read_inode(fs, ROOT_INO, &cur)) != FS_OK)
            return e;
    }
    if ((cur.mode & S_IFMT) != S_IFREG)
        return FS_NOT_FILE;
    f->fs = fs;
    f->inode = cur;
    f->size = cur.size;
    return FS_OK;
}
