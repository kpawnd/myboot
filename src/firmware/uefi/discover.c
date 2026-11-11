/* UEFI hardware/firmware discovery. */
#include "firmware/uefi_boot.h"
#include "core/config.h"
#include "core/util.h"
#include "arch/x86_64/cpu.h"
#include "cpu/cpu.h"
#include "csme/csme.h"
#include "lib/mem.h"

#define MMAP_SLACK_DESCS 32
#define MAX_CONFIG_TABLES 1024

EFI_GUID g_guid_loaded_image = {0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID guid_acpi20  = {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
static EFI_GUID guid_acpi10  = {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};
static EFI_GUID guid_smbios  = {0xEB9D2D31, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};
static EFI_GUID guid_smbios3 = {0xF2FD1544, 0x9794, 0x4A2C, {0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94}};
static EFI_GUID guid_gop     = {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};
static EFI_GUID guid_devpath = {0x09576E91, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID guid_blockio = {0x964E5B21, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

static struct uefi_memory g_mem;
static bool g_mem_done, g_mem_ok;

struct uefi_memory *discover_memory(void)
{
    if (!g_mem_done) {
        uint32_t n;

        g_mem_done = true;
        if (EFI_ERROR(uefi_mmap_alloc(&g_mem.map, MMAP_SLACK_DESCS)))
            return NULL;
        if (EFI_ERROR(uefi_mmap_read(&g_mem.map))) {
            uefi_mmap_free(&g_mem.map);
            return NULL;
        }
        n = uefi_mmap_count(&g_mem.map);
        for (uint32_t i = 0; i < n; i++) {
            const EFI_MEMORY_DESCRIPTOR *d = uefi_mmap_desc(&g_mem.map, i);
            uint64_t start = d->PhysicalStart, bytes, end;

            if (d->NumberOfPages > (UINT64_MAX - start) / PAGE_SIZE)
                continue;
            bytes = d->NumberOfPages * PAGE_SIZE;
            end = start + bytes;
            switch (d->Type) {
            case EfiConventionalMemory:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiLoaderCode:
            case EfiLoaderData:
                g_mem.usable += bytes;
            case EfiRuntimeServicesCode:
            case EfiRuntimeServicesData:
            case EfiACPIReclaimMemory:
            case EfiACPIMemoryNVS:
            case EfiPersistentMemory:
                g_mem.total += bytes;
                if (end > g_mem.top)
                    g_mem.top = end;
                break;
            }
        }
        g_mem_ok = true;
    }
    return g_mem_ok ? &g_mem : NULL;
}

bool uefi_phys_range_ok(uint64_t addr, uint64_t len)
{
    int t0, t1;

    if (!g_mem_ok || addr == 0 || len == 0 || len - 1 > UINT64_MAX - addr)
        return false;
    t0 = uefi_mmap_type_at(&g_mem.map, addr);
    t1 = uefi_mmap_type_at(&g_mem.map, addr + len - 1);
    return t0 >= 0 && t1 >= 0 && t0 != EfiConventionalMemory && t1 != EfiConventionalMemory;
}

static uint64_t g_ct_acpi20, g_ct_acpi10, g_ct_smbios3, g_ct_smbios;
static bool g_ct_done;

static void scan_config_tables(void)
{
    const EFI_CONFIGURATION_TABLE *ct = g_uefi.st->ConfigurationTable;
    UINTN n = g_uefi.st->NumberOfTableEntries;

    if (g_ct_done)
        return;
    g_ct_done = true;
    if (!ct || n > MAX_CONFIG_TABLES)
        return;
    for (UINTN i = 0; i < n; i++) {
        uint64_t p = (uintptr_t)ct[i].VendorTable;
        if (!memcmp(&ct[i].VendorGuid, &guid_acpi20, sizeof(EFI_GUID)))
            g_ct_acpi20 = p;
        else if (!memcmp(&ct[i].VendorGuid, &guid_acpi10, sizeof(EFI_GUID)))
            g_ct_acpi10 = p;
        else if (!memcmp(&ct[i].VendorGuid, &guid_smbios3, sizeof(EFI_GUID)))
            g_ct_smbios3 = p;
        else if (!memcmp(&ct[i].VendorGuid, &guid_smbios, sizeof(EFI_GUID)))
            g_ct_smbios = p;
    }
}

static struct acpi_info g_acpi;
static bool g_acpi_done;

const struct acpi_info *discover_acpi(void)
{
    if (!g_acpi_done) {
        g_acpi_done = true;
        discover_memory();
        scan_config_tables();
        if (!(g_ct_acpi20 && acpi_parse(g_ct_acpi20, uefi_phys_range_ok, &g_acpi) == ACPI_OK) &&
            !(g_ct_acpi10 && acpi_parse(g_ct_acpi10, uefi_phys_range_ok, &g_acpi) == ACPI_OK))
            memset(&g_acpi, 0, sizeof(g_acpi));
    }
    return &g_acpi;
}

static struct smbios_info g_smbios;
static bool g_smbios_done;

const struct smbios_info *discover_smbios(void)
{
    if (!g_smbios_done) {
        g_smbios_done = true;
        discover_memory();
        scan_config_tables();
        if (!(g_ct_smbios3 && smbios_parse(g_ct_smbios3, true, uefi_phys_range_ok, &g_smbios) == SMBIOS_OK) &&
            !(g_ct_smbios && smbios_parse(g_ct_smbios, false, uefi_phys_range_ok, &g_smbios) == SMBIOS_OK))
            memset(&g_smbios, 0, sizeof(g_smbios));
    }
    return &g_smbios;
}

static struct boot_framebuffer g_fb;
static bool g_fb_done;

static void mask_to_field(uint32_t mask, uint8_t *shift, uint8_t *size)
{
    uint8_t s = 0, n = 0;

    if (mask) {
        while (!(mask & 1)) {
            mask >>= 1;
            s++;
        }
        while (mask & 1) {
            mask >>= 1;
            n++;
        }
    }
    *shift = s;
    *size = n;
}

const struct boot_framebuffer *discover_graphics(void)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    const EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    struct boot_framebuffer fb = {0};
    int type;

    if (g_fb_done)
        return &g_fb;
    g_fb_done = true;
    discover_memory();

    if (EFI_ERROR(g_uefi.bs->LocateProtocol(&guid_gop, NULL, (void **)&gop)) || !gop ||
        !(mode = gop->Mode) || !(info = mode->Info) || mode->SizeOfInfo < sizeof(*info))
        return &g_fb;

    switch (info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        fb.red_shift = 0, fb.green_shift = 8, fb.blue_shift = 16;
        fb.red_size = fb.green_size = fb.blue_size = 8;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        fb.blue_shift = 0, fb.green_shift = 8, fb.red_shift = 16;
        fb.red_size = fb.green_size = fb.blue_size = 8;
        break;
    case PixelBitMask:
        mask_to_field(info->PixelInformation.RedMask, &fb.red_shift, &fb.red_size);
        mask_to_field(info->PixelInformation.GreenMask, &fb.green_shift, &fb.green_size);
        mask_to_field(info->PixelInformation.BlueMask, &fb.blue_shift, &fb.blue_size);
        break;
    default:
        return &g_fb;
    }

    fb.base = mode->FrameBufferBase;
    fb.size = mode->FrameBufferSize;
    fb.width = info->HorizontalResolution;
    fb.height = info->VerticalResolution;
    fb.pitch = info->PixelsPerScanLine * 4;
    fb.bpp = 32;

    if (!fb.base || !fb.width || !fb.height || info->PixelsPerScanLine < fb.width ||
        (uint64_t)fb.pitch * fb.height > fb.size || fb.size > UINT64_MAX - fb.base)
        return &g_fb;
    type = uefi_mmap_type_at(&g_mem.map, fb.base);
    if (type == EfiConventionalMemory || type == EfiBootServicesData || type == EfiLoaderData)
        return &g_fb;

    g_fb = fb;
    return &g_fb;
}

static struct boot_device g_dev;
static bool g_dev_done;

static uint32_t messaging_interface(uint8_t subtype)
{
    switch (subtype) {
    case DP_MSG_NVME:      return BOOT_DEV_NVME;
    case DP_MSG_SATA:      return BOOT_DEV_SATA;
    case DP_MSG_USB:
    case DP_MSG_USB_CLASS: return BOOT_DEV_USB;
    case DP_MSG_SCSI:      return BOOT_DEV_SCSI;
    case DP_MSG_ATAPI:     return BOOT_DEV_ATAPI;
    case DP_MSG_SD:        return BOOT_DEV_SD;
    case DP_MSG_EMMC:      return BOOT_DEV_EMMC;
    default:               return BOOT_DEV_UNKNOWN;
    }
}

const struct boot_device *discover_storage(void)
{
    EFI_HANDLE dev = g_uefi.li->DeviceHandle;
    const uint8_t *node;
    EFI_BLOCK_IO_PROTOCOL *bio;

    if (g_dev_done)
        return &g_dev;
    g_dev_done = true;

    if (!EFI_ERROR(g_uefi.bs->HandleProtocol(dev, &guid_devpath, (void **)&node)) && node) {
        for (unsigned i = 0; i < 64 && node[0] != DP_TYPE_END; i++) {
            uint16_t len = rd16(node + 2);
            if (len < 4)
                break;
            if (node[0] == DP_TYPE_MESSAGING) {
                uint32_t itf = messaging_interface(node[1]);
                if (itf != BOOT_DEV_UNKNOWN)
                    g_dev.interface = itf;
            } else if (node[0] == DP_TYPE_MEDIA && node[1] == DP_MEDIA_HD && len >= 42) {
                g_dev.partition_number = rd32(node + 4);
                g_dev.partition_start_lba = rd64(node + 8);
                g_dev.partition_lba_count = rd64(node + 16);
                memcpy(g_dev.partition_guid, node + 24, 16);
                g_dev.partition_scheme = node[41] == 2 ? BOOT_PART_GPT
                                       : node[41] == 1 ? BOOT_PART_MBR : BOOT_PART_NONE;
            }
            node += len;
        }
    }

    if (!EFI_ERROR(g_uefi.bs->HandleProtocol(dev, &guid_blockio, (void **)&bio)) && bio && bio->Media) {
        g_dev.block_size = bio->Media->BlockSize;
        g_dev.removable = bio->Media->RemovableMedia ? 1 : 0;
    }
    return &g_dev;
}

#define HECI_DEVFN   ((0x16u << 3) | 0)
#define MCFG_ENTRIES 44
#define MCFG_ENTRY   16

static struct boot_csme g_csme;
static struct csme_clients g_csme_clients;
static bool g_csme_done;
static uint64_t g_heci_ecam;

static uint64_t ecam_bus0(void)
{
    const struct acpi_info *a = discover_acpi();
    uint32_t len;
    uint64_t t = a->rsdp ? acpi_find_table(a, "MCFG", uefi_phys_range_ok, &len) : 0;

    for (uint32_t off = MCFG_ENTRIES; t && off + MCFG_ENTRY <= len; off += MCFG_ENTRY) {
        const uint8_t *e = (const uint8_t *)(uintptr_t)t + off;
        if (rd16(e + 8) == 0 && e[10] == 0)
            return rd64(e);
    }
    return 0;
}

static uint32_t heci_cfg_read32(uint16_t off)
{
    uint32_t v;
    UINTN tpl;

    if (g_heci_ecam)
        return *(volatile uint32_t *)(uintptr_t)(g_heci_ecam + off);
    tpl = g_uefi.bs->RaiseTPL(TPL_HIGH_LEVEL);
    outl(0xCF8, 0x80000000u | (HECI_DEVFN << 8) | (off & 0xFC));
    v = inl(0xCFC);
    g_uefi.bs->RestoreTPL(tpl);
    return v;
}

static void heci_cfg_write16(uint16_t off, uint16_t v)
{
    UINTN tpl;

    if (g_heci_ecam) {
        *(volatile uint16_t *)(uintptr_t)(g_heci_ecam + off) = v;
        return;
    }
    tpl = g_uefi.bs->RaiseTPL(TPL_HIGH_LEVEL);
    outl(0xCF8, 0x80000000u | (HECI_DEVFN << 8) | (off & 0xFC));
    outw((uint16_t)(0xCFC + (off & 2)), v);
    g_uefi.bs->RestoreTPL(tpl);
}

static uint32_t heci_mmio_read32(uint64_t addr) { return *(volatile uint32_t *)(uintptr_t)addr; }
static void heci_mmio_write32(uint64_t addr, uint32_t v) { *(volatile uint32_t *)(uintptr_t)addr = v; }
static void heci_delay_us(uint32_t us) { g_uefi.bs->Stall(us); }

const struct boot_csme *discover_csme(void)
{
    static const struct csme_ops ops = {
        heci_cfg_read32, heci_cfg_write16, heci_mmio_read32, heci_mmio_write32, heci_delay_us,
    };
    uint64_t ecam;

    if (g_csme_done)
        return &g_csme;
    g_csme_done = true;
    if (memcmp(discover_cpu()->vendor, "GenuineIntel", 12) != 0)
        return &g_csme;
    if ((ecam = ecam_bus0()) != 0)
        g_heci_ecam = ecam + ((uint64_t)HECI_DEVFN << 12);
    csme_probe(&ops, &g_csme);
#if BOOT_CSME_QUERY
    {
        UINTN tpl = g_uefi.bs->RaiseTPL(TPL_HIGH_LEVEL);
        csme_query_version(&ops, &g_csme);
        csme_hbm_session(&ops, &g_csme, &g_csme_clients);
        g_uefi.bs->RestoreTPL(tpl);
    }
#endif
    return &g_csme;
}

const struct csme_clients *discover_csme_clients(void)
{
    return &g_csme_clients;
}

static struct smp_madt_cpu g_smp_cpu[SMP_MAX_CPUS];
static struct smp_cpus g_smp = {0, 0, g_smp_cpu};
static bool g_smp_done;

const struct smp_cpus *discover_smp(void)
{
    const struct acpi_info *a;
    uint32_t len;
    uint64_t madt;

    if (g_smp_done)
        return &g_smp;
    g_smp_done = true;
    a = discover_acpi();
    if (!a->rsdp || !(madt = acpi_find_table(a, "APIC", uefi_phys_range_ok, &len)))
        return &g_smp;
    g_smp.total = smp_parse_madt((const uint8_t *)(uintptr_t)madt, len, discover_cpu()->bsp_apic_id, g_smp_cpu,
                                 SMP_MAX_CPUS);
    g_smp.count = g_smp.total < SMP_MAX_CPUS ? g_smp.total : SMP_MAX_CPUS;
    return &g_smp;
}
