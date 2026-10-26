/*
 * ELF64 kernel image validation. Firmware-independent and allocation-free:
 * it parses headers already in memory and produces a load plan; the backend
 * performs the reads into the final location.
 */
#ifndef ELF_ELF_H
#define ELF_ELF_H

#include <stddef.h>
#include <stdint.h>
#include "core/util.h"

#define ELF_MAX_SEGMENTS 16          /* PT_LOAD entries we accept */
#define ELF_MAX_PHDRS    64          /* program headers of any type */
#define ELF_MAX_SPAN     GiB(1)      /* sanity limit on the image span */
#define ELF_MAX_ALIGN    (2u << 20)  /* largest p_align we honour physically */

#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

struct elf_segment {
    uint64_t vaddr;
    uint64_t memsz;
    uint64_t offset;
    uint64_t filesz;
    uint32_t flags; /* PF_* */
};

struct elf_image {
    uint64_t entry;
    uint64_t vbase;  /* lowest PT_LOAD vaddr, page-aligned down */
    uint64_t vend;   /* highest PT_LOAD end, page-aligned up */
    uint64_t align;  /* physical alignment to honour, 4 KiB .. 2 MiB */
    uint32_t nseg;
    struct elf_segment seg[ELF_MAX_SEGMENTS]; /* sorted by vaddr */
};

enum elf_error {
    ELF_OK = 0,
    ELF_ERR_TRUNCATED,
    ELF_ERR_MAGIC,
    ELF_ERR_CLASS,
    ELF_ERR_ENDIAN,
    ELF_ERR_VERSION,
    ELF_ERR_TYPE,
    ELF_ERR_MACHINE,
    ELF_ERR_PHDR,
    ELF_ERR_NO_LOAD,
    ELF_ERR_TOO_MANY,
    ELF_ERR_SEG_SIZE,
    ELF_ERR_SEG_FILE,
    ELF_ERR_SEG_ADDR,
    ELF_ERR_SEG_ORDER,
    ELF_ERR_ALIGN,
    ELF_ERR_SPAN,
    ELF_ERR_ENTRY
};

/*
 * hdr/hdr_len: the first bytes of the file (all program headers must lie
 * inside them). file_size: total file size, for bounding segment reads.
 */
enum elf_error elf_parse(const void *hdr, uint64_t hdr_len, uint64_t file_size,
                         struct elf_image *out);
const char *elf_strerror(enum elf_error e);

#endif
