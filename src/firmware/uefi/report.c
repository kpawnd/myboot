#include "core/config.h"

#if BOOT_LOG
#include "core/prof.h"
#include "firmware/uefi_console.h"
#include "csme/csme.h"
#include "lib/mem.h"
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

static void blog_name(const char *name, const char *what, uint32_t v)
{
    if (name) {
        blog_str(name);
    } else {
        blog_str(what);
        blog_str(" ");
        blog_dec(v);
    }
}

static void blog_hexn(uint32_t v, int digits)
{
    char d[9];

    for (int i = digits - 1; i >= 0; i--, v >>= 4)
        d[i] = "0123456789ABCDEF"[v & 0xF];
    d[digits] = 0;
    blog_str(d);
}

static void blog_fw_version(const struct boot_csme *c)
{
    blog_dec(c->fw_major);
    blog_str(".");
    blog_dec(c->fw_minor);
    blog_str(".");
    blog_dec(c->fw_hotfix);
    blog_str(".");
    blog_dec(c->fw_build);
}

#if BOOT_DEBUG
static void blog_uuid(const uint8_t u[16])
{
    blog_hexn(u[0] | (u[1] << 8) | (u[2] << 16) | ((uint32_t)u[3] << 24), 8);
    blog_str("-");
    blog_hexn(u[4] | (u[5] << 8), 4);
    blog_str("-");
    blog_hexn(u[6] | (u[7] << 8), 4);
    blog_str("-");
    for (int i = 8; i < 16; i++) {
        if (i == 10)
            blog_str("-");
        blog_hexn(u[i], 2);
    }
}
#endif

static void blog_csme_clients(const struct boot_csme *c, const struct csme_clients *cl)
{
    unsigned unnamed = c->client_count - cl->count;
    const char *sep = " (";

    blog_dec(c->client_count);
    blog_str(c->client_count == 1 ? " client" : " clients");
    for (uint32_t i = 0; i < cl->count; i++) {
        const char *name = csme_client_name(cl->c[i].uuid);
        if (!name) {
            unnamed++;
            continue;
        }
        blog_str(sep);
        blog_str(name);
        sep = ", ";
    }
    if (unnamed) {
        blog_str(sep);
        blog_dec(unnamed);
        blog_str(" unnamed");
        sep = ", ";
    }
    if (sep[0] == ',')
        blog_str(")");
}

static void report_csme(const struct boot_state *s)
{
    const struct boot_csme *c = s->csme;
    const struct csme_clients *cl = s->csme_clients;
    bool normal = c->working_state == 5 && c->op_mode == 0;
    const char *platform;

    if (!(c->flags & BOOT_CSME_PRESENT)) {
        if (!memcmp(s->cpu->vendor, "GenuineIntel", 12)) {
            blog_str("No Intel CSME visible (HECI hidden or absent)");
            blog_end(TAG_INFO);
        } else if (BOOT_DEBUG) {
            blog_str("No Intel CSME check: not an Intel CPU");
            blog_end(TAG_INFO);
        }
        return;
    }

    blog_str("Detected Intel CSME: ");
    if ((platform = csme_platform_name(c->device_id)) != NULL) {
        blog_str(platform);
        blog_str(" ");
    }
    blog_str("(HECI 8086:");
    blog_hexn(c->device_id, 4);
    blog_str("), ");
    blog_name(csme_state_name(c->working_state), "state", c->working_state);
    if (c->op_mode != 0) {
        blog_str(", mode: ");
        blog_name(csme_mode_name(c->op_mode), "mode", c->op_mode);
    }
    if (c->error_code) {
        blog_str(", error ");
        blog_dec(c->error_code);
    }
    if (!(c->flags & BOOT_CSME_INIT_COMPLETE))
        blog_str(", init not complete");
    if (c->flags & BOOT_CSME_VERSION_VALID) {
        blog_str(", firmware ");
        blog_fw_version(c);
    }
    blog_end(normal && !c->error_code ? TAG_OK : TAG_INFO);

    if (c->version_status > BOOT_CSME_ST_NOT_NORMAL) {
        blog_str("Intel CSME did not answer at MKHI's fixed address: ");
        blog_str(csme_status_name(c->version_status));
        blog_end(c->flags & BOOT_CSME_VERSION_VALID ? TAG_INFO : TAG_WARN);
    }
    if (c->hbm_status == BOOT_CSME_ST_OK) {
        blog_str("Talked to Intel CSME over HBM ");
        blog_dec(cl->hbm_major);
        blog_str(".");
        blog_dec(cl->hbm_minor);
        blog_str(": ");
        blog_csme_clients(c, cl);
        blog_end(TAG_OK);
    } else if (c->hbm_status > BOOT_CSME_ST_NOT_NORMAL) {
        blog_str("Intel CSME HBM session failed: ");
        blog_str(csme_status_name(c->hbm_status));
        if (c->client_count) {
            blog_str(", after ");
            blog_csme_clients(c, cl);
        }
        blog_end(TAG_WARN);
    }
    if (c->mkhi_status == BOOT_CSME_ST_OK) {
        blog_str("Intel CSME MKHI client answered over HBM: firmware ");
        blog_fw_version(c);
        blog_end(TAG_OK);
    } else if (c->mkhi_status > BOOT_CSME_ST_NOT_NORMAL) {
        blog_str("Intel CSME MKHI client did not answer over HBM: ");
        blog_str(csme_status_name(c->mkhi_status));
        blog_end(TAG_WARN);
    } else if (c->hbm_status == BOOT_CSME_ST_OK) {
        blog_str("Intel CSME lists no MKHI client");
        blog_end(TAG_INFO);
    }
#if BOOT_DEBUG
    blog_str("CSME status:");
    for (unsigned i = 0; i < 6; i++) {
        blog_str(" HFSTS");
        blog_dec(i + 1);
        blog_str("=");
        blog_hex(c->hfsts[i]);
    }
    blog_end(TAG_INFO);
    for (uint32_t i = 0; i < cl->count; i++) {
        const struct csme_client *e = &cl->c[i];
        const char *name = csme_client_name(e->uuid);

        blog_str("CSME client ");
        blog_dec(e->addr);
        blog_str(": ");
        blog_uuid(e->uuid);
        if (name) {
            blog_str(" ");
            blog_str(name);
        }
        if (e->fixed_address) {
            blog_str(", fixed address ");
            blog_dec(e->fixed_address);
        }
        blog_str(", protocol ");
        blog_dec(e->protocol_version);
        blog_str(", ");
        blog_dec(e->max_connections);
        blog_str(" connections, messages up to ");
        blog_dec(e->max_msg_length);
        blog_str(" bytes");
        blog_end(TAG_INFO);
    }
#endif
}

