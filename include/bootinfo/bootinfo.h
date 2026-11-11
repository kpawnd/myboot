/* MYBOOT boot protocol, version 3: the contract between loader and kernel. */
#ifndef BOOTINFO_BOOTINFO_H
#define BOOTINFO_BOOTINFO_H

#include <stddef.h>
#include <stdint.h>

#define BOOT_INFO_MAGIC   0x4942544F4F42594DULL
#define BOOT_INFO_VERSION 3u

#define BOOT_HHDM_BASE    0xFFFF800000000000ULL
#define BOOT_KERNEL_VMIN  0xFFFFFFFF80000000ULL

#define BOOT_F_UEFI    (1u << 0)
#define BOOT_F_LA57    (1u << 1)
#define BOOT_F_NX      (1u << 2)
#define BOOT_F_HHDM_1G (1u << 3)
#define BOOT_F_FB_WC   (1u << 4)

enum boot_mem_type {
    BOOT_MEM_USABLE = 1,
    BOOT_MEM_RESERVED = 2,
    BOOT_MEM_ACPI_RECLAIMABLE = 3,
    BOOT_MEM_ACPI_NVS = 4,
    BOOT_MEM_BAD = 5,
    BOOT_MEM_BOOTLOADER_RECLAIMABLE = 6,
    BOOT_MEM_KERNEL = 7,
    BOOT_MEM_FIRMWARE_RUNTIME = 8,
    BOOT_MEM_MMIO = 9,
    BOOT_MEM_PERSISTENT = 10,
    BOOT_MEM_MODULE = 11
};

#define BOOT_MEM_F_NONVOLATILE      (1u << 0)
#define BOOT_MEM_F_MORE_RELIABLE    (1u << 1)
#define BOOT_MEM_F_SPECIFIC_PURPOSE (1u << 2)

struct boot_mem_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t flags;
};

#define BOOT_CPU_FPU           (1ull << 0)
#define BOOT_CPU_TSC           (1ull << 1)
#define BOOT_CPU_APIC          (1ull << 2)
#define BOOT_CPU_X2APIC        (1ull << 3)
#define BOOT_CPU_INVARIANT_TSC (1ull << 4)
#define BOOT_CPU_NX            (1ull << 5)
#define BOOT_CPU_PDPE1GB       (1ull << 6)
#define BOOT_CPU_LA57          (1ull << 7)
#define BOOT_CPU_SSE2          (1ull << 8)
#define BOOT_CPU_SSE3          (1ull << 9)
#define BOOT_CPU_SSSE3         (1ull << 10)
#define BOOT_CPU_SSE41         (1ull << 11)
#define BOOT_CPU_SSE42         (1ull << 12)
#define BOOT_CPU_POPCNT        (1ull << 13)
#define BOOT_CPU_AES           (1ull << 14)
#define BOOT_CPU_XSAVE         (1ull << 15)
#define BOOT_CPU_AVX           (1ull << 16)
#define BOOT_CPU_AVX2          (1ull << 17)
#define BOOT_CPU_AVX512F       (1ull << 18)
#define BOOT_CPU_BMI1          (1ull << 19)
#define BOOT_CPU_BMI2          (1ull << 20)
#define BOOT_CPU_ERMS          (1ull << 21)
#define BOOT_CPU_RDRAND        (1ull << 22)
#define BOOT_CPU_RDTSCP        (1ull << 23)
#define BOOT_CPU_SHA           (1ull << 24)
#define BOOT_CPU_HYPERVISOR    (1ull << 25)

enum boot_tsc_source {
    BOOT_TSC_UNKNOWN = 0,
    BOOT_TSC_CPUID_15H = 1,
    BOOT_TSC_CPUID_16H = 2,
    BOOT_TSC_HYPERVISOR = 3,
    BOOT_TSC_CALIBRATED = 4
};

