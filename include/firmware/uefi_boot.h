/*
 * Internal interface of the UEFI backend (src/firmware/uefi/). Nothing here
 * is valid after ExitBootServices except the uefi_mmap buffer contents.
 */
#ifndef FIRMWARE_UEFI_BOOT_H
#define FIRMWARE_UEFI_BOOT_H

#include <stdbool.h>
#include <stdint.h>
#include "firmware/uefi.h"
#include "bootinfo/bootinfo.h"
#include "acpi/acpi.h"
#include "smbios/smbios.h"

struct uefi_ctx {
    EFI_HANDLE                 image;
    EFI_SYSTEM_TABLE          *st;
    EFI_BOOT_SERVICES         *bs;
    EFI_LOADED_IMAGE_PROTOCOL *li;
};
extern struct uefi_ctx g_uefi;

extern EFI_GUID g_guid_loaded_image;

/* Raw firmware memory map in one page allocation we own. The buffer is
 * sized once with slack and reused for the final pre-exit read, so no
 * allocation happens between the final GetMemoryMap and ExitBootServices. */
struct uefi_mmap {
    uint8_t *buf;
    UINTN    capacity;  /* bytes allocated */
    UINTN    size;      /* bytes valid after the last read */
    UINTN    key;
    UINTN    desc_size; /* stride; may exceed sizeof(EFI_MEMORY_DESCRIPTOR) */
    uint32_t desc_version;
    uint32_t pages;
};

EFI_STATUS uefi_mmap_alloc(struct uefi_mmap *m, uint32_t slack_descs);
EFI_STATUS uefi_mmap_read(struct uefi_mmap *m);
void       uefi_mmap_free(struct uefi_mmap *m);
uint32_t   uefi_mmap_count(const struct uefi_mmap *m);
uint32_t   uefi_mmap_capacity(const struct uefi_mmap *m);
const EFI_MEMORY_DESCRIPTOR *uefi_mmap_desc(const struct uefi_mmap *m, uint32_t i);
int        uefi_mmap_type_at(const struct uefi_mmap *m, uint64_t addr); /* -1: not mapped */
/* Convert to boot protocol entries, carving [kbase, kbase+ksize) out of
 * loader memory as BOOT_MEM_KERNEL. Unsorted; cannot fail. */
uint32_t   uefi_mmap_normalize(const struct uefi_mmap *m, struct boot_mem_entry *out,
                               uint32_t cap, uint64_t kbase, uint64_t ksize);

/* Discovery: each runs once, caches its result and returns it. */
struct uefi_memory {
    struct uefi_mmap map;
    uint64_t usable; /* free after handoff: conventional + boot services + loader */
    uint64_t total;  /* all RAM-backed types */
    uint64_t top;    /* highest end address of any RAM-backed entry */
};
struct uefi_memory            *discover_memory(void); /* NULL on failure */
const struct acpi_info        *discover_acpi(void);   /* rsdp == 0 if absent */
const struct smbios_info      *discover_smbios(void); /* entry == 0 if absent */
const struct boot_framebuffer *discover_graphics(void); /* base == 0 if absent */
const struct boot_device      *discover_storage(void);
bool uefi_phys_range_ok(uint64_t addr, uint64_t len);

/* The kernel file. All reads go through kfile_read, which counts them. */
struct kfile {
    EFI_FILE_PROTOCOL *root, *file;
    uint64_t size;
    uint32_t reads;
    uint64_t bytes;
};
EFI_STATUS kfile_open(struct kfile *kf, const CHAR16 *path);
EFI_STATUS kfile_read(struct kfile *kf, uint64_t offset, void *dst, uint64_t len);
void       kfile_close(struct kfile *kf);

#endif
