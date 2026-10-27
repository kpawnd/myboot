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
#include "config/config.h"

#define LOADER_VERSION   "0.3.0"
#define LOADER_VERSION_W L"0.3.0"
#define CONFIG_PATH      CFG_DIR "BOOT.CFG"
#define DEFAULT_KERNEL   CFG_DIR "KERNEL.ELF" /* used when there is no BOOT.CFG */
#define KERNEL_STACK_SIZE KiB(64)

struct loaded_module {
    uint64_t    phys;   /* 0 for an empty file */
    uint64_t    pages;
    uint64_t    size;
    const char *string; /* points into the parsed config */
};

struct boot_state {
    /* discovery results (owned by their discover_* caches) */
    const struct boot_cpu         *cpu;
    struct uefi_memory            *mem;
    const struct acpi_info        *acpi;
    const struct smbios_info      *smbios;
    const struct boot_framebuffer *fb;
    const struct boot_device      *dev;

    /* configuration and the entry being loaded */
    const struct boot_config *cfg;
    bool                      have_cfg; /* false: built-in default entry */
    uint32_t                  entry;    /* index into cfg->entry */
    const char               *cmdline;  /* ASCII, cmdline_len bytes, not terminated */
    uint32_t                  cmdline_len;

    /* kernel image: physically contiguous, [kphys, kphys + span) */
    char             kpath[CFG_RESOLVED_MAX];
    struct kfile     kf;
    struct elf_image elf;
    uint64_t         kphys;
    uint64_t         kpages;

    /* modules: one allocation each */
    struct loaded_module mods[CFG_MAX_MODULES];
    uint32_t             nmod;
    uint64_t             mod_bytes;

    /* typed ranges inside our allocations, for the kernel's memory map */
    struct mmap_carve carve[1 + CFG_MAX_MODULES];
    uint32_t          ncarve;

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
    struct boot_module    *bmods;
    char                  *log; /* CON_LOG_MAX bytes, filled just before exit */
    uint64_t               gdtr_virt;
    uint64_t               stack_top_virt;

    uint32_t ebs_attempts;
};

/* report.c, BOOT_LOG builds: status lines for what discovery found, and
 * for the prepared handoff. */
void report_discovery(const struct boot_state *s);
void report_handoff(const struct boot_state *s);

#endif