struct boot_cpu {
    char     vendor[12];
    char     hypervisor[12];
    char     brand[48];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t bsp_apic_id;
    uint64_t features;
    uint64_t tsc_hz;
    uint32_t tsc_source;
    uint16_t threads_per_core;
    uint16_t logical_per_package;
    uint8_t  phys_addr_bits;
    uint8_t  virt_addr_bits;
    uint8_t  reserved[6];
};

struct boot_framebuffer {
    uint64_t base;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint8_t  red_shift, red_size;
    uint8_t  green_shift, green_size;
    uint8_t  blue_shift, blue_size;
    uint8_t  reserved[2];
};

enum boot_dev_interface {
    BOOT_DEV_UNKNOWN = 0,
    BOOT_DEV_NVME = 1,
    BOOT_DEV_SATA = 2,
    BOOT_DEV_USB = 3,
    BOOT_DEV_SCSI = 4,
    BOOT_DEV_ATAPI = 5,
    BOOT_DEV_SD = 6,
    BOOT_DEV_EMMC = 7
};

enum boot_part_scheme { BOOT_PART_NONE = 0, BOOT_PART_MBR = 1, BOOT_PART_GPT = 2 };

struct boot_device {
    uint32_t interface;
    uint8_t  partition_scheme;
    uint8_t  removable;
    uint16_t reserved;
    uint32_t partition_number;
    uint32_t block_size;
    uint64_t partition_start_lba;
    uint64_t partition_lba_count;
    uint8_t  partition_guid[16];
};

#define BOOT_CSME_PRESENT       (1u << 0)
#define BOOT_CSME_INIT_COMPLETE (1u << 1)
#define BOOT_CSME_VERSION_VALID (1u << 2)

enum boot_csme_status {
    BOOT_CSME_ST_NONE = 0,
    BOOT_CSME_ST_OK = 1,
    BOOT_CSME_ST_NOT_NORMAL = 2,
    BOOT_CSME_ST_NO_BAR = 3,
    BOOT_CSME_ST_NOT_READY = 4,
    BOOT_CSME_ST_BUSY = 5,
    BOOT_CSME_ST_TIMEOUT = 6,
    BOOT_CSME_ST_BAD_REPLY = 7,
    BOOT_CSME_ST_REFUSED = 8
};

struct boot_csme {
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t flags;
    uint32_t hfsts[6];
    uint16_t fw_major;
    uint16_t fw_minor;
    uint16_t fw_hotfix;
    uint16_t fw_build;
    uint8_t  revision_id;
    uint8_t  working_state;
    uint8_t  op_mode;
    uint8_t  error_code;
    uint8_t  version_status;
    uint8_t  hbm_status;
    uint8_t  mkhi_status;
    uint8_t  client_count;
};

enum boot_smp_state {
    BOOT_SMP_OFF = 0,
    BOOT_SMP_BSP = 1,
    BOOT_SMP_PARKED = 2
};

#define BOOT_SMP_F_X2APIC (1u << 0)

struct boot_smp_cpu {
    uint32_t apic_id;
    uint32_t acpi_uid;
    uint32_t state;
    uint32_t reserved;
    uint64_t stack_top;
    uint64_t goto_address;
    uint64_t extra_argument;
    uint64_t reserved2[3];
};

#define BOOT_MODULE_STRING_MAX 111
struct boot_module {
    uint64_t base;
    uint64_t size;
    char     string[112];
};

