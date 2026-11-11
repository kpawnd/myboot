/* A readable disk or partition, as filesystem drivers see it. */
#ifndef FS_BLOCKDEV_H
#define FS_BLOCKDEV_H

#include <stdint.h>

struct blockdev {
    int (*read)(struct blockdev *dev, uint64_t offset, void *dst, uint64_t len);
    uint64_t size;
    uint64_t reads, bytes;
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
