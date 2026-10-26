/*
 * MYBOOT boot protocol, version 1: the contract between loader and kernel.
 *
 * This header is shared by the loader and the kernel and is the only
 * definition of the ABI. Every field has an explicit width and a fixed
 * offset that is verified below, so the layout does not depend on compiler
 * packing rules. Little-endian throughout (x86-64).
 *
 * Kernel requirements
 *   ELF64, ET_EXEC, EM_X86_64, every PT_LOAD at a virtual address
 *   >= BOOT_KERNEL_VMIN (the top 2 GiB; link with -mcmodel=kernel).
 *   p_paddr is ignored: the loader chooses the physical location, keeping
 *   the whole image physically contiguous (phys = virt - kernel_virt_base +
 *   kernel_phys_base) and honouring p_align up to 2 MiB.
 *   File placed at \EFI\MYBOOT\KERNEL.ELF on the boot volume.
 *
 * Machine state at kernel entry
 *   64-bit long mode, CPL 0, interrupts disabled, no IDT loaded (install one
 *   before enabling interrupts).
 *   CR0.PG, CR0.WP and CR4.PAE set; EFER.NXE set iff BOOT_F_NX.
 *   CR4.LA57 is left as firmware set it; BOOT_F_LA57 says which.
 *   GDT: 0x08 = 64-bit code, 0x10 = data; CS = 0x08, DS/ES/SS/FS/GS = 0x10.
 *   RDI = virtual address of struct boot_info (inside the HHDM).
 *   RSP = top of a 64 KiB stack minus 8 (aligned as just after a CALL);
 *   [RSP] = 0 as a fake return address. RBP = 0.
 *   SSE/x87 state: untouched, initialise it yourself.
 *
 * Address space at kernel entry
 *   Kernel PT_LOAD segments at their p_vaddr with 4 KiB pages:
 *     writable iff PF_W, executable iff PF_X (NX used when BOOT_F_NX).
 *   HHDM: physical [0, hhdm_size) mapped read/write, non-executable, at
 *     hhdm_base, using 1 GiB pages (BOOT_F_HHDM_1G) or 2 MiB pages.
 *     hhdm_size reaches the end of the highest RAM-backed entry (usable,
 *     reclaimable, ACPI, runtime, kernel, persistent), rounded up to the
 *     page size used. Reserved and MMIO ranges below that point are
 *     covered incidentally; above it (PCI holes, framebuffers, high
 *     reserved windows) they are not mapped. Map those yourself.
 *     Cacheability comes from the firmware's MTRRs.
 *   Framebuffer (if present): mapped read/write, non-executable, at
 *     hhdm_base + framebuffer.base, even when above hhdm_size. The part
 *     beyond hhdm_size is uncached-minus (PCD set; UC, or WC if a firmware
 *     MTRR says so). Remap it write-combining for speed.
 *   One identity-mapped executable 8 KiB window holding the loader's
 *   handoff code. Nothing else. The kernel should build its own tables.
 *
 * Pointers inside boot_info are PHYSICAL addresses (0 = absent); add
 * hhdm_base to dereference them. All referenced memory lies in entries of
 * type BOOT_MEM_BOOTLOADER_RECLAIMABLE, except firmware tables (ACPI,
 * SMBIOS), which stay where firmware put them.
 */
#ifndef BOOTINFO_BOOTINFO_H
#define BOOTINFO_BOOTINFO_H

#include <stddef.h>
#include <stdint.h>

#define BOOT_INFO_MAGIC   0x4942544F4F42594DULL /* "MYBOOTBI" little-endian */
#define BOOT_INFO_VERSION 1u

#define BOOT_HHDM_BASE    0xFFFF800000000000ULL
#define BOOT_KERNEL_VMIN  0xFFFFFFFF80000000ULL

