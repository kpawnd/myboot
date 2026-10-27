/*
 * Status lines for the boot log (DEBUG and BALANCED builds only): what
 * discovery found, right after it ran, and the prepared handoff, just
 * before the kernel starts. DEBUG adds detail lines: CPU features, ACPI
 * table signatures, SMBIOS strings, kernel placement and phase timings.
 */
#include "core/config.h"

#if BOOT_LOG
#include "core/prof.h"
#include "firmware/uefi_console.h"
#include "boot_state.h"

static void blog_uefi_revision(uint32_t rev)
{
    uint32_t lo = rev & 0xFFFF;

    blog_dec(rev >> 16);
    blog_str(".");
    blog_dec(lo / 10);
    if (lo % 10) {
        blog_str(".");
        blog_dec(lo % 10);
    }
}

/* Firmware strings are UTF-16 of unknown length: bounded, ASCII only. */
static void blog_vendor(const CHAR16 *s)
{
    char buf[64];
    unsigned i = 0;

    if (s == NULL) {
        blog_str("(none)");
        return;
    }
    for (; i + 1 < sizeof(buf) && s[i]; i++)
        buf[i] = (s[i] >= 0x20 && s[i] < 0x7F) ? (char)s[i] : '?';
    buf[i] = 0;
    blog_str(buf);
}

static const char *interface_name(uint32_t itf)
{
    static const char *const names[] = {
        "unknown", "NVMe", "SATA", "USB", "SCSI", "ATAPI", "SD", "eMMC",
    };
    return itf < sizeof(names) / sizeof(names[0]) ? names[itf] : "unknown";
}

#if BOOT_DEBUG
static void blog_features(uint64_t f)
{
    static const struct { uint64_t bit; const char *name; } tab[] = {
        {BOOT_CPU_NX, "NX"}, {BOOT_CPU_PDPE1GB, "1G-pages"}, {BOOT_CPU_LA57, "LA57"},
        {BOOT_CPU_APIC, "APIC"}, {BOOT_CPU_X2APIC, "x2APIC"}, {BOOT_CPU_TSC, "TSC"},
        {BOOT_CPU_INVARIANT_TSC, "invariant-TSC"}, {BOOT_CPU_RDTSCP, "RDTSCP"},
        {BOOT_CPU_SSE2, "SSE2"}, {BOOT_CPU_SSE3, "SSE3"}, {BOOT_CPU_SSSE3, "SSSE3"},
        {BOOT_CPU_SSE41, "SSE4.1"}, {BOOT_CPU_SSE42, "SSE4.2"}, {BOOT_CPU_POPCNT, "POPCNT"},
        {BOOT_CPU_AVX, "AVX"}, {BOOT_CPU_AVX2, "AVX2"}, {BOOT_CPU_AVX512F, "AVX-512F"},
        {BOOT_CPU_AES, "AES"}, {BOOT_CPU_SHA, "SHA"}, {BOOT_CPU_BMI1, "BMI1"},
        {BOOT_CPU_BMI2, "BMI2"}, {BOOT_CPU_ERMS, "ERMS"}, {BOOT_CPU_RDRAND, "RDRAND"},
        {BOOT_CPU_XSAVE, "XSAVE"}, {BOOT_CPU_HYPERVISOR, "hypervisor"},
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (f & tab[i].bit) {
            blog_str(" ");
            blog_str(tab[i].name);
        }
    }
}

static void blog_smbios_field(const struct smbios_info *sm, uint8_t type, uint8_t field)
{
    size_t len;
    const char *s = smbios_string(sm, type, field, &len);

    if (s)
        blog_ascii(s, len);
    else
        blog_str("?");
}

static void blog_us(uint64_t ticks, uint64_t hz)
{
    if (hz) {
        blog_dec(ticks * 1000000 / hz);
        blog_str(" us");
    } else {
        blog_dec(ticks);
        blog_str(" ticks");
    }
}
#endif

