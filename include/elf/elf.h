/* ELF64 kernel image validation. */
#ifndef ELF_ELF_H
#define ELF_ELF_H

#include <stddef.h>
#include <stdint.h>
#include "core/util.h"

#define ELF_MAX_SEGMENTS 16
#define ELF_MAX_PHDRS    64
#define ELF_MAX_SPAN     GiB(1)
#define ELF_MAX_ALIGN    (2u << 20)

#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

struct elf_segment {
    uint64_t vaddr;
    uint64_t memsz;
    uint64_t offset;
    uint64_t filesz;
    uint32_t flags;
};

struct elf_image {
    uint64_t entry;
    uint64_t vbase;
    uint64_t vend;
    uint64_t align;
    uint32_t nseg;
    struct elf_segment seg[ELF_MAX_SEGMENTS];
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

enum elf_error elf_parse(const void *hdr, uint64_t hdr_len, uint64_t file_size,
                         struct elf_image *out);
const char *elf_strerror(enum elf_error e);

#endif
