/*
 * UEFI memory map access (UEFI 2.10 section 7.2, GetMemoryMap) and its
 * conversion to the boot protocol's types.
 */
#include "firmware/uefi_boot.h"
#include "core/util.h"

EFI_STATUS uefi_mmap_alloc(struct uefi_mmap *m, uint32_t slack_descs)
{
    EFI_BOOT_SERVICES *bs = g_uefi.bs;
    EFI_PHYSICAL_ADDRESS addr;
    UINTN need = 0;
    EFI_STATUS s;

    /* A zero-size call reports the needed size and descriptor stride. */
    s = bs->GetMemoryMap(&need, NULL, &m->key, &m->desc_size, &m->desc_version);
    if (s != EFI_BUFFER_TOO_SMALL)
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    if (m->desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) || (m->desc_size & 7) || need > MiB(4))
        return EFI_UNSUPPORTED;

    /* Slack absorbs descriptors created by our own later allocations (each
     * can split one free range into three) and by firmware activity. */
    need += (UINTN)slack_descs * m->desc_size;
    m->pages = (uint32_t)((need + PAGE_SIZE - 1) / PAGE_SIZE);
    s = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, m->pages, &addr);
    if (EFI_ERROR(s))
        return s;
    m->buf = (uint8_t *)(uintptr_t)addr;
    m->capacity = (UINTN)m->pages * PAGE_SIZE;
    m->size = 0;
    return EFI_SUCCESS;
}

EFI_STATUS uefi_mmap_read(struct uefi_mmap *m)
{
    m->size = m->capacity;
    return g_uefi.bs->GetMemoryMap(&m->size, (EFI_MEMORY_DESCRIPTOR *)m->buf, &m->key,
                                   &m->desc_size, &m->desc_version);
}

void uefi_mmap_free(struct uefi_mmap *m)
{
    if (m->buf)
        g_uefi.bs->FreePages((uintptr_t)m->buf, m->pages);
    m->buf = 0;
}

uint32_t uefi_mmap_count(const struct uefi_mmap *m)
{
    return (uint32_t)(m->size / m->desc_size);
}

uint32_t uefi_mmap_capacity(const struct uefi_mmap *m)
{
    return (uint32_t)(m->capacity / m->desc_size);
}

const EFI_MEMORY_DESCRIPTOR *uefi_mmap_desc(const struct uefi_mmap *m, uint32_t i)
{
    return (const EFI_MEMORY_DESCRIPTOR *)(m->buf + (UINTN)i * m->desc_size);
}

/* End address of a descriptor, or 0 if it is malformed (unaligned, empty
 * or wrapping past 2^64). Such descriptors are ignored everywhere. */
static uint64_t desc_end(const EFI_MEMORY_DESCRIPTOR *d)
{
    uint64_t start = d->PhysicalStart;

    if ((start & (PAGE_SIZE - 1)) || d->NumberOfPages == 0 ||
        d->NumberOfPages > (UINT64_MAX - start) / PAGE_SIZE)
        return 0;
    return start + d->NumberOfPages * PAGE_SIZE;
}

int uefi_mmap_type_at(const struct uefi_mmap *m, uint64_t addr)
{
    uint32_t n = uefi_mmap_count(m);

    for (uint32_t i = 0; i < n; i++) {
        const EFI_MEMORY_DESCRIPTOR *d = uefi_mmap_desc(m, i);
        uint64_t end = desc_end(d);
        if (end && addr >= d->PhysicalStart && addr < end)
            return (int)d->Type;
    }
    return -1;
}

static uint32_t boot_type(uint32_t efi_type)
{
    switch (efi_type) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:    return BOOT_MEM_USABLE;
    case EfiLoaderCode:
    case EfiLoaderData:          return BOOT_MEM_BOOTLOADER_RECLAIMABLE;
    case EfiACPIReclaimMemory:   return BOOT_MEM_ACPI_RECLAIMABLE;
    case EfiACPIMemoryNVS:       return BOOT_MEM_ACPI_NVS;
    case EfiUnusableMemory:      return BOOT_MEM_BAD;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData: return BOOT_MEM_FIRMWARE_RUNTIME;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace: return BOOT_MEM_MMIO;
    case EfiPersistentMemory:    return BOOT_MEM_PERSISTENT;
    default:                     return BOOT_MEM_RESERVED; /* incl. unaccepted, OEM/OS types */
    }
}

static uint32_t boot_flags(uint64_t attr)
{
    return ((attr & EFI_MEMORY_NV) ? BOOT_MEM_F_NONVOLATILE : 0) |
           ((attr & EFI_MEMORY_MORE_RELIABLE) ? BOOT_MEM_F_MORE_RELIABLE : 0) |
           ((attr & EFI_MEMORY_SP) ? BOOT_MEM_F_SPECIFIC_PURPOSE : 0);
}

static void emit(struct boot_mem_entry *out, uint32_t *n, uint32_t cap,
                 uint64_t base, uint64_t end, uint32_t type, uint32_t flags)
{
    if (end > base && *n < cap) {
        out[*n].base = base;
        out[*n].length = end - base;
        out[*n].type = type;
        out[*n].flags = flags;
        (*n)++;
    }
}

uint32_t uefi_mmap_normalize(const struct uefi_mmap *m, struct boot_mem_entry *out,
                             uint32_t cap, uint64_t kbase, uint64_t ksize)
{
    uint32_t count = uefi_mmap_count(m), n = 0;
    uint64_t kend = kbase + ksize;

    for (uint32_t i = 0; i < count; i++) {
        const EFI_MEMORY_DESCRIPTOR *d = uefi_mmap_desc(m, i);
        uint64_t start = d->PhysicalStart, end = desc_end(d);
        uint32_t type = boot_type(d->Type), flags = boot_flags(d->Attribute);

        if (!end)
            continue;
        if (type == BOOT_MEM_BOOTLOADER_RECLAIMABLE && start < kend && kbase < end) {
            /* The kernel is ordinary EfiLoaderData to firmware (custom
             * memory types break some firmware), so carve it out here. */
            uint64_t ks = kbase > start ? kbase : start;
            uint64_t ke = kend < end ? kend : end;
            emit(out, &n, cap, start, ks, type, flags);
            emit(out, &n, cap, ks, ke, BOOT_MEM_KERNEL, flags);
            emit(out, &n, cap, ke, end, type, flags);
        } else {
            emit(out, &n, cap, start, end, type, flags);
        }
    }
    return n;
}
