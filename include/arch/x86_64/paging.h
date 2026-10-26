/*
 * x86-64 page-table builder (Intel SDM vol. 3A ch. 4.5, 4-level and 5-level
 * paging). Tables come from a caller-provided pool sized with
 * pt_pages_bound(), so building can never allocate or fail for lack of
 * memory once the bound is honoured.
 *
 * Physical addresses must be directly dereferenceable while building
 * (true under UEFI identity mapping and in the BIOS stage's identity map).
 */
#ifndef ARCH_X86_64_PAGING_H
#define ARCH_X86_64_PAGING_H

#include <stdbool.h>
#include <stdint.h>

/* Protection requested for a mapping; read access is implied. */
#define PT_W  (1u << 0)
#define PT_X  (1u << 1)
/* Uncached-minus: sets PCD, which selects PAT entry 2 (UC-) in the
 * power-on PAT. With the firmware's MTRRs this yields UC for MMIO, or WC
 * where firmware set a WC MTRR. Used for the framebuffer. */
#define PT_UC (1u << 2)

struct pt_builder {
    uint64_t *root;  /* PML5 when la57, else PML4: what CR3 points to */
    uint64_t *pml4;
    uint8_t  *pool;
    uint32_t  used, cap; /* pages */
    bool      la57, nx;
};

bool pt_init(struct pt_builder *b, void *pool, uint32_t pool_pages, bool la57, bool nx);

/*
 * Map [virt, virt+size) to [phys, phys+size). All three must be 4 KiB
 * aligned. Uses pages up to `max_page` (4 KiB, 2 MiB or 1 GiB) wherever
 * alignment and remaining size allow. 4 KiB pages mapped twice to the same
 * frame merge permissions (writable if either is, executable if either is).
 */
bool pt_map(struct pt_builder *b, uint64_t virt, uint64_t phys, uint64_t size,
            uint32_t prot, uint64_t max_page);

/* Upper bound on table pages for the boot protocol's mappings. */
uint32_t pt_pages_bound(uint64_t kernel_span, uint64_t hhdm_size, bool hhdm_1g, bool la57,
                        uint64_t fb_size);

#endif
