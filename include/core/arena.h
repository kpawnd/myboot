/* Bump allocator over one contiguous block. */
#ifndef CORE_ARENA_H
#define CORE_ARENA_H

#include <stdint.h>

struct arena {
    uint64_t base;
    uint64_t size;
    uint64_t used;
};

static inline void *arena_alloc(struct arena *a, uint64_t size, uint64_t align)
{
    uint64_t end = a->base + a->size;
    uint64_t p = (a->base + a->used + align - 1) & ~(align - 1);

    if (p < a->base || p > end || size > end - p)
        return 0;
    a->used = p + size - a->base;
    return (void *)(uintptr_t)p;
}

#endif
