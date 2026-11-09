/*
 * UEFI boot sequence: firmware entry -> discovery -> BOOT.CFG and menu ->
 * kernel and modules -> page tables -> ExitBootServices -> kernel.
 *
 * The firmware has already loaded this PE32+ image, applied its base
 * relocations, and called efi_main in 64-bit long mode with paging
 * enabled (memory identity-mapped), interrupts enabled and a firmware
 * stack of at least 128 KiB (UEFI spec 2.3.4, x64 platforms).
 * There is no C runtime: efi_main is the PE entry point.
 *
 * Allocations (all EfiLoaderData, all tracked in S for cleanup on error):
 *   raw memory-map buffer   (discover_memory)
 *   kernel image            (load_kernel)
 *   one block per module    (load_module)
 *   handoff arena           (prepare_handoff)
 * If an entry fails to load, its kernel and modules are freed and the next
 * entry is tried; only when every entry fails does control return to the
 * firmware.
 */
#include <stddef.h>
#include <stdint.h>
#include "core/config.h"
#include "core/prof.h"
#include "core/util.h"
#include "arch/x86_64/cpu.h"
#include "cpu/cpu.h"
#include "memory/memmap.h"
#include "lib/mem.h"
#include "firmware/uefi_console.h"
#include "boot_state.h"

#define CMDLINE_MAX      CFG_CMDLINE_MAX /* UTF-16 chars accepted from LoadOptions */
#define CONFIG_MAX       KiB(16)
#define MODULE_MAX       GiB(16)         /* sanity bound on a module file */
#define EBS_MAX_ATTEMPTS 3
#define FAIL_DELAY_US    3000000         /* keep an error on screen before firmware moves on */
#define TSC_CAL_US       2000
#define HHDM_MAX         (1ull << 46)    /* 64 TiB: keeps the HHDM clear of the kernel's PML4 slot */
#define LOW_HALF_END     (1ull << 47)

/* Progress squares after ExitBootServices (see stage()) */
enum {
    STAGE_EXITED = 1, /* firmware gone, memory map final */
    STAGE_CPU_STATE,  /* CR0, CR4, XCR0 */
    STAGE_PAT,
    STAGE_SMP,        /* + SMP_STAGE_*: APIC read, INIT sent, 10 ms passed, SIPI, SIPI */
    STAGE_HANDOFF = STAGE_SMP + SMP_STAGE_SIPI2 + 1 /* APs settled, jumping to the kernel */
};

struct uefi_ctx g_uefi;
#if BOOT_PROFILE
uint64_t g_prof[PROF_COUNT];
#endif

static struct boot_state S;
static uint8_t g_hdr_buf[4096];          /* ELF + program headers; loader scratch */
static char g_cfg_text[CONFIG_MAX + 1];  /* BOOT.CFG; parsed in place */
static struct boot_config g_cfg;
static char g_load_options[CMDLINE_MAX]; /* LoadOptions as ASCII, without BOOT.CFG */

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

/* src/arch/x86_64/handoff.S */
extern void boot_handoff(uint64_t cr3, uint64_t gdtr_virt, uint64_t stack_top,
                         uint64_t bootinfo_virt, uint64_t entry) __attribute__((noreturn));

static int system_table_valid(const EFI_SYSTEM_TABLE *st)
{
    const EFI_BOOT_SERVICES *bs;

    if (st == NULL || st->Hdr.Signature != EFI_SYSTEM_TABLE_SIGNATURE)
        return 0;
    /* HeaderSize covers the whole table; it must reach every field we read. */
    if (st->Hdr.HeaderSize < offsetof(EFI_SYSTEM_TABLE, ConfigurationTable) + sizeof(void *))
        return 0;
    if (!st->ConOut || !(bs = st->BootServices) || bs->Hdr.Signature != EFI_BOOT_SERVICES_SIGNATURE)
        return 0;
    return bs->Hdr.HeaderSize >= offsetof(EFI_BOOT_SERVICES, LocateProtocol) + sizeof(void *);
}

static EFI_STATUS entry_error(const CHAR16 *what, const char *detail, EFI_STATUS status)
{
    boot_error(what, detail, status);
    return status;
}

/* Undo load_entry, so the next entry starts from a clean state. */
static void release_entry(void)
{
    kfile_close(&S.kf);
    if (S.kpages)
        g_uefi.bs->FreePages(S.kphys, S.kpages);
    for (uint32_t i = 0; i < S.nmod; i++)
        if (S.mods[i].pages)
            g_uefi.bs->FreePages(S.mods[i].phys, S.mods[i].pages);
    S.kpages = 0;
    S.nmod = 0;
    S.mod_bytes = 0;
}

static void release_resources(void)
{
    release_entry();
    if (S.arena_pages)
        g_uefi.bs->FreePages(S.arena.base, S.arena_pages);
    if (S.tramp_page)
        g_uefi.bs->FreePages(S.tramp_page, 1);
    if (S.mem)
        uefi_mmap_free(&S.mem->map);
    S.arena_pages = 0;
    S.tramp_page = 0;
}

/* Every fatal failure before ExitBootServices ends here: all our memory
 * released, and control returned to the firmware so its boot manager can
 * try the next boot option. */
static EFI_STATUS fail(const CHAR16 *what, const char *detail, EFI_STATUS status)
{
    boot_error(what, detail, status);
    release_resources();
    g_uefi.bs->Stall(FAIL_DELAY_US);
    return status;
}

/* Only reached when CPUID gives no TSC frequency (AMD, most VMs). Costs
 * TSC_CAL_US of wall time: profiling builds pay it at startup, the rest
 * only when AP startup needs a clock after ExitBootServices. */