/* boot_info.flags */
#define BOOT_F_UEFI    (1u << 0) /* booted through UEFI (else legacy BIOS) */
#define BOOT_F_LA57    (1u << 1) /* 5-level paging active; CR3 is a PML5 */
#define BOOT_F_NX      (1u << 2) /* EFER.NXE set; NX bits in use */
#define BOOT_F_HHDM_1G (1u << 3) /* HHDM built from 1 GiB pages */

/* boot_mem_entry.type. Entries are sorted by base, non-overlapping, 4 KiB
 * aligned, and adjacent entries of equal type and flags are merged. */
enum boot_mem_type {
    BOOT_MEM_USABLE = 1,                 /* free RAM */
    BOOT_MEM_RESERVED = 2,               /* never touch */
    BOOT_MEM_ACPI_RECLAIMABLE = 3,       /* free once ACPI tables are parsed */
    BOOT_MEM_ACPI_NVS = 4,               /* firmware-owned, must preserve */
    BOOT_MEM_BAD = 5,                    /* faulty RAM */
    BOOT_MEM_BOOTLOADER_RECLAIMABLE = 6, /* boot_info, our stack/tables, loader image */
    BOOT_MEM_KERNEL = 7,                 /* the loaded kernel image */
    BOOT_MEM_FIRMWARE_RUNTIME = 8,       /* UEFI runtime services code/data */
    BOOT_MEM_MMIO = 9,                   /* firmware-declared MMIO */
    BOOT_MEM_PERSISTENT = 10             /* NVDIMM / persistent memory */
};

/* boot_mem_entry.flags */
#define BOOT_MEM_F_NONVOLATILE      (1u << 0)
#define BOOT_MEM_F_MORE_RELIABLE    (1u << 1)
#define BOOT_MEM_F_SPECIFIC_PURPOSE (1u << 2) /* e.g. HBM; usable but special */

struct boot_mem_entry {
    uint64_t base;   /* physical, 4 KiB aligned */
    uint64_t length; /* bytes, multiple of 4 KiB, never 0 */
    uint32_t type;   /* enum boot_mem_type */
    uint32_t flags;  /* BOOT_MEM_F_* */
};

/* boot_cpu.features */
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

/* boot_cpu.tsc_source: where tsc_hz came from */
enum boot_tsc_source {
    BOOT_TSC_UNKNOWN = 0,    /* tsc_hz is 0 */
    BOOT_TSC_CPUID_15H = 1,  /* crystal ratio: exact */
    BOOT_TSC_CPUID_16H = 2,  /* nominal base MHz: approximate */
    BOOT_TSC_HYPERVISOR = 3, /* hypervisor leaf 0x40000010 */
    BOOT_TSC_CALIBRATED = 4  /* measured against a firmware delay */
};

/* Strings are ASCII, NUL-padded, not necessarily NUL-terminated. */
struct boot_cpu {
    char     vendor[12];          /* CPUID 0, e.g. "GenuineIntel" */
    char     hypervisor[12];      /* CPUID 0x40000000, empty on bare metal */
    char     brand[48];           /* CPUID 0x80000002-4, leading spaces removed */
    uint32_t family;              /* display family (base + extended) */
    uint32_t model;               /* display model (with extended model) */
    uint32_t stepping;
    uint32_t bsp_apic_id;         /* x2APIC ID when available, else xAPIC ID */
    uint64_t features;            /* BOOT_CPU_* */
    uint64_t tsc_hz;              /* 0 if unknown */
    uint32_t tsc_source;          /* enum boot_tsc_source */
    uint16_t threads_per_core;    /* 0 if unknown */
    uint16_t logical_per_package; /* 0 if unknown */
    uint8_t  phys_addr_bits;
    uint8_t  virt_addr_bits;
    uint8_t  reserved[6];
};

/* Linear 32-bit-per-pixel framebuffer. base == 0 means none. */
struct boot_framebuffer {
    uint64_t base;   /* physical */
    uint64_t size;   /* bytes */
    uint32_t width;  /* pixels */
    uint32_t height; /* pixels */
    uint32_t pitch;  /* bytes per scanline */
    uint32_t bpp;    /* always 32 in version 1 */
    uint8_t  red_shift, red_size;
    uint8_t  green_shift, green_size;
    uint8_t  blue_shift, blue_size;
    uint8_t  reserved[2];
};

