/*
 * Boot summary on the firmware console (DEBUG and BALANCED builds only).
 * Printed once, after all work and before ExitBootServices, so console
 * cost never sits inside a measured phase. BALANCED prints the summary;
 * DEBUG adds feature lists, ACPI table signatures and SMBIOS strings.
 */
#include "core/config.h"

#if BOOT_LOG
#include "core/prof.h"
#include "firmware/uefi_console.h"
#include "boot_state.h"

static const CHAR16 *vendor_string(const CHAR16 *s)
{
    if (s == NULL)
        return L"(none)";
    for (unsigned i = 0; i < 128; i++)
        if (s[i] == 0)
            return s;
    return L"(invalid)";
}

static void put_uefi_revision(uint32_t rev)
{
    uint32_t lo = rev & 0xFFFF;

    con_put_dec(rev >> 16);
    con_puts(L".");
    con_put_dec(lo / 10);
    if (lo % 10) {
        con_puts(L".");
        con_put_dec(lo % 10);
    }
}

static void put_kib(uint64_t bytes)
{
    con_put_dec((bytes + 1023) / 1024);
    con_puts(L" KiB");
}

static void put_ticks(uint64_t ticks, uint64_t hz)
{
    if (hz) {
        con_put_dec(ticks * 1000000 / hz);
        con_puts(L" us");
    } else {
        con_put_dec(ticks);
        con_puts(L" ticks");
    }
}

static const CHAR16 *tsc_source_name(uint32_t src)
{
    switch (src) {
    case BOOT_TSC_CPUID_15H:  return L"CPUID 15h";
    case BOOT_TSC_CPUID_16H:  return L"CPUID 16h";
    case BOOT_TSC_HYPERVISOR: return L"hypervisor";
    case BOOT_TSC_CALIBRATED: return L"calibrated";
    default:                  return L"unknown";
    }
}

static const CHAR16 *interface_name(uint32_t itf)
{
    static const CHAR16 *const names[] = {
        L"unknown", L"NVMe", L"SATA", L"USB", L"SCSI", L"ATAPI", L"SD", L"eMMC",
    };
    return itf < sizeof(names) / sizeof(names[0]) ? names[itf] : L"unknown";
}

#if BOOT_DEBUG
static void put_features(uint64_t f)
{
    static const struct { uint64_t bit; const CHAR16 *name; } tab[] = {
        {BOOT_CPU_NX, L"NX"}, {BOOT_CPU_PDPE1GB, L"1G-pages"}, {BOOT_CPU_LA57, L"LA57"},
        {BOOT_CPU_APIC, L"APIC"}, {BOOT_CPU_X2APIC, L"x2APIC"}, {BOOT_CPU_TSC, L"TSC"},
        {BOOT_CPU_INVARIANT_TSC, L"invariant-TSC"}, {BOOT_CPU_RDTSCP, L"RDTSCP"},
        {BOOT_CPU_SSE2, L"SSE2"}, {BOOT_CPU_SSE3, L"SSE3"}, {BOOT_CPU_SSSE3, L"SSSE3"},
        {BOOT_CPU_SSE41, L"SSE4.1"}, {BOOT_CPU_SSE42, L"SSE4.2"}, {BOOT_CPU_POPCNT, L"POPCNT"},
        {BOOT_CPU_AVX, L"AVX"}, {BOOT_CPU_AVX2, L"AVX2"}, {BOOT_CPU_AVX512F, L"AVX-512F"},
        {BOOT_CPU_AES, L"AES"}, {BOOT_CPU_SHA, L"SHA"}, {BOOT_CPU_BMI1, L"BMI1"},
        {BOOT_CPU_BMI2, L"BMI2"}, {BOOT_CPU_ERMS, L"ERMS"}, {BOOT_CPU_RDRAND, L"RDRAND"},
        {BOOT_CPU_XSAVE, L"XSAVE"}, {BOOT_CPU_HYPERVISOR, L"hypervisor"},
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (f & tab[i].bit) {
            con_puts(L" ");
            con_puts(tab[i].name);
        }
    }
}

static void put_smbios_field(const struct smbios_info *sm, uint8_t type, uint8_t field)
{
    size_t len;
    const char *s = smbios_string(sm, type, field, &len);

    if (s)
        con_put_ascii(s, len);
    else
        con_puts(L"?");
}
#endif