static void calibrate_tsc(void)
{
    uint64_t t0 = rdtsc();
    g_uefi.bs->Stall(TSC_CAL_US);
    cpu_set_tsc_hz((rdtsc() - t0) * (1000000 / TSC_CAL_US), BOOT_TSC_CALIBRATED);
}

/* LoadOptions become the kernel command line only if they are plain
 * printable ASCII in UTF-16: boot managers often pass binary data there. */
static uint32_t load_options_cmdline(void)
{
    const CHAR16 *s = g_uefi.li->LoadOptions;
    uint32_t n = g_uefi.li->LoadOptionsSize / 2, len = 0;

    if (!s || n > CMDLINE_MAX)
        return 0;
    while (len < n && s[len]) {
        if (s[len] < 0x20 || s[len] > 0x7E)
            return 0;
        g_load_options[len] = (char)s[len];
        len++;
    }
    return len;
}

static void report_config_errors(void)
{
    for (uint32_t i = 0; i < g_cfg.nerrors && i < CFG_MAX_ERRORS; i++)
        boot_error_line("BOOT.CFG", g_cfg.error[i].line, g_cfg.error[i].msg);
    if (g_cfg.nerrors > CFG_MAX_ERRORS)
        boot_error_more("BOOT.CFG", g_cfg.nerrors - CFG_MAX_ERRORS);
}

/* BOOT.CFG is optional. Without a usable one the loader boots
 * DEFAULT_KERNEL with the firmware load options as command line, which is
 * how a firmware boot entry (efibootmgr, bcdedit) passes arguments. */
static void load_config(void)
{
    struct kfile f = {0};
    struct cfg_entry *e;
    EFI_STATUS s;

    s = kfile_open(&f, CONFIG_PATH);
    if (!EFI_ERROR(s)) {
        if (f.size > CONFIG_MAX)
            s = EFI_BUFFER_TOO_SMALL;
        else if (!EFI_ERROR(s = kfile_read(&f, 0, g_cfg_text, f.size)))
            cfg_parse(g_cfg_text, f.size, &g_cfg);
    }
    kfile_close(&f);
    S.cfg = &g_cfg;

    if (s == EFI_BUFFER_TOO_SMALL)
        boot_error(L"BOOT.CFG is larger than 16 KiB; ignored", NULL, 0);
    else if (EFI_ERROR(s) && s != EFI_NOT_FOUND)
        boot_error(L"cannot read BOOT.CFG", NULL, s);
    if (!EFI_ERROR(s)) {
        if (g_cfg.nentries) {
            blog_str("Read configuration ");
            blog_str(CONFIG_PATH);
            blog_str(" (");
            blog_dec(g_cfg.nentries);
            blog_str(g_cfg.nentries == 1 ? " entry)" : " entries)");
            blog_end(TAG_OK);
        }
        report_config_errors();
        if (g_cfg.nentries) {
            S.have_cfg = true;
            return;
        }
        boot_error(L"BOOT.CFG has no usable entry; using \\EFI\\MYBOOT\\KERNEL.ELF", NULL, 0);
    }

    if (s == EFI_NOT_FOUND) {
        blog_str("No ");
        blog_str(CONFIG_PATH);
        blog_str("; using the built-in entry");
        blog_end(TAG_INFO);
    }
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.auto_windows = 1;
    g_cfg.nentries = 1;
    e = &g_cfg.entry[0];
    e->title = "";
    e->kernel = DEFAULT_KERNEL;
    if ((e->cmdline_len = load_options_cmdline()))
        e->cmdline = g_load_options;
}

/*
 * Read headers (1 read), validate, allocate one physically contiguous block
 * for the whole image, then read each PT_LOAD straight into its final place
 * (1 read per segment, no intermediate buffer). Every byte of the block is
 * written exactly once: either file data or zero (BSS, gaps, page tails).
 */
static EFI_STATUS load_kernel(struct uefi_vol *vol, const char *path)
{
    EFI_BOOT_SERVICES *bs = g_uefi.bs;
    EFI_PHYSICAL_ADDRESS addr;
    uint64_t hdr_len, span, pages, kend, cursor;
    enum elf_error e;
    EFI_STATUS s;

    if (!cfg_resolve_path(path, kfile_sep(vol), S.kpath))
        return entry_error(L"kernel path too long", path, EFI_INVALID_PARAMETER);
    boot_progress(L"kernel", S.kpath);
    if (EFI_ERROR(s = kfile_open_on(&S.kf, vol, S.kpath)))
        return entry_error(L"cannot open kernel", S.kpath, s);
    hdr_len = S.kf.size < sizeof(g_hdr_buf) ? S.kf.size : sizeof(g_hdr_buf);
    if (EFI_ERROR(s = kfile_read(&S.kf, 0, g_hdr_buf, hdr_len)))
        return entry_error(L"kernel read failed", S.kpath, s);
    if ((e = elf_parse(g_hdr_buf, hdr_len, S.kf.size, &S.elf)) != ELF_OK) {
        boot_error(L"invalid kernel", elf_strerror(e), 0); /* the reason says it all */
        return EFI_LOAD_ERROR;
    }

    /* Over-allocate by (align - 4 KiB) so a block with the right physical
     * alignment exists inside, then give back the unused head and tail. */
    span = S.elf.vend - S.elf.vbase;
    pages = span / PAGE_SIZE + (S.elf.align / PAGE_SIZE - 1);
    if (EFI_ERROR(s = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr)))
        return entry_error(L"no memory for kernel", NULL, s);
    S.kphys = addr + ((S.elf.vbase - addr) & (S.elf.align - 1));
    S.kpages = span / PAGE_SIZE;
    kend = S.kphys + span;
    if (S.kphys > addr)
        bs->FreePages(addr, (S.kphys - addr) / PAGE_SIZE);
    if (addr + pages * PAGE_SIZE > kend)
        bs->FreePages(kend, (addr + pages * PAGE_SIZE - kend) / PAGE_SIZE);

    cursor = S.kphys;
    for (uint32_t i = 0; i < S.elf.nseg; i++) {
        const struct elf_segment *seg = &S.elf.seg[i];
        uint64_t dst = S.kphys + (seg->vaddr - S.elf.vbase);

        memset((void *)(uintptr_t)cursor, 0, dst - cursor);
        if (seg->filesz && EFI_ERROR(s = kfile_read(&S.kf, seg->offset, (void *)(uintptr_t)dst, seg->filesz)))
            return entry_error(L"kernel segment read failed", S.kpath, s);
        memset((void *)(uintptr_t)(dst + seg->filesz), 0, seg->memsz - seg->filesz);
        cursor = dst + seg->memsz;
    }
    memset((void *)(uintptr_t)cursor, 0, kend - cursor);
    kfile_close(&S.kf);
    boot_progress_done(S.kf.size);
    return EFI_SUCCESS;
}