void report_discovery(const struct boot_state *s)
{
    const struct boot_cpu *cpu = s->cpu;
    const struct boot_framebuffer *fb = s->fb;
    const struct boot_device *dev = s->dev;

    /* The heading, untagged like systemd's "Welcome to ...". */
    blog_str("MYBOOT " LOADER_VERSION " [");
    blog_wstr(BOOT_CONFIG_NAME);
    blog_str("] on ");
    blog_vendor(g_uefi.st->FirmwareVendor);
    blog_str(" firmware, UEFI ");
    blog_uefi_revision(g_uefi.st->Hdr.Revision);
    blog_end(TAG_NONE);

    blog_str("Detected CPU: ");
    blog_ascii(cpu->brand[0] ? cpu->brand : cpu->vendor, cpu->brand[0] ? 48 : 12);
    if (cpu->logical_per_package) {
        blog_str(", ");
        blog_dec(cpu->logical_per_package);
        blog_str(cpu->logical_per_package == 1 ? " thread" : " threads");
    }
    if (cpu->tsc_hz) {
        blog_str(", TSC ");
        blog_dec(cpu->tsc_hz / 1000000);
        blog_str(" MHz");
    }
    blog_end(TAG_OK);
#if BOOT_DEBUG
    blog_str("CPU features:");
    blog_features(cpu->features);
    blog_end(TAG_INFO);
#endif

    blog_str("Read memory map: ");
    blog_dec(s->mem->usable >> 20);
    blog_str(" MiB usable of ");
    blog_dec(s->mem->total >> 20);
    blog_str(" MiB (");
    blog_dec(uefi_mmap_count(&s->mem->map));
    blog_str(" entries)");
    blog_end(TAG_OK);

    if (s->acpi->rsdp) {
        blog_str("Found ACPI tables (revision ");
        blog_dec(s->acpi->revision);
        blog_str(", ");
        blog_dec(s->acpi->count);
        blog_str(" tables)");
        blog_end(TAG_OK);
#if BOOT_DEBUG
        blog_str("ACPI tables:");
        for (uint32_t i = 0; i < s->acpi->count && i < 32; i++) {
            char sig[4];
            blog_str(" ");
            if (acpi_table_sig(acpi_table(s->acpi, i), uefi_phys_range_ok, sig))
                blog_ascii(sig, 4);
            else
                blog_str("(bad)");
        }
        blog_end(TAG_INFO);
#endif
    } else {
        blog_str("No ACPI tables: the kernel cannot find its devices");
        blog_end(TAG_WARN);
    }

    if (s->smbios->entry) {
        blog_str("Found SMBIOS ");
        blog_dec(s->smbios->major);
        blog_str(".");
        blog_dec(s->smbios->minor);
#if BOOT_DEBUG
        blog_str(": ");
        blog_smbios_field(s->smbios, 1, 0x04);
        blog_str(" ");
        blog_smbios_field(s->smbios, 1, 0x05);
#endif
        blog_end(TAG_OK);
    } else {
        blog_str("No SMBIOS tables");
        blog_end(TAG_INFO);
    }

    if (fb->base) {
        blog_str("Found framebuffer ");
        blog_dec(fb->width);
        blog_str("x");
        blog_dec(fb->height);
        blog_str(" at ");
        blog_hex(fb->base);
        blog_end(TAG_OK);
    } else {
        blog_str("No framebuffer: the kernel gets no screen");
        blog_end(TAG_WARN);
    }

    blog_str("Found boot device: ");
    blog_str(interface_name(dev->interface));
    if (dev->partition_number) {
        blog_str(", partition ");
        blog_dec(dev->partition_number);
        blog_str(dev->partition_scheme == BOOT_PART_GPT ? " (GPT)"
                 : dev->partition_scheme == BOOT_PART_MBR ? " (MBR)" : "");
    }
    if (dev->removable)
        blog_str(", removable");
    blog_end(TAG_OK);
}

void report_handoff(const struct boot_state *s)
{
#if BOOT_DEBUG
    blog_str("Kernel at ");
    blog_hex(s->elf.vbase);
    blog_str(" (physical ");
    blog_hex(s->kphys);
    blog_str("), ");
    blog_dec(s->elf.nseg);
    blog_str(" segments, entry ");
    blog_hex(s->elf.entry);
    blog_end(TAG_INFO);
#endif

    blog_str("Built page tables: ");
    blog_dec(s->pt.used);
    blog_str(" pages, direct map of ");
    blog_dec(s->hhdm_size >> 20);
    blog_str(s->hhdm_1g ? " MiB with 1 GiB pages" : " MiB with 2 MiB pages");
    if (s->la57)
        blog_str(", 5-level");
    blog_end(TAG_OK);

    blog_str("Prepared boot information: ");
    blog_size(s->arena.used);
    blog_str(", ");
    blog_dec(s->nmod);
    blog_str(s->nmod == 1 ? " module" : " modules");
    blog_end(TAG_OK);

#if BOOT_DEBUG && BOOT_PROFILE
    {
        static const char *const names[PROF_COUNT] = {
            [PROF_CPU] = "cpu", [PROF_TSC_CAL] = "tsc-cal", [PROF_MEMORY] = "memory",
            [PROF_ACPI] = "acpi", [PROF_SMBIOS] = "smbios", [PROF_GRAPHICS] = "gop",
            [PROF_STORAGE] = "storage", [PROF_CONFIG] = "config+menu", [PROF_KERNEL] = "kernel",
            [PROF_PAGING] = "handoff-prep",
        };
        blog_str("Timings:");
        for (unsigned i = 1; i < PROF_COUNT; i++) {
            blog_str(" ");
            blog_str(names[i]);
            blog_str(" ");
            blog_us(g_prof[i] - g_prof[i - 1], s->cpu->tsc_hz);
            blog_str(i + 1 < PROF_COUNT ? "," : "");
        }
        blog_end(TAG_INFO);
    }
#endif
}
#endif
