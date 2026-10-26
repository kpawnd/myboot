/*
 * Firmware-independent memory map post-processing. Runs after firmware has
 * been exited, so it allocates nothing and cannot fail.
 */
#ifndef MEMORY_MEMMAP_H
#define MEMORY_MEMMAP_H

#include <stdint.h>
#include "bootinfo/bootinfo.h"

/*
 * Sort by base, resolve overlaps conservatively (memory that any entry calls
 * non-usable is never reported usable) and merge adjacent entries of equal
 * type and flags. In place; returns the new count.
 */
uint32_t mmap_sort_merge(struct boot_mem_entry *e, uint32_t n);

#endif