/* The whole file in one read, straight into its own page allocation. */
static EFI_STATUS load_module(struct uefi_vol *vol, const struct cfg_module *m)
{
    struct loaded_module *lm = &S.mods[S.nmod];
    char path[CFG_RESOLVED_MAX];
    struct kfile f = {0};
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS s;

    if (!cfg_resolve_path(m->path, kfile_sep(vol), path))
        return entry_error(L"module path too long", m->path, EFI_INVALID_PARAMETER);
    boot_progress(L"module", path);
    if (EFI_ERROR(s = kfile_open_on(&f, vol, path))) {
        kfile_close(&f);
        return entry_error(L"cannot open module", path, s);
    }
    if (f.size > MODULE_MAX) {
        kfile_close(&f);
        return entry_error(L"module larger than 16 GiB", path, EFI_UNSUPPORTED);
    }
    lm->size = f.size;
    lm->pages = (f.size + PAGE_SIZE - 1) / PAGE_SIZE;
    lm->phys = 0;
    lm->string = m->string ? m->string : m->path;
    if (lm->pages) {
        if (EFI_ERROR(s = g_uefi.bs->AllocatePages(AllocateAnyPages, EfiLoaderData, lm->pages, &addr))) {
            kfile_close(&f);
            return entry_error(L"no memory for module", path, s);
        }
        lm->phys = addr;
    }
    S.nmod++; /* owned by S from here: release_entry frees it */
    s = lm->size ? kfile_read(&f, 0, (void *)(uintptr_t)lm->phys, lm->size) : EFI_SUCCESS;
    kfile_close(&f);
    if (EFI_ERROR(s))
        return entry_error(L"module read failed", path, s);
    memset((void *)(uintptr_t)(lm->phys + lm->size), 0, lm->pages * PAGE_SIZE - lm->size);
    S.mod_bytes += lm->size;
    boot_progress_done(lm->size);
    return EFI_SUCCESS;
}

/* Entries found on disk rather than in BOOT.CFG. Needs every volume opened,
 * so the menu calls it only when it is about to be shown. */
static void add_auto_entries(void)
{
    chain_add_windows(&g_cfg);
}

/* MYBOOT-protocol entries are loaded here and started by efi_main. Linux
 * and EFI entries are started by chain_boot, which returns only if that
 * failed; `started` then says whether the program ran and came back. */
static EFI_STATUS load_entry(uint32_t idx, const char *cmdline, uint32_t cmdline_len, bool *started)
{
    struct cfg_entry *e = &g_cfg.entry[idx];
    struct uefi_vol *vol;
    EFI_STATUS s;

    S.entry = idx;
    S.cmdline = cmdline;
    S.cmdline_len = cmdline_len;
    if (cmdline_len) {
        blog_str("Command line: ");
        blog_ascii(cmdline, cmdline_len);
        blog_end(TAG_INFO);
    }
    if (!(vol = vol_for_entry(e)))
        return EFI_NOT_FOUND;
    if (e->type != CFG_KERNEL)
        return chain_boot(e, vol, cmdline, cmdline_len, started);
    if (EFI_ERROR(s = load_kernel(vol, e->kernel)))
        return s;
    for (uint32_t i = 0; i < e->nmod; i++)
        if (EFI_ERROR(s = load_module(vol, &e->mod[i])))
            return s;
    return EFI_SUCCESS;
}

/* The chosen entry (with its command line as edited at the menu), then
 * every other entry in order. Stops at the first that loads, or at a
 * chainloaded program that ran and returned: falling back to a different
 * system after that would surprise the user, so the recovery screen
 * decides instead. */
static EFI_STATUS load_entries(const struct menu_choice *c)
{
    EFI_STATUS s = EFI_LOAD_ERROR;

    for (uint32_t i = 0; i < g_cfg.nentries; i++) {
        uint32_t idx = (c->entry + i) % g_cfg.nentries;
        const struct cfg_entry *e = &g_cfg.entry[idx];

        if (i) {
            blog_str("Falling back to entry ");
            blog_dec(idx + 1);
            blog_str(": ");
        } else {
            blog_str("Booting ");
        }
        blog_str("'");
        blog_ascii(e->title[0] ? e->title : e->kernel, CFG_PATH_MAX);
        blog_str("'");
        blog_end(TAG_INFO);
        bool edited = i == 0 && c->cmdline, started = false;
        s = load_entry(idx, edited ? c->cmdline : e->cmdline,
                       edited ? c->cmdline_len : (e->cmdline ? e->cmdline_len : 0), &started);
        if (!EFI_ERROR(s))
            return EFI_SUCCESS;
        release_entry();
        if (started)
            break;
    }
    return s;
}

