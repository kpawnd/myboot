/*
 * Bump allocator over one contiguous block. Never frees; the whole block is
 * handed to the kernel (as BOOTLOADER_RECLAIMABLE) or released at once.
 * Addresses are physical; before paging changes they are also usable as
 * pointers because firmware identity-maps memory.
 */
#ifndef CORE_ARENA_H
#define CORE_ARENA_H

#include <stdint.h>

struct arena {
    uint64_t base;
    uint64_t size;
    uint64_t used; /* high-water mark == current offset (no frees) */
};

/* Returns NULL when the request does not fit. `align` is a power of two. */
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