void report_discovery(const struct boot_state *s)
{
    const struct boot_cpu *cpu = s->cpu;
    const struct boot_framebuffer *fb = s->fb;
    const struct boot_device *dev = s->dev;

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
    if (s->smp->total) {
        blog_str("Found ");
        blog_dec(s->smp->total);
        blog_str(s->smp->total == 1 ? " CPU" : " CPUs");
        blog_str(" in the ACPI MADT");
        if (s->smp->total > s->smp->count) {
            blog_str(", using the first ");
            blog_dec(s->smp->count);
        }
        blog_end(TAG_OK);
    } else {
        blog_str("No ACPI MADT: the kernel gets the boot CPU only");
        blog_end(TAG_INFO);
    }
#if BOOT_DEBUG
    {
        const struct con_mode *m;
        uint32_t cur, n = con_modes(&m, &cur);

        blog_str("Text modes:");
        for (uint32_t i = 0; i < n; i++) {
            blog_str(m[i].number == cur ? " [" : " ");
            blog_dec(m[i].number);
            blog_str("=");
            blog_dec(m[i].cols);
            blog_str("x");
            blog_dec(m[i].rows);
            if (m[i].number == cur)
                blog_str("]");
        }
        blog_end(TAG_INFO);
    }
#endif

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

    report_csme(s);
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

    blog_str("Prepared CPU state: SSE");
    if (s->xcr0 & XCR0_AVX)
        blog_str(", AVX");
    if (s->xcr0 & XCR0_AVX512)
        blog_str(", AVX-512");
    blog_str(", write-combining framebuffer");
    blog_end(TAG_OK);
    if (s->tramp_page) {
        blog_str("Prepared startup of ");
        blog_dec(s->smp_count - 1);
        blog_str(s->smp_count == 2 ? " application processor" : " application processors");
        blog_str(" (trampoline at ");
        blog_hex(s->tramp_page);
        blog_str(")");
        blog_end(TAG_OK);
    } else if (s->smp_count > 1 && s->nosmp) {
        blog_str("Not starting application processors: nosmp");
        blog_end(TAG_INFO);
    } else if (s->smp_count > 1) {
        blog_str("Not starting application processors: no page below 1 MiB or tables above 4 GiB");
        blog_end(TAG_WARN);
    }

#if BOOT_DEBUG && BOOT_PROFILE
    {
        static const char *const names[PROF_COUNT] = {
            [PROF_CPU] = "cpu", [PROF_TSC_CAL] = "tsc-cal", [PROF_MEMORY] = "memory",
            [PROF_ACPI] = "acpi", [PROF_SMBIOS] = "smbios", [PROF_GRAPHICS] = "gop",
            [PROF_STORAGE] = "storage", [PROF_CSME] = "csme", [PROF_CONFIG] = "config+menu", [PROF_KERNEL] = "kernel",
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