static void copy_vendor(char *dst, size_t cap, const CHAR16 *src)
{
    for (size_t i = 0; src && i + 1 < cap && src[i]; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
}

/* dst is zeroed by the caller; at most cap - 1 bytes are copied. */
static void copy_ascii(char *dst, size_t cap, const char *src)
{
    for (size_t i = 0; i + 1 < cap && src[i]; i++)
        dst[i] = src[i];
}

static bool map_kernel(void)
{
    for (uint32_t i = 0; i < S.elf.nseg; i++) {
        const struct elf_segment *seg = &S.elf.seg[i];
        uint64_t v0 = align_down(seg->vaddr, PAGE_SIZE);
        uint64_t v1 = align_up(seg->vaddr + seg->memsz, PAGE_SIZE);
        uint32_t prot = ((seg->flags & PF_W) ? PT_W : 0) | ((seg->flags & PF_X) ? PT_X : 0);

        if (!pt_map(&S.pt, v0, S.kphys + (v0 - S.elf.vbase), v1 - v0, prot, PAGE_SIZE))
            return false;
    }
    return true;
}

/* Kernel and module ranges, sorted by base, for uefi_mmap_normalize. */
static void build_carves(void)
{
    S.carve[0] = (struct mmap_carve){S.kphys, S.kphys + S.kpages * PAGE_SIZE, BOOT_MEM_KERNEL};
    S.ncarve = 1;
    for (uint32_t i = 0; i < S.nmod; i++) {
        struct mmap_carve c = {S.mods[i].phys, S.mods[i].phys + S.mods[i].pages * PAGE_SIZE, BOOT_MEM_MODULE};
        uint32_t j;

        if (!S.mods[i].pages)
            continue;
        j = S.ncarve++;
        while (j > 0 && S.carve[j - 1].base > c.base) {
            S.carve[j] = S.carve[j - 1];
            j--;
        }
        S.carve[j] = c;
    }
}

static void fill_boot_info(struct boot_info *bi, const char *cmdline, uint64_t tsc_entry)
{
    memset(bi, 0, sizeof(*bi));
    bi->magic = BOOT_INFO_MAGIC;
    bi->version = BOOT_INFO_VERSION;
    bi->size = sizeof(*bi);
    bi->flags = BOOT_F_UEFI | (S.la57 ? BOOT_F_LA57 : 0) | (S.nx ? BOOT_F_NX : 0) |
                (S.hhdm_1g ? BOOT_F_HHDM_1G : 0);
    bi->hhdm_base = BOOT_HHDM_BASE;
    bi->hhdm_size = S.hhdm_size;
    bi->kernel_phys_base = S.kphys;
    bi->kernel_virt_base = S.elf.vbase;
    bi->kernel_size = S.elf.vend - S.elf.vbase;
    bi->mmap = (uintptr_t)S.mmap;
    bi->mmap_entry_size = sizeof(struct boot_mem_entry);
    bi->cmdline = S.cmdline_len ? (uintptr_t)cmdline : 0;
    bi->acpi_rsdp = S.acpi->rsdp;
    bi->acpi_revision = (uint8_t)S.acpi->revision;
    bi->smbios_entry = S.smbios->entry;
    bi->smbios_major = S.smbios->major;
    bi->smbios_minor = S.smbios->minor;
    bi->uefi_system_table = (uintptr_t)g_uefi.st;
    bi->firmware_type = 1;
    bi->uefi_revision = g_uefi.st->Hdr.Revision;
    bi->firmware_revision = g_uefi.st->FirmwareRevision;
    bi->loader_reads = g_file_stats.reads;
    bi->loader_bytes_read = g_file_stats.bytes;
    bi->tsc_loader_entry = tsc_entry;
    copy_vendor(bi->firmware_vendor, sizeof(bi->firmware_vendor), g_uefi.st->FirmwareVendor);
    memcpy(bi->loader_version, LOADER_VERSION, sizeof(LOADER_VERSION));
    bi->cpu = *S.cpu;
    bi->framebuffer = *S.fb;
    bi->boot_device = *S.dev;
    bi->csme = *S.csme;
    bi->pat = PAT_LINUX;
    bi->xcr0 = S.xcr0;
    bi->smp_count = S.smp_count;
    bi->smp_cpus = S.smp_count ? (uintptr_t)S.smp_cpus : 0;
    if (S.fb->base && S.fb->base >= S.hhdm_size)
        bi->flags |= BOOT_F_FB_WC;

    memset(S.bmods, 0, (uint64_t)S.nmod * sizeof(struct boot_module));
    for (uint32_t i = 0; i < S.nmod; i++) {
        S.bmods[i].base = S.mods[i].phys;
        S.bmods[i].size = S.mods[i].size;
        copy_ascii(S.bmods[i].string, sizeof(S.bmods[i].string), S.mods[i].string);
    }
    bi->modules = S.nmod ? (uintptr_t)S.bmods : 0;
    bi->module_count = S.nmod;
    bi->module_entry_size = sizeof(struct boot_module);
    bi->efi_mmap = (uintptr_t)S.mem->map.buf; /* size fields: after the final read */
    if (S.have_cfg) {
        bi->boot_entry = S.entry + 1;
        copy_ascii(bi->boot_entry_title, sizeof(bi->boot_entry_title), g_cfg.entry[S.entry].title);
    }
}

/* `word` as a whole space-separated word of the kernel command line. */
static bool cmdline_has(const char *word)
{
    size_t n = 0;

    while (word[n])
        n++;

    for (uint32_t i = 0; i + n <= S.cmdline_len; i++)
        if ((i == 0 || S.cmdline[i - 1] == ' ') && !memcmp(S.cmdline + i, word, n) &&
            (i + n == S.cmdline_len || S.cmdline[i + n] == ' '))
            return true;
    return false;
}

/* N of "key=N" (key including the '=') in the command line, or 0. */
static uint32_t cmdline_uint(const char *key)
{
    size_t n = 0;
    uint32_t v = 0;

    while (key[n])
        n++;
    for (uint32_t i = 0; i + n <= S.cmdline_len; i++)
        if ((i == 0 || S.cmdline[i - 1] == ' ') && !memcmp(S.cmdline + i, key, n)) {
            for (i += (uint32_t)n; i < S.cmdline_len && S.cmdline[i] >= '0' && S.cmdline[i] <= '9'; i++)
                v = v * 10 + (uint32_t)(S.cmdline[i] - '0');
            return v;
        }
    return 0;
}

/* The loader's own IDT from ExitBootServices to the handoff: an NMI, a
 * machine check (broadcast to every CPU) or a fault now shows as a row of
 * red squares, vector + 1 of them, instead of a silent hang. */
struct __attribute__((packed)) idt_gate {
    uint16_t off0, sel;
    uint8_t  ist, type;
    uint16_t off1;
    uint32_t off2, zero;
};
static struct idt_gate g_idt[32];
extern const uint8_t exc_stubs[]; /* exc.S */
__attribute__((noreturn)) void exc_report(uint32_t vec);

static void install_idt(void)
{
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } r = {sizeof(g_idt) - 1, (uintptr_t)g_idt};
    uint16_t cs;

    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    for (uint32_t i = 0; i < 32; i++) {
        uint64_t a = (uintptr_t)exc_stubs + 16 * (uint64_t)i;

        g_idt[i] = (struct idt_gate){(uint16_t)a, cs, 0, 0x8E, (uint16_t)(a >> 16), (uint32_t)(a >> 32), 0};
    }
    __asm__ volatile("lidt %0" : : "m"(r));
}

