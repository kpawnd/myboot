/*
 * A readable disk or partition, as filesystem drivers see it. Reads take
 * byte offsets and lengths with no alignment requirement; the backend
 * (UEFI Disk I/O, later BIOS INT 13h) deals with sectors and bounce
 * buffers. Filesystem code depends only on this, so it is shared by every
 * firmware backend.
 */
#ifndef FS_BLOCKDEV_H
#define FS_BLOCKDEV_H

#include <stdint.h>

struct blockdev {
    /* 0 on success; any other value is an I/O error. */
    int (*read)(struct blockdev *dev, uint64_t offset, void *dst, uint64_t len);
    uint64_t size; /* bytes */
    uint64_t reads, bytes; /* counted by bdev_read */
};

static inline int bdev_read(struct blockdev *dev, uint64_t offset, void *dst, uint64_t len)
{
    if (offset > dev->size || len > dev->size - offset)
        return -1;
    dev->reads++;
    dev->bytes += len;
    return dev->read(dev, offset, dst, len);
}

#endif
