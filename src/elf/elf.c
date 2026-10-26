/*
 * ELF64 validation (System V gABI, x86-64 psABI).
 *
 * Everything in the file is untrusted. Each check below guards a later
 * operation: sizes bound the reads, address checks bound the page-table
 * mappings, and every addition is checked for wrap-around before use.
 * Section headers are never read: only PT_LOAD matters for execution.
 */
#include <stdbool.h>
#include "core/util.h"
#include "elf/elf.h"
#include "bootinfo/bootinfo.h"

#define EHDR_SIZE   64u
#define PHDR_SIZE   56u
#define PT_LOAD     1u
#define ET_EXEC     2u
#define EM_X86_64   62u

/* Elf64_Ehdr / Elf64_Phdr field offsets. */
#define E_TYPE      16
#define E_MACHINE   18
#define E_VERSION   20
#define E_ENTRY     24
#define E_PHOFF     32
#define E_EHSIZE    52
#define E_PHENTSIZE 54
#define E_PHNUM     56
#define P_TYPE      0
#define P_FLAGS     4
#define P_OFFSET    8
#define P_VADDR     16
#define P_FILESZ    32
#define P_MEMSZ     40
#define P_ALIGN     48

/* Highest address whose page-rounded end still fits in 64 bits. */
#define VADDR_END_MAX 0xFFFFFFFFFFFFF000ULL

static enum elf_error check_ident(const uint8_t *h)
{
    if (h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F')
        return ELF_ERR_MAGIC;
    if (h[4] != 2) /* ELFCLASS64 */
        return ELF_ERR_CLASS;
    if (h[5] != 1) /* ELFDATA2LSB */
        return ELF_ERR_ENDIAN;
    if (h[6] != 1 || rd32(h + E_VERSION) != 1) /* EV_CURRENT */
        return ELF_ERR_VERSION;
    if (rd16(h + E_TYPE) != ET_EXEC)
        return ELF_ERR_TYPE;
    if (rd16(h + E_MACHINE) != EM_X86_64)
        return ELF_ERR_MACHINE;
    return ELF_OK;
}

static enum elf_error add_segment(struct elf_image *img, const uint8_t *ph, uint64_t file_size)
{
    struct elf_segment s;
    uint64_t align = rd64(ph + P_ALIGN);

    s.offset = rd64(ph + P_OFFSET);
    s.vaddr = rd64(ph + P_VADDR);
    s.filesz = rd64(ph + P_FILESZ);
    s.memsz = rd64(ph + P_MEMSZ);
    s.flags = rd32(ph + P_FLAGS);

    if (s.memsz == 0)
        return ELF_OK; /* occupies nothing; ignore */
    if (s.filesz > s.memsz)
        return ELF_ERR_SEG_SIZE;
    if (s.offset > file_size || s.filesz > file_size - s.offset)
        return ELF_ERR_SEG_FILE;
    if (s.vaddr < BOOT_KERNEL_VMIN || s.memsz > VADDR_END_MAX - s.vaddr)
        return ELF_ERR_SEG_ADDR;
    if (align > 1) {
        if (!is_pow2(align) || align > ELF_MAX_ALIGN)
            return ELF_ERR_ALIGN;
        if (align > img->align)
            img->align = align;
    }
    if (img->nseg) {
        const struct elf_segment *prev = &img->seg[img->nseg - 1];
        /* The gABI requires ascending p_vaddr; overlap would make two
         * segments write the same bytes. Sharing a page is allowed. */
        if (s.vaddr < prev->vaddr + prev->memsz)
            return ELF_ERR_SEG_ORDER;
    }
    if (img->nseg == ELF_MAX_SEGMENTS)
        return ELF_ERR_TOO_MANY;
    img->seg[img->nseg++] = s;
    return ELF_OK;
}

enum elf_error elf_parse(const void *hdr, uint64_t hdr_len, uint64_t file_size,
                         struct elf_image *out)
{
    const uint8_t *h = hdr;
    uint64_t phoff;
    uint32_t phnum, i;
    enum elf_error err;
    bool entry_ok = false;

    if (hdr_len < EHDR_SIZE || file_size < EHDR_SIZE || hdr_len > file_size)
        return ELF_ERR_TRUNCATED;
    if ((err = check_ident(h)) != ELF_OK)
        return err;
    if (rd16(h + E_EHSIZE) < EHDR_SIZE || rd16(h + E_PHENTSIZE) != PHDR_SIZE)
        return ELF_ERR_PHDR;

    phoff = rd64(h + E_PHOFF);
    phnum = rd16(h + E_PHNUM);
    if (phnum == 0 || phnum > ELF_MAX_PHDRS)
        return ELF_ERR_PHDR;
    if (phoff > hdr_len || (uint64_t)phnum * PHDR_SIZE > hdr_len - phoff)
        return ELF_ERR_PHDR;

    out->nseg = 0;
    out->align = PAGE_SIZE;
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = h + phoff + (uint64_t)i * PHDR_SIZE;
        if (rd32(ph + P_TYPE) == PT_LOAD && (err = add_segment(out, ph, file_size)) != ELF_OK)
            return err;
    }
    if (out->nseg == 0)
        return ELF_ERR_NO_LOAD;

    out->vbase = align_down(out->seg[0].vaddr, PAGE_SIZE);
    out->vend = align_up(out->seg[out->nseg - 1].vaddr + out->seg[out->nseg - 1].memsz, PAGE_SIZE);
    if (out->vend - out->vbase > ELF_MAX_SPAN)
        return ELF_ERR_SPAN;

    out->entry = rd64(h + E_ENTRY);
    for (i = 0; i < out->nseg; i++) {
        const struct elf_segment *s = &out->seg[i];
        if ((s->flags & PF_X) && out->entry >= s->vaddr && out->entry - s->vaddr < s->memsz)
            entry_ok = true;
    }
    return entry_ok ? ELF_OK : ELF_ERR_ENTRY;
}

const char *elf_strerror(enum elf_error e)
{
    static const char *const msg[] = {
        [ELF_OK] = "ok",
        [ELF_ERR_TRUNCATED] = "file too small",
        [ELF_ERR_MAGIC] = "not an ELF file",
        [ELF_ERR_CLASS] = "not ELF64",
        [ELF_ERR_ENDIAN] = "not little-endian",
        [ELF_ERR_VERSION] = "bad ELF version",
        [ELF_ERR_TYPE] = "not ET_EXEC",
        [ELF_ERR_MACHINE] = "not x86-64",
        [ELF_ERR_PHDR] = "bad program headers",
        [ELF_ERR_NO_LOAD] = "no PT_LOAD segments",
        [ELF_ERR_TOO_MANY] = "too many PT_LOAD segments",
        [ELF_ERR_SEG_SIZE] = "segment filesz > memsz",
        [ELF_ERR_SEG_FILE] = "segment outside file",
        [ELF_ERR_SEG_ADDR] = "segment below 0xFFFFFFFF80000000 or wraps",
        [ELF_ERR_SEG_ORDER] = "segments unsorted or overlapping",
        [ELF_ERR_ALIGN] = "unsupported p_align",
        [ELF_ERR_SPAN] = "image span over 1 GiB",
        [ELF_ERR_ENTRY] = "entry not in an executable segment",
    };
    return (unsigned)e < sizeof(msg) / sizeof(msg[0]) ? msg[e] : "unknown";
}
