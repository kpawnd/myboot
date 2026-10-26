/*
 * Page-size trade-off used by the boot protocol:
 *   4 KiB  kernel segments: exact W^X per segment; one 4 KiB table per 2 MiB.
 *   2 MiB  HHDM fallback: one 4 KiB table per GiB mapped.
 *   1 GiB  HHDM when CPUID reports PDPE1GB: one 4 KiB table per 512 GiB.
 * Large pages also cut TLB misses for the kernel's early HHDM accesses.
 */
#include "arch/x86_64/paging.h"
#include "core/util.h"
#include "lib/mem.h"

#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_PCD  (1ull << 4)  /* same bit at every level, unlike the PAT bit */
#define PTE_PS   (1ull << 7)  /* large page at PDPT (1 GiB) / PD (2 MiB) level */
#define PTE_NX   (1ull << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ull

static uint64_t *pt_alloc(struct pt_builder *b)
{
    uint64_t *t;

    if (b->used == b->cap)
        return NULL;
    t = (uint64_t *)(b->pool + (uint64_t)b->used++ * PAGE_SIZE);
    memset(t, 0, PAGE_SIZE);
    return t;
}

/* Return the table referenced by t[idx], creating it if absent. Intermediate
 * entries are present+writable and never NX: leaf entries decide access. */
static uint64_t *pt_next(struct pt_builder *b, uint64_t *t, unsigned idx)
{
    uint64_t e = t[idx];
    uint64_t *n;

    if (e & PTE_P)
        return (e & PTE_PS) ? NULL : (uint64_t *)(uintptr_t)(e & PTE_ADDR);
    if (!(n = pt_alloc(b)))
        return NULL;
    t[idx] = (uint64_t)(uintptr_t)n | PTE_P | PTE_W;
    return n;
}

bool pt_init(struct pt_builder *b, void *pool, uint32_t pool_pages, bool la57, bool nx)
{
    b->pool = pool;
    b->cap = pool_pages;
    b->used = 0;
    b->la57 = la57;
    b->nx = nx;
    if (!(b->pml4 = pt_alloc(b)))
        return false;
    b->root = b->pml4;
    if (la57) {
        /* With 5-level paging only PML5 slots 0 (low canonical half) and
         * 511 (high half) are used, and both lead to the same PML4: bits
         * 47:39 then index it exactly as in 4-level mode. */
        if (!(b->root = pt_alloc(b)))
            return false;
        b->root[0] = b->root[511] = (uint64_t)(uintptr_t)b->pml4 | PTE_P | PTE_W;
    }
    return true;
}

bool pt_map(struct pt_builder *b, uint64_t virt, uint64_t phys, uint64_t size,
            uint32_t prot, uint64_t max_page)
{
    uint64_t leaf = PTE_P | ((prot & PT_W) ? PTE_W : 0) | ((prot & PT_UC) ? PTE_PCD : 0) |
                    ((b->nx && !(prot & PT_X)) ? PTE_NX : 0);

    if ((virt | phys | size) & (PAGE_SIZE - 1))
        return false;
    while (size) {
        uint64_t *pdpt, *pd, *pt, *e;

        if (!(pdpt = pt_next(b, b->pml4, (virt >> 39) & 511)))
            return false;
        if (max_page >= GiB(1) && !((virt | phys) & (GiB(1) - 1)) && size >= GiB(1)) {
            e = &pdpt[(virt >> 30) & 511];
            if (*e & PTE_P)
                return false;
            *e = phys | leaf | PTE_PS;
            virt += GiB(1), phys += GiB(1), size -= GiB(1);
            continue;
        }
        if (!(pd = pt_next(b, pdpt, (virt >> 30) & 511)))
            return false;
        if (max_page >= MiB(2) && !((virt | phys) & (MiB(2) - 1)) && size >= MiB(2)) {
            e = &pd[(virt >> 21) & 511];
            if (*e & PTE_P)
                return false;
            *e = phys | leaf | PTE_PS;
            virt += MiB(2), phys += MiB(2), size -= MiB(2);
            continue;
        }
        if (!(pt = pt_next(b, pd, (virt >> 21) & 511)))
            return false;
        e = &pt[(virt >> 12) & 511];
        if (*e & PTE_P) {
            /* A page shared by two segments (one ends, the next begins in
             * the same 4 KiB): union of the permissions. */
            if ((*e & PTE_ADDR) != phys)
                return false;
            *e |= leaf & PTE_W;
            if (!(leaf & PTE_NX))
                *e &= ~PTE_NX;
        } else {
            *e = phys | leaf;
        }
        virt += PAGE_SIZE, phys += PAGE_SIZE, size -= PAGE_SIZE;
    }
    return true;
}

uint32_t pt_pages_bound(uint64_t kernel_span, uint64_t hhdm_size, bool hhdm_1g, bool la57,
                        uint64_t fb_size)
{
    uint64_t n = 1 + (la57 ? 1 : 0);                  /* PML4 (+ PML5) */

    n += 6;                                           /* identity window: 2 x (PDPT, PD, PT) */
    n += (hhdm_size + GiB(512) - 1) / GiB(512);       /* HHDM PDPTs */
    if (!hhdm_1g)
        n += (hhdm_size + GiB(1) - 1) / GiB(1);       /* HHDM PDs */
    n += 1 + 2 + kernel_span / MiB(2) + 2;            /* kernel PDPT, PDs, PTs */
    if (fb_size)                                      /* framebuffer beyond the HHDM */
        n += 2 + fb_size / GiB(1) + 2 + 2;            /* PDPTs, PDs, head/tail PTs */
    return (uint32_t)n;
}