void report_summary(const struct boot_state *s)
{
    const struct boot_cpu *cpu = s->cpu;
    const struct boot_framebuffer *fb = s->fb;
    const struct boot_device *dev = s->dev;

    con_puts(L"MYBOOT x86-64 " LOADER_VERSION_W L" [" BOOT_CONFIG_NAME L"]\r\n");

    con_puts(L"Firmware       : ");
    con_puts(vendor_string(g_uefi.st->FirmwareVendor));
    con_puts(L" rev ");
    con_put_hex(g_uefi.st->FirmwareRevision);
    con_puts(L", UEFI ");
    put_uefi_revision(g_uefi.st->Hdr.Revision);

    con_puts(L"\r\nCPU            : ");
    con_put_ascii(cpu->brand[0] ? cpu->brand : cpu->vendor, cpu->brand[0] ? 48 : 12);
    con_puts(L"\r\nCPU id         : ");
    con_put_ascii(cpu->vendor, 12);
    con_puts(L" family ");
    con_put_dec(cpu->family);
    con_puts(L" model ");
    con_put_dec(cpu->model);
    con_puts(L" stepping ");
    con_put_dec(cpu->stepping);
    if (cpu->hypervisor[0]) {
        con_puts(L", hypervisor ");
        con_put_ascii(cpu->hypervisor, 12);
    }
    con_puts(L"\r\nAddress bits   : ");
    con_put_dec(cpu->phys_addr_bits);
    con_puts(L" physical, ");
    con_put_dec(cpu->virt_addr_bits);
    con_puts(L" virtual");
    if (cpu->threads_per_core || cpu->logical_per_package) {
        con_puts(L"\r\nTopology       : ");
        con_put_dec(cpu->logical_per_package);
        con_puts(L" logical per package, ");
        con_put_dec(cpu->threads_per_core);
        con_puts(L" per core");
    }
#if BOOT_DEBUG
    con_puts(L"\r\nCPU features   :");
    put_features(cpu->features);
#endif
    con_puts(L"\r\nTSC            : ");
    if (cpu->tsc_hz) {
        con_put_dec(cpu->tsc_hz / 1000000);
        con_puts(L" MHz (");
        con_puts(tsc_source_name(cpu->tsc_source));
        con_puts(L")");
    } else {
        con_puts(L"frequency unknown");
    }
    con_puts((cpu->features & BOOT_CPU_INVARIANT_TSC) ? L", invariant" : L", not invariant");

    con_puts(L"\r\nMemory         : ");
    con_put_dec(s->mem->usable >> 20);
    con_puts(L" MiB usable of ");
    con_put_dec(s->mem->total >> 20);
    con_puts(L" MiB RAM, ");
    con_put_dec(uefi_mmap_count(&s->mem->map));
    con_puts(L" firmware map entries, top ");
    con_put_hex(s->mem->top);

    con_puts(L"\r\nACPI           : ");
    if (s->acpi->rsdp) {
        con_puts(L"revision ");
        con_put_dec(s->acpi->revision);
        con_puts(s->acpi->entry_size == 8 ? L", XSDT at " : L", RSDT at ");
        con_put_hex(s->acpi->sdt);
        con_puts(L", ");
        con_put_dec(s->acpi->count);
        con_puts(L" tables");
#if BOOT_DEBUG
        con_puts(L"\r\nACPI tables    :");
        for (uint32_t i = 0; i < s->acpi->count && i < 64; i++) {
            char sig[4];
            con_puts(L" ");
            if (acpi_table_sig(acpi_table(s->acpi, i), uefi_phys_range_ok, sig))
                con_put_ascii(sig, 4);
            else
                con_puts(L"(bad)");
        }
#endif
    } else {
        con_puts(L"not found");
    }

    con_puts(L"\r\nSMBIOS         : ");
    if (s->smbios->entry) {
        con_put_dec(s->smbios->major);
        con_puts(L".");
        con_put_dec(s->smbios->minor);
        con_puts(L" at ");
        con_put_hex(s->smbios->entry);
#if BOOT_DEBUG
        con_puts(L"\r\nSystem         : ");
        put_smbios_field(s->smbios, 1, 0x04);
        con_puts(L" ");
        put_smbios_field(s->smbios, 1, 0x05);
        con_puts(L"\r\nBIOS           : ");
        put_smbios_field(s->smbios, 0, 0x04);
        con_puts(L" ");
        put_smbios_field(s->smbios, 0, 0x05);
#endif
    } else {
        con_puts(L"not found");
    }

    con_puts(L"\r\nFramebuffer    : ");
    if (fb->base) {
        con_put_dec(fb->width);
        con_puts(L"x");
        con_put_dec(fb->height);
        con_puts(L", pitch ");
        con_put_dec(fb->pitch);
        con_puts(L", at ");
        con_put_hex(fb->base);
    } else {
        con_puts(L"none");
    }

    con_puts(L"\r\nBoot device    : ");
    con_puts(interface_name(dev->interface));
    if (dev->partition_number) {
        con_puts(L", partition ");
        con_put_dec(dev->partition_number);
        con_puts(dev->partition_scheme == BOOT_PART_GPT ? L" (GPT)"
                 : dev->partition_scheme == BOOT_PART_MBR ? L" (MBR)" : L"");
    }
    if (dev->block_size) {
        con_puts(L", ");
        con_put_dec(dev->block_size);
        con_puts(L"-byte blocks");
    }
    if (dev->removable)
        con_puts(L", removable");

    con_puts(L"\r\nBoot entry     : ");
    if (s->have_cfg) {
        con_put_dec(s->entry + 1);
        con_puts(L" of ");
        con_put_dec(s->cfg->nentries);
        con_puts(L", ");
        con_put_ascii(s->cfg->entry[s->entry].title, CFG_TITLE_MAX);
    } else {
        con_puts(L"built-in default (no usable BOOT.CFG)");
    }

    con_puts(L"\r\nKernel         : ");
    con_put_ascii(s->kpath, CFG_RESOLVED_MAX);
    con_puts(L", ");
    put_kib(s->kf.size);
    con_puts(L", ");
    con_put_dec(s->elf.nseg);
    con_puts(L" segments, entry ");
    con_put_hex(s->elf.entry);
    con_puts(L"\r\nKernel image   : virt ");
    con_put_hex(s->elf.vbase);
    con_puts(L" -> phys ");
    con_put_hex(s->kphys);
    con_puts(L", ");
    put_kib(s->elf.vend - s->elf.vbase);
    con_puts(L"\r\nModules        : ");
    con_put_dec(s->nmod);
    if (s->nmod) {
        con_puts(L", ");
        put_kib(s->mod_bytes);
    }
#if BOOT_DEBUG
    for (uint32_t i = 0; i < s->nmod; i++) {
        con_puts(L"\r\n  module       : ");
        con_put_ascii(s->mods[i].string, BOOT_MODULE_STRING_MAX);
        con_puts(L", ");
        con_put_dec(s->mods[i].size);
        con_puts(L" bytes at ");
        con_put_hex(s->mods[i].phys);
    }
#endif
    con_puts(L"\r\nDisk reads     : ");
    con_put_dec(g_file_stats.reads);
    con_puts(L" reads, ");
    con_put_dec(g_file_stats.bytes);
    con_puts(L" bytes (config, kernel, modules)");

    con_puts(L"\r\nCommand line   : ");
    if (s->cmdline_len)
        con_put_ascii(s->cmdline, s->cmdline_len);
    else
        con_puts(L"(none)");

    con_puts(L"\r\nPage tables    : ");
    con_put_dec(s->pt.used);
    con_puts(L" of ");
    con_put_dec(s->pt_bound);
    con_puts(L" pages reserved, HHDM ");
    con_put_dec(s->hhdm_size >> 20);
    con_puts(s->hhdm_1g ? L" MiB with 1 GiB pages" : L" MiB with 2 MiB pages");
    if (s->la57)
        con_puts(L", 5-level");
    con_puts(L"\r\nHandoff arena  : ");
    put_kib(s->arena.used);
    con_puts(L" used of ");
    put_kib(s->arena.size);
    con_puts(L" (64 KiB is the kernel stack)");

#if BOOT_PROFILE
    {
        static const CHAR16 *const names[PROF_COUNT] = {
            [PROF_CPU] = L"cpu", [PROF_TSC_CAL] = L"tsc-cal", [PROF_MEMORY] = L"memory",
            [PROF_ACPI] = L"acpi", [PROF_SMBIOS] = L"smbios", [PROF_GRAPHICS] = L"gop",
            [PROF_STORAGE] = L"storage", [PROF_CONFIG] = L"config", [PROF_KERNEL] = L"kernel", [PROF_PAGING] = L"handoff-prep",
        };
        con_puts(L"\r\nTimings        :");
        for (unsigned i = 1; i < PROF_COUNT; i++) {
            con_puts(L" ");
            con_puts(names[i]);
            con_puts(L" ");
            put_ticks(g_prof[i] - g_prof[i - 1], cpu->tsc_hz);
            con_puts(i + 1 < PROF_COUNT ? L"," : L"");
        }
        con_puts(L"\r\nLoader total   : ");
        put_ticks(g_prof[PROF_COUNT - 1] - g_prof[PROF_ENTRY], cpu->tsc_hz);
        con_puts(L" before this summary");
    }
#endif
    con_puts(L"\r\nEntering kernel\r\n");
}
#endif