#if BOOT_LOG
/* After ExitBootServices nothing can print, so progress shows as squares
 * in the top-left corner: one per step (green), then a row with one per
 * CPU (green: running or parked, red: did not start). The kernel paints
 * over them; if the boot stops, they say where. */
static void fb_square(uint32_t x, uint32_t y, uint32_t size, bool good)
{
    const struct boot_framebuffer *fb = S.fb;
    uint8_t r = good ? 0x40 : 0xFF, g = good ? 0xE0 : 0x40, b = 0x40;
    uint32_t px;

    if (!fb->base || x + size > fb->width || y + size > fb->height || fb->red_size > 8 || fb->green_size > 8 ||
        fb->blue_size > 8)
        return;
    px = ((uint32_t)(r >> (8 - fb->red_size)) << fb->red_shift) |
         ((uint32_t)(g >> (8 - fb->green_size)) << fb->green_shift) |
         ((uint32_t)(b >> (8 - fb->blue_size)) << fb->blue_shift);
    for (uint32_t j = 0; j < size; j++) {
        volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)(fb->base + (uint64_t)(y + j) * fb->pitch) + x;
        for (uint32_t i = 0; i < size; i++)
            row[i] = px;
    }
}

/* SFENCE: the framebuffer may be write-combining now; a square must be
 * on screen before whatever comes next can stop the CPU. */
static void stage(int n)
{
    fb_square(16 + (uint32_t)(n - 1) * 40, 16, 32, true);
    __asm__ volatile("sfence" : : : "memory");
}
/* Heartbeat ticks (one per ms of waiting) are a thin bar under the squares. */
static void smp_stage(int n)
{
    if (n >= SMP_STAGE_TICK) {
        fb_square(16 + (uint32_t)(n - SMP_STAGE_TICK) % 200 * 6, 52, 4, true);
        __asm__ volatile("sfence" : : : "memory");
    } else {
        stage(STAGE_SMP + n);
    }
}

/* The per-CPU row, then three rows counting the APs that reached the
 * trampoline's 16-, 32- and 64-bit code. */
static void show_cpus(void)
{
    uint32_t seen[3];

    for (uint32_t i = 0; i < S.smp_count; i++)
        fb_square(16 + (i % 64) * 20, 64 + (i / 64) * 20, 14, S.smp_cpus[i].state != BOOT_SMP_OFF);
    smp_tramp_seen(S.tramp_page, seen);
    for (uint32_t r = 0; r < 3; r++)
        for (uint32_t i = 0; i < seen[r] && i < 64; i++)
            fb_square(16 + i * 20, 104 + r * 20, 14, true);
    __asm__ volatile("sfence" : : : "memory");
}

void exc_report(uint32_t vec)
{
    for (uint32_t i = 0; i <= vec; i++)
        fb_square(16 + i * 40, 184, 32, false);
    __asm__ volatile("sfence" : : : "memory");
    for (;;)
        __asm__ volatile("cli; hlt");
}
#else
static void stage(int n) { (void)n; }
#define smp_stage NULL
static void show_cpus(void) {}

void exc_report(uint32_t vec)
{
    (void)vec;
    for (;;)
        __asm__ volatile("cli; hlt");
}
#endif

/* Linux's rule (arch/x86/kernel/smpboot.c): Intel family 6 from model 0x0F
 * (Core 2) and later families, and AMD from family 0x10, need no wait after
 * INIT; older CPUs get the SDM's 10 ms. */
static uint32_t init_delay_us(void)
{
    const struct boot_cpu *c = S.cpu;
    bool intel = !memcmp(c->vendor, "GenuineIntel", 12), amd = !memcmp(c->vendor, "AuthenticAMD", 12);

    if ((intel && ((c->family == 6 && c->model >= 0x0F) || c->family > 6)) || (amd && c->family >= 0x10))
        return 10;
    return 10000;
}

/* What every CPU runs the kernel with: SSE on, and XSAVE with x87, SSE,
 * AVX and AVX-512 where the CPU has them (CPUID 0Dh: what XCR0 may hold).
 * Applied after ExitBootServices, so firmware never sees it. */
