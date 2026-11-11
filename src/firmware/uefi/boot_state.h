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
#define DEFAULT_KERNEL   CFG_DIR "KERNEL.ELF"
#define KERNEL_STACK_SIZE KiB(64)

struct loaded_module {
    uint64_t    phys;
    uint64_t    pages;
    uint64_t    size;
    const char *string;
};

struct boot_state {
    const struct boot_cpu         *cpu;
    struct uefi_memory            *mem;
    const struct acpi_info        *acpi;
    const struct smbios_info      *smbios;
    const struct boot_framebuffer *fb;
    const struct boot_device      *dev;
    const struct boot_csme        *csme;
    const struct csme_clients     *csme_clients;
    const struct smp_cpus         *smp;

    const struct boot_config *cfg;
    bool                      have_cfg;
    uint32_t                  entry;
    const char               *cmdline;
    uint32_t                  cmdline_len;

    char             kpath[CFG_RESOLVED_MAX];
    struct kfile     kf;
    struct elf_image elf;
    uint64_t         kphys;
    uint64_t         kpages;

    struct loaded_module mods[CFG_MAX_MODULES];
    uint32_t             nmod;
    uint64_t             mod_bytes;

    struct mmap_carve carve[1 + CFG_MAX_MODULES];
    uint32_t          ncarve;

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
    char                  *log;
    uint64_t               gdtr_virt;
    uint64_t               stack_top_virt;

    uint64_t cr0, cr4, xcr0;
    uint64_t             tramp_page;
    bool                 nosmp;
    uint16_t             smpstop;
    struct boot_smp_cpu *smp_cpus;
    uint32_t             smp_count;

    uint32_t ebs_attempts;
};

void report_discovery(const struct boot_state *s);
void report_handoff(const struct boot_state *s);

#endif