enum boot_dev_interface {
    BOOT_DEV_UNKNOWN = 0, /* includes virtio and controllers with no path node */
    BOOT_DEV_NVME = 1,
    BOOT_DEV_SATA = 2,
    BOOT_DEV_USB = 3,
    BOOT_DEV_SCSI = 4,
    BOOT_DEV_ATAPI = 5,
    BOOT_DEV_SD = 6,
    BOOT_DEV_EMMC = 7
};

enum boot_part_scheme { BOOT_PART_NONE = 0, BOOT_PART_MBR = 1, BOOT_PART_GPT = 2 };

/* The volume the loader (and kernel) was read from. */
struct boot_device {
    uint32_t interface;           /* enum boot_dev_interface */
    uint8_t  partition_scheme;    /* enum boot_part_scheme */
    uint8_t  removable;           /* 1 if firmware reports removable media */
    uint16_t reserved;
    uint32_t partition_number;    /* 1-based, 0 if unknown */
    uint32_t block_size;          /* bytes per logical block, 0 if unknown */
    uint64_t partition_start_lba;
    uint64_t partition_lba_count;
    uint8_t  partition_guid[16];  /* GPT unique partition GUID, or MBR signature */
};

struct boot_info {
    uint64_t magic;               /* BOOT_INFO_MAGIC */
    uint32_t version;             /* BOOT_INFO_VERSION */
    uint32_t size;                /* sizeof(struct boot_info) as built */
    uint64_t flags;               /* BOOT_F_* */
    uint64_t hhdm_base;           /* virtual address of physical 0 */
    uint64_t hhdm_size;           /* bytes of physical memory mapped there */
    uint64_t kernel_phys_base;    /* physical address of kernel_virt_base */
    uint64_t kernel_virt_base;    /* lowest PT_LOAD page */
    uint64_t kernel_size;         /* bytes, page-rounded span of all PT_LOADs */
    uint64_t mmap;                /* phys of struct boot_mem_entry[mmap_count] */
    uint32_t mmap_count;
    uint32_t mmap_entry_size;     /* sizeof(struct boot_mem_entry); stride */
    uint64_t cmdline;             /* phys of NUL-terminated ASCII, 0 if none */
    uint64_t acpi_rsdp;           /* phys of a checksum-validated RSDP, 0 if none */
    uint64_t smbios_entry;        /* phys of a validated SMBIOS entry point, 0 if none */
    uint64_t uefi_system_table;   /* phys, 0 on BIOS. Boot services are gone. */
    uint8_t  acpi_revision;       /* RSDP revision: 0 = ACPI 1.0, 2 = 2.0+ */
    uint8_t  smbios_major;        /* >= 3 means a 64-bit (_SM3_) entry point */
    uint8_t  smbios_minor;
    uint8_t  firmware_type;       /* 1 = UEFI, 2 = BIOS */
    uint32_t uefi_revision;       /* EFI_SYSTEM_TABLE.Hdr.Revision */
    uint32_t firmware_revision;   /* vendor-defined */
    uint32_t loader_reads;        /* file read calls the loader issued */
    uint64_t loader_bytes_read;   /* bytes those reads transferred */
    uint64_t tsc_loader_entry;    /* TSC at the loader's first instruction */
    uint64_t tsc_exit_start;      /* TSC before the final memory map/exit */
    uint64_t tsc_handoff;         /* TSC immediately before jumping to the kernel */
    char     firmware_vendor[32]; /* ASCII, NUL-padded */
    char     loader_version[16];  /* ASCII, NUL-padded */
    struct boot_cpu         cpu;
    struct boot_framebuffer framebuffer;
    struct boot_device      boot_device;
};

/* ABI verification. A change here is a protocol version change. */
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
_Static_assert(sizeof(struct boot_info) == 416, "boot_info");

#endif