static void plan_cpu_state(void)
{
    uint32_t r[4];

    S.cr0 = (read_cr0() & ~CR0_EM) | CR0_MP | CR0_NE | CR0_WP;
    S.cr4 = read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
    S.xcr0 = 0;
    if (S.cpu->features & BOOT_CPU_XSAVE) {
        uint64_t ok;

        cpuid(0x0D, 0, r);
        ok = r[0] | ((uint64_t)r[3] << 32);
        S.xcr0 = XCR0_X87 | XCR0_SSE;
        if ((S.cpu->features & BOOT_CPU_AVX) && (ok & XCR0_AVX))
            S.xcr0 |= XCR0_AVX;
        if ((S.xcr0 & XCR0_AVX) && (S.cpu->features & BOOT_CPU_AVX512F) && (ok & XCR0_AVX512) == XCR0_AVX512)
            S.xcr0 |= XCR0_AVX512;
        S.cr4 |= CR4_OSXSAVE;
    }
}

/* The AP list in the arena, and the trampoline in a page below 1 MiB.
 * Without the page (low memory taken) or a table root below 4 GiB, the
 * APs stay as firmware left them and the list says so. */
static void plan_smp(uint8_t *stacks, uint64_t gdtr_base, uint16_t gdtr_limit)
{
    struct smp_tramp_params p;
    EFI_PHYSICAL_ADDRESS page = 0x9F000; /* below the EBDA and VGA holes */

    for (uint32_t i = 0; i < S.smp_count; i++) {
        struct boot_smp_cpu *c = &S.smp_cpus[i];

        memset(c, 0, sizeof(*c));
        c->apic_id = S.smp->cpu[i].apic_id;
        c->acpi_uid = S.smp->cpu[i].acpi_uid;
        if (i == 0)
            c->state = BOOT_SMP_BSP;
        else
            c->stack_top = BOOT_HHDM_BASE + (uintptr_t)stacks + (uint64_t)i * SMP_AP_STACK_SIZE;
    }
    if (S.smp_count < 2 || S.nosmp || (uintptr_t)S.pt.root >= GiB(4) ||
        EFI_ERROR(g_uefi.bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &page)))
        return;
    S.tramp_page = page;
    p = (struct smp_tramp_params){
        .cr3 = (uintptr_t)S.pt.root,
        .cr0 = S.cr0,
        .cr4 = CR4_PAE | CR4_OSFXSR | CR4_OSXMMEXCPT | (S.la57 ? CR4_LA57 : 0) | (S.xcr0 ? CR4_OSXSAVE : 0),
        .efer_set = EFER_LME | (S.nx ? EFER_NXE : 0),
        .pat = PAT_LINUX,
        .xcr0 = S.xcr0,
        .gdtr_base = gdtr_base,
        .gdtr_limit = gdtr_limit,
        .cpus = BOOT_HHDM_BASE + (uintptr_t)S.smp_cpus,
        .ncpus = S.smp_count,
        .stop = S.smpstop,
    };
    smp_tramp_install(page, &p);
}

/*
 * One allocation holds everything the kernel receives. Its size is computed
 * up front from upper bounds, so nothing is reallocated:
 *   page tables | 64 KiB stack | boot_info | GDT | modules | memory map |
 *   cmdline | log | CPU list | AP stacks
 * It is placed below 4 GiB when possible: APs load CR3 in 32-bit mode.
 */
