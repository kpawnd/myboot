#include "memory/memmap.h"

static void sort_by_base(struct boot_mem_entry *e, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        struct boot_mem_entry t = e[i];
        uint32_t j = i;
        while (j > 0 && e[j - 1].base > t.base) {
            e[j] = e[j - 1];
            j--;
        }
        e[j] = t;
    }
}

uint32_t mmap_sort_merge(struct boot_mem_entry *e, uint32_t n)
{
    uint32_t out = 0;

    sort_by_base(e, n);
    for (uint32_t i = 0; i < n; i++) {
        struct boot_mem_entry cur = e[i];

        if (cur.length == 0)
            continue;
        if (out > 0) {
            struct boot_mem_entry *prev = &e[out - 1];
            uint64_t prev_end = prev->base + prev->length;

            if (cur.base < prev_end) {
                if (prev->type == BOOT_MEM_USABLE && cur.type != BOOT_MEM_USABLE) {
                    prev->length = cur.base - prev->base;
                    if (prev->length == 0)
                        out--;
                } else {
                    uint64_t cur_end = cur.base + cur.length;
                    if (cur_end <= prev_end)
                        continue;
                    cur.length = cur_end - prev_end;
                    cur.base = prev_end;
                }
            }
        }
        if (out > 0) {
            struct boot_mem_entry *prev = &e[out - 1];
            if (prev->type == cur.type && prev->flags == cur.flags &&
                prev->base + prev->length == cur.base) {
                prev->length += cur.length;
                continue;
            }
        }
        e[out++] = cur;
    }
    return out;
}
