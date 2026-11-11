/* x86-64 page-table builder (Intel SDM vol. */
#ifndef ARCH_X86_64_PAGING_H
#define ARCH_X86_64_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#define PT_W  (1u << 0)
#define PT_X  (1u << 1)
#define PT_WC (1u << 2)

struct pt_builder {
    uint64_t *root;
    uint64_t *pml4;
    uint8_t  *pool;
    uint32_t  used, cap;
    bool      la57, nx;
};

bool pt_init(struct pt_builder *b, void *pool, uint32_t pool_pages, bool la57, bool nx);

bool pt_map(struct pt_builder *b, uint64_t virt, uint64_t phys, uint64_t size,
            uint32_t prot, uint64_t max_page);

uint32_t pt_pages_bound(uint64_t kernel_span, uint64_t hhdm_size, bool hhdm_1g, bool la57,
                        uint64_t fb_size);

#endif