static EFI_STATUS prepare_handoff(uint64_t tsc_entry)
{
    uint64_t span = S.elf.vend - S.elf.vbase;
    uint64_t trampoline = (uintptr_t)boot_handoff & ~(uint64_t)(PAGE_SIZE - 1);
    EFI_PHYSICAL_ADDRESS addr;
    uint64_t size, *gdt, fb_lo = 0, fb_hi = 0;
    struct gdtr *gdtr;
    uint8_t *pool, *stack, *ap_stacks = NULL;
    char *cmdline;
    EFI_STATUS s;

    S.nx = (S.cpu->features & BOOT_CPU_NX) != 0;
    S.hhdm_1g = (S.cpu->features & BOOT_CPU_PDPE1GB) != 0;
    S.la57 = (read_cr4() & CR4_LA57) != 0;
    S.hhdm_size = align_up(S.mem->top, S.hhdm_1g ? GiB(1) : MiB(2));
    if (S.hhdm_size > HHDM_MAX)
        return fail(L"physical memory beyond 64 TiB", NULL, EFI_UNSUPPORTED);
    if (trampoline + 2 * PAGE_SIZE > LOW_HALF_END)
        return fail(L"loader image above the low canonical half", NULL, EFI_UNSUPPORTED);

    /* The framebuffer usually sits in the MMIO hole above RAM (Hyper-V:
     * 0xF8000000 with 1 GiB of RAM), outside the HHDM. Map the part beyond
     * hhdm_size at the same HHDM offset so "phys + hhdm_base" works for it. */
    if (S.fb->base) {
        fb_lo = align_down(S.fb->base, PAGE_SIZE);
        fb_hi = align_up(S.fb->base + S.fb->size, PAGE_SIZE);
        if (fb_lo < S.hhdm_size)
            fb_lo = S.hhdm_size;
        if (fb_hi > HHDM_MAX)
            return fail(L"framebuffer beyond 64 TiB", NULL, EFI_UNSUPPORTED);
        if (fb_hi < fb_lo)
            fb_hi = fb_lo;
    }
    build_carves();
    plan_cpu_state();
    if (!S.cpu->tsc_hz && S.smp->count > 1)
        calibrate_tsc(); /* AP startup times its waits with the TSC */
    S.smp_count = S.smp->count;
    S.nosmp = cmdline_has("nosmp");
    S.smpstop = (uint16_t)cmdline_uint("smpstop=");
    S.pt_bound = pt_pages_bound(span, S.hhdm_size, S.hhdm_1g, S.la57, fb_hi - fb_lo);
    /* Each carve can split one entry into three; +2 margin. */
    S.mmap_cap = uefi_mmap_capacity(&S.mem->map) + 2 * S.ncarve + 2;
    size = (uint64_t)S.pt_bound * PAGE_SIZE + KERNEL_STACK_SIZE + sizeof(struct boot_info) + 64 +
           (uint64_t)S.nmod * sizeof(struct boot_module) + 16 +
           (uint64_t)S.mmap_cap * sizeof(struct boot_mem_entry) + S.cmdline_len + 1 + CON_LOG_MAX + 64 +
           (uint64_t)S.smp_count * sizeof(struct boot_smp_cpu) + 64 +
           (S.smp_count > 1 ? (uint64_t)S.smp_count * SMP_AP_STACK_SIZE + PAGE_SIZE : 0);
    S.arena_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    addr = GiB(4) - 1;
    if (EFI_ERROR(g_uefi.bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, S.arena_pages, &addr)) &&
        EFI_ERROR(s = g_uefi.bs->AllocatePages(AllocateAnyPages, EfiLoaderData, S.arena_pages, &addr))) {
        S.arena_pages = 0;
        return fail(L"no memory for boot data", NULL, s);
    }
    S.arena.base = addr;
    S.arena.size = S.arena_pages * PAGE_SIZE;
    S.arena.used = 0;

    pool = arena_alloc(&S.arena, (uint64_t)S.pt_bound * PAGE_SIZE, PAGE_SIZE);
    stack = arena_alloc(&S.arena, KERNEL_STACK_SIZE, PAGE_SIZE);
    S.bi = arena_alloc(&S.arena, sizeof(struct boot_info), 16);
    gdt = arena_alloc(&S.arena, 3 * sizeof(uint64_t), 16);
    gdtr = arena_alloc(&S.arena, sizeof(struct gdtr), 16);
    S.bmods = arena_alloc(&S.arena, (uint64_t)S.nmod * sizeof(struct boot_module), 16);
    S.mmap = arena_alloc(&S.arena, (uint64_t)S.mmap_cap * sizeof(struct boot_mem_entry), 8);
    cmdline = arena_alloc(&S.arena, S.cmdline_len + 1, 1);
    S.log = arena_alloc(&S.arena, CON_LOG_MAX, 8);
    S.smp_cpus = arena_alloc(&S.arena, (uint64_t)S.smp_count * sizeof(struct boot_smp_cpu), 64);
    if (S.smp_count > 1) /* one stack per AP; the last slot is spare */
        ap_stacks = arena_alloc(&S.arena, (uint64_t)S.smp_count * SMP_AP_STACK_SIZE, PAGE_SIZE);
    if (!pool || !stack || !S.bi || !gdt || !gdtr || !S.bmods || !S.mmap || !cmdline || !S.log ||
        (S.smp_count && !S.smp_cpus) || (S.smp_count > 1 && !ap_stacks))
        return fail(L"boot data arena too small", NULL, EFI_OUT_OF_RESOURCES);

    if (!pt_init(&S.pt, pool, S.pt_bound, S.la57, S.nx) ||
        !pt_map(&S.pt, BOOT_HHDM_BASE, 0, S.hhdm_size, PT_W, S.hhdm_1g ? GiB(1) : MiB(2)) ||
        !map_kernel() ||
        (fb_hi > fb_lo && !pt_map(&S.pt, BOOT_HHDM_BASE + fb_lo, fb_lo, fb_hi - fb_lo, PT_W | PT_WC, MiB(2))) ||
        !pt_map(&S.pt, trampoline, trampoline, 2 * PAGE_SIZE, PT_X, PAGE_SIZE))
        return fail(L"page table construction failed", NULL, EFI_OUT_OF_RESOURCES);

    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFull; /* 0x08: present, DPL0, code, L=1 (64-bit) */
    gdt[2] = 0x00CF92000000FFFFull; /* 0x10: present, DPL0, data, writable */
    gdtr->limit = 3 * sizeof(uint64_t) - 1;
    gdtr->base = BOOT_HHDM_BASE + (uintptr_t)gdt;
    S.gdtr_virt = BOOT_HHDM_BASE + (uintptr_t)gdtr;
    S.stack_top_virt = BOOT_HHDM_BASE + (uintptr_t)stack + KERNEL_STACK_SIZE;

    /* After the page tables: the trampoline page joins them. Entry i's
     * stack is [(i - 1), i) * SMP_AP_STACK_SIZE into ap_stacks. */
    plan_smp(ap_stacks, gdtr->base, gdtr->limit);
    if (S.tramp_page && !pt_map(&S.pt, S.tramp_page, S.tramp_page, PAGE_SIZE, PT_W | PT_X, PAGE_SIZE))
        return fail(L"page table construction failed", NULL, EFI_OUT_OF_RESOURCES);

    if (S.cmdline_len)
        memcpy(cmdline, S.cmdline, S.cmdline_len);
    cmdline[S.cmdline_len] = 0;

    fill_boot_info(S.bi, cmdline, tsc_entry);
    return EFI_SUCCESS;
}

/* Final memory map and ExitBootServices with no allocation in between.
 * EFI_INVALID_PARAMETER means the map changed (a firmware event allocated
 * memory) after our read: read again into the same buffer and retry. */
