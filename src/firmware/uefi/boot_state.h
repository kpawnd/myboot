/*
 * Everything the UEFI boot sequence accumulates, in one static object
 * (loader scratch: .bss of the loader image, reclaimable after handoff).
 * Shared between entry.c (which fills it) and report.c (which prints it).
 */
#ifndef UEFI_BOOT_STATE_H
#define UEFI_BOOT_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "firmware/uefi_boot.h"
#include "elf/elf.h"
#include "core/arena.h"
#include "arch/x86_64/paging.h"

#define LOADER_VERSION   "0.1.0"
#define LOADER_VERSION_W L"0.1.0"
#define KERNEL_PATH L"\\EFI\\MYBOOT\\KERNEL.ELF"
#define KERNEL_STACK_SIZE KiB(64)

struct boot_state {
    /* discovery results (owned by their discover_* caches) */
    const struct boot_cpu         *cpu;
    struct uefi_memory            *mem;
    const struct acpi_info        *acpi;
    const struct smbios_info      *smbios;
    const struct boot_framebuffer *fb;
    const struct boot_device      *dev;

    /* kernel image: physically contiguous, [kphys, kphys + span) */
    struct kfile     kf;
    struct elf_image elf;
    uint64_t         kphys;
    uint64_t         kpages;

    /* handoff arena and what lives in it */
    struct arena           arena;
    uint64_t               arena_pages;
    struct pt_builder      pt;
    uint32_t               pt_bound;
    uint64_t               hhdm_size;
    bool                   hhdm_1g, la57, nx;
    struct boot_info      *bi;
    struct boot_mem_entry *mmap;
    uint32_t               mmap_cap;
    uint32_t               cmdline_len;
    uint64_t               gdtr_virt;
    uint64_t               stack_top_virt;

    uint32_t ebs_attempts;
};

void report_summary(const struct boot_state *s);

#endif
