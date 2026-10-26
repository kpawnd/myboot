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
#include "config/config.h"

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
/* A range of our own EfiLoaderData allocations that the kernel must see
 * with a more specific type (the kernel image, modules). */
struct mmap_carve {
    uint64_t base, end;
    uint32_t type; /* enum boot_mem_type */
};
/* Convert to boot protocol entries, giving each carve its type. Carves are
 * sorted by base and do not overlap; each adds at most 2 entries. Output is
 * unsorted; cannot fail. */
uint32_t   uefi_mmap_normalize(const struct uefi_mmap *m, struct boot_mem_entry *out, uint32_t cap,
                               const struct mmap_carve *carve, uint32_t ncarve);

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

/* Files on the boot volume (config, kernel, modules). Every read goes
 * through kfile_read, which counts it in g_file_stats. */
struct kfile {
    EFI_FILE_PROTOCOL *root, *file;
    uint64_t size;
};
struct file_stats {
    uint32_t reads;
    uint64_t bytes;
};
extern struct file_stats g_file_stats;
/* `path` is ASCII, absolute, backslash-separated (see cfg_resolve_path). */
EFI_STATUS kfile_open(struct kfile *kf, const char *path);
EFI_STATUS kfile_read(struct kfile *kf, uint64_t offset, void *dst, uint64_t len);
void       kfile_close(struct kfile *kf);

/* Boot menu (menu.c). Returns the 0-based entry to try first: the default
 * at once when the timeout is 0, else the user's pick or the default once
 * the countdown runs out. */
uint32_t menu_choose(const struct boot_config *c);

#endif