static EFI_STATUS exit_firmware(void)
{
    EFI_STATUS s = EFI_LOAD_ERROR;

    S.bi->tsc_exit_start = rdtsc();
    for (S.ebs_attempts = 1; S.ebs_attempts <= EBS_MAX_ATTEMPTS; S.ebs_attempts++) {
        if (EFI_ERROR(s = uefi_mmap_read(&S.mem->map)))
            break;
        s = g_uefi.bs->ExitBootServices(g_uefi.image, S.mem->map.key);
        if (s != EFI_INVALID_PARAMETER)
            break;
    }
    return s == EFI_SUCCESS ? s : fail(L"ExitBootServices failed", NULL, s);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    /* First instruction of our code: the reference point for all timings. */
    uint64_t tsc_entry = rdtsc();
    EFI_STATUS s = EFI_LOAD_ERROR;
    struct menu_choice choice;
    uint32_t n;

    /* Without a valid table there is no console to report on: hand the
     * failure back to the firmware boot manager. */
    if (!system_table_valid(st))
        return EFI_LOAD_ERROR;
    g_uefi.image = image;
    g_uefi.st = st;
    g_uefi.bs = st->BootServices;
    con_init(st->ConOut);
    con_pick_mode();
    PROF_SET(PROF_ENTRY, tsc_entry);

    s = g_uefi.bs->HandleProtocol(image, &g_guid_loaded_image, (void **)&g_uefi.li);
    if (EFI_ERROR(s) || !g_uefi.li || !g_uefi.li->DeviceHandle)
        return fail(L"no loaded-image protocol", NULL, EFI_ERROR(s) ? s : EFI_LOAD_ERROR);

    S.cpu = discover_cpu();
    PROF_MARK(PROF_CPU);
#if BOOT_PROFILE
    if (!S.cpu->tsc_hz)
        calibrate_tsc();
#endif
    PROF_MARK(PROF_TSC_CAL);
    if (!(S.mem = discover_memory()))
        return fail(L"cannot read the memory map", NULL, EFI_LOAD_ERROR);
    PROF_MARK(PROF_MEMORY);
    S.acpi = discover_acpi();
    PROF_MARK(PROF_ACPI);
    S.smbios = discover_smbios();
    PROF_MARK(PROF_SMBIOS);
    S.fb = discover_graphics();
    PROF_MARK(PROF_GRAPHICS);
    S.dev = discover_storage();
    PROF_MARK(PROF_STORAGE);
    S.csme = discover_csme();
    S.csme_clients = discover_csme_clients();
    PROF_MARK(PROF_CSME);
    S.smp = discover_smp();
#if BOOT_LOG
    report_discovery(&S);
#endif

    load_config();
    menu_choose(&g_cfg, &choice, add_auto_entries);
    PROF_MARK(PROF_CONFIG);

    /* Until an entry loads: the chosen one, then the others in order as
     * fallbacks; if all fail, the recovery screen picks again or gives up. */
    while (choice.action == MENU_BOOT && EFI_ERROR(s = load_entries(&choice))) {
        menu_recover(&g_cfg, &choice, add_auto_entries);
        boot_errors_clear();
    }
    if (choice.action == MENU_RETURN) {
        release_resources();
        return EFI_ERROR(s) ? s : EFI_ABORTED;
    }
    PROF_MARK(PROF_KERNEL);
    if (EFI_ERROR(s = prepare_handoff(tsc_entry)))
        return s;
    PROF_MARK(PROF_PAGING);

#if BOOT_LOG
    report_handoff(&S);
#endif
    blog_str("Starting kernel ..."); /* the last output before firmware exits */
    blog_end(TAG_INFO);
    {
        uint32_t len;
        const char *log = con_log(&len);

        memcpy(S.log, log, len);
        S.bi->loader_log = (uintptr_t)S.log;
        S.bi->loader_log_size = len;
    }

    if (EFI_ERROR(s = exit_firmware()))
        return s;

    /* Firmware is gone: no console, no allocation, nothing may fail. The
     * firmware IDT lives in memory we just told the kernel is free. */
    cpu_cli();
    S.bi->efi_mmap_size = S.mem->map.size;
    S.bi->efi_mmap_desc_size = (uint32_t)S.mem->map.desc_size;
    S.bi->efi_mmap_desc_version = S.mem->map.desc_version;
    n = uefi_mmap_normalize(&S.mem->map, S.mmap, S.mmap_cap, S.carve, S.ncarve);
    S.bi->mmap_count = mmap_sort_merge(S.mmap, n);
    install_idt();
    stage(STAGE_EXITED);
    if (S.nx)
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE); /* before NX bits become live */
    write_cr0(S.cr0);                                /* WP: kernel R/O pages are R/O to CPL0 too; SSE */
    write_cr4(S.cr4);
    if (S.xcr0) {
        uint32_t r[4];

        xsetbv0(S.xcr0);
        cpuid(0x0D, 0, r); /* EBX: XSAVE area size for the XCR0 just set */
        S.bi->xsave_size = r[1];
    }
    stage(STAGE_CPU_STATE);
    /* PWT-only PTEs exist only in the kernel's tables, which are not live
     * yet; flush anything cached under the old meaning anyway. */
    wbinvd();
    wrmsr(MSR_PAT, PAT_LINUX);
    stage(STAGE_PAT);
    if (S.tramp_page) {
        bool x2apic;
        uint32_t self;

        S.bi->smp_parked =
            smp_start_aps(S.tramp_page, S.smp_cpus, S.smp_count, S.cpu->tsc_hz, init_delay_us(), &x2apic, &self,
                          smp_stage);
        S.bi->smp_flags = x2apic ? BOOT_SMP_F_X2APIC : 0;
        S.smp_cpus[0].apic_id = self; /* what the BSP's local APIC says, if CPUID disagreed */
        show_cpus();
        if (S.smpstop) /* time to read the rows before the kernel paints over them */
            for (uint64_t t0 = rdtsc(); rdtsc() - t0 < 5 * S.cpu->tsc_hz;)
                __asm__ volatile("pause");
    }
    stage(STAGE_HANDOFF);
    S.bi->tsc_handoff = rdtsc();
    boot_handoff((uintptr_t)S.pt.root, S.gdtr_virt, S.stack_top_virt,
                 BOOT_HHDM_BASE + (uintptr_t)S.bi, S.elf.entry);
}
