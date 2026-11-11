/* Firmware-independent memory map post-processing. */
#ifndef MEMORY_MEMMAP_H
#define MEMORY_MEMMAP_H

#include <stdint.h>
#include "bootinfo/bootinfo.h"

uint32_t mmap_sort_merge(struct boot_mem_entry *e, uint32_t n);

#endif