struct boot_info {
    uint64_t magic;
    uint32_t version;
    uint32_t size;
    uint64_t flags;
    uint64_t hhdm_base;
    uint64_t hhdm_size;
    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;
    uint64_t kernel_size;
    uint64_t mmap;
    uint32_t mmap_count;
    uint32_t mmap_entry_size;
    uint64_t cmdline;
    uint64_t acpi_rsdp;
    uint64_t smbios_entry;
    uint64_t uefi_system_table;
    uint8_t  acpi_revision;
    uint8_t  smbios_major;
    uint8_t  smbios_minor;
    uint8_t  firmware_type;
    uint32_t uefi_revision;
    uint32_t firmware_revision;
    uint32_t loader_reads;
    uint64_t loader_bytes_read;
    uint64_t tsc_loader_entry;
    uint64_t tsc_exit_start;
    uint64_t tsc_handoff;
    char     firmware_vendor[32];
    char     loader_version[16];
    struct boot_cpu         cpu;
    struct boot_framebuffer framebuffer;
    struct boot_device      boot_device;
    uint64_t modules;
    uint32_t module_count;
    uint32_t module_entry_size;
    uint64_t efi_mmap;
    uint64_t efi_mmap_size;
    uint32_t efi_mmap_desc_size;
    uint32_t efi_mmap_desc_version;
    uint32_t boot_entry;
    uint32_t reserved;
    char     boot_entry_title[64];
    uint64_t loader_log;
    uint32_t loader_log_size;
    uint32_t reserved2;
    struct boot_csme        csme;
    uint64_t pat;
    uint64_t xcr0;
    uint32_t xsave_size;
    uint32_t smp_count;
    uint64_t smp_cpus;
    uint32_t smp_parked;
    uint32_t smp_flags;
};

_Static_assert(sizeof(struct boot_mem_entry) == 24, "boot_mem_entry");
_Static_assert(sizeof(struct boot_cpu) == 120, "boot_cpu");
_Static_assert(offsetof(struct boot_cpu, features) == 88, "boot_cpu.features");
_Static_assert(offsetof(struct boot_cpu, phys_addr_bits) == 112, "boot_cpu.phys_addr_bits");
_Static_assert(sizeof(struct boot_framebuffer) == 40, "boot_framebuffer");
_Static_assert(sizeof(struct boot_device) == 48, "boot_device");
_Static_assert(offsetof(struct boot_info, hhdm_base) == 24, "boot_info.hhdm_base");
_Static_assert(offsetof(struct boot_info, mmap) == 64, "boot_info.mmap");
_Static_assert(offsetof(struct boot_info, acpi_rsdp) == 88, "boot_info.acpi_rsdp");
_Static_assert(offsetof(struct boot_info, acpi_revision) == 112, "boot_info.acpi_revision");
_Static_assert(offsetof(struct boot_info, loader_bytes_read) == 128, "boot_info.loader_bytes_read");
_Static_assert(offsetof(struct boot_info, tsc_handoff) == 152, "boot_info.tsc_handoff");
_Static_assert(offsetof(struct boot_info, firmware_vendor) == 160, "boot_info.firmware_vendor");
_Static_assert(offsetof(struct boot_info, cpu) == 208, "boot_info.cpu");
_Static_assert(offsetof(struct boot_info, framebuffer) == 328, "boot_info.framebuffer");
_Static_assert(offsetof(struct boot_info, boot_device) == 368, "boot_info.boot_device");
_Static_assert(sizeof(struct boot_module) == 128, "boot_module");
_Static_assert(offsetof(struct boot_info, modules) == 416, "boot_info.modules");
_Static_assert(offsetof(struct boot_info, efi_mmap) == 432, "boot_info.efi_mmap");
_Static_assert(offsetof(struct boot_info, boot_entry) == 456, "boot_info.boot_entry");
_Static_assert(offsetof(struct boot_info, loader_log) == 528, "boot_info.loader_log");
_Static_assert(sizeof(struct boot_csme) == 48, "boot_csme");
_Static_assert(offsetof(struct boot_csme, fw_major) == 32, "boot_csme.fw_major");
_Static_assert(offsetof(struct boot_info, csme) == 544, "boot_info.csme");
_Static_assert(sizeof(struct boot_smp_cpu) == 64, "boot_smp_cpu");
_Static_assert(offsetof(struct boot_smp_cpu, stack_top) == 16, "boot_smp_cpu.stack_top");
_Static_assert(offsetof(struct boot_smp_cpu, goto_address) == 24, "boot_smp_cpu.goto_address");
_Static_assert(offsetof(struct boot_info, pat) == 592, "boot_info.pat");
_Static_assert(offsetof(struct boot_info, smp_cpus) == 616, "boot_info.smp_cpus");
_Static_assert(sizeof(struct boot_info) == 632, "boot_info");

#endif
