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

/* One line in every build configuration: release builds print nothing
 * else, but an error the user cannot see is useless. Status 0 is omitted. */
static void report_error(const CHAR16 *what, const char *detail, EFI_STATUS status)
{
    con_puts(L"MYBOOT: error: ");
    con_puts(what);
    if (detail) {
        con_puts(L": ");
        con_put_ascii(detail, CFG_RESOLVED_MAX);
    }
    if (status) {
        con_puts(L" (status ");
        con_put_hex(status);
        con_puts(L")");
    }
    con_puts(L"\r\n");
}

static EFI_STATUS entry_error(const CHAR16 *what, const char *detail, EFI_STATUS status)
{
    report_error(what, detail, status);
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
    if (S.mem)
        uefi_mmap_free(&S.mem->map);
    S.arena_pages = 0;
}

/* Every fatal failure before ExitBootServices ends here: all our memory
 * released, and control returned to the firmware so its boot manager can
 * try the next boot option. */
static EFI_STATUS fail(const CHAR16 *what, const char *detail, EFI_STATUS status)
{
    report_error(what, detail, status);
    release_resources();
    g_uefi.bs->Stall(FAIL_DELAY_US);
    return status;
}

#if BOOT_PROFILE
/* Only reached when CPUID gives no TSC frequency (AMD, most VMs). Costs
 * TSC_CAL_US of wall time, so release builds skip it and report 0. */
static void calibrate_tsc(void)
{
    uint64_t t0 = rdtsc();
    g_uefi.bs->Stall(TSC_CAL_US);
    cpu_set_tsc_hz((rdtsc() - t0) * (1000000 / TSC_CAL_US), BOOT_TSC_CALIBRATED);
}
#endif

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
    for (uint32_t i = 0; i < g_cfg.nerrors && i < CFG_MAX_ERRORS; i++) {
        con_puts(L"MYBOOT: BOOT.CFG line ");
        con_put_dec(g_cfg.error[i].line);
        con_puts(L": ");
        con_put_ascii(g_cfg.error[i].msg, 80);
        con_puts(L"\r\n");
    }
    if (g_cfg.nerrors > CFG_MAX_ERRORS) {
        con_puts(L"MYBOOT: BOOT.CFG: ");
        con_put_dec(g_cfg.nerrors - CFG_MAX_ERRORS);
        con_puts(L" more errors\r\n");
    }
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
        report_error(L"BOOT.CFG is larger than 16 KiB; ignored", NULL, 0);
    else if (EFI_ERROR(s) && s != EFI_NOT_FOUND)
        report_error(L"cannot read BOOT.CFG", NULL, s);
    if (!EFI_ERROR(s)) {
        report_config_errors();
        if (g_cfg.nentries) {
            S.have_cfg = true;
            return;
        }
        report_error(L"BOOT.CFG has no usable entry; using \\EFI\\MYBOOT\\KERNEL.ELF", NULL, 0);
    }

    memset(&g_cfg, 0, sizeof(g_cfg));
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
static EFI_STATUS load_kernel(const char *path)
{
    EFI_BOOT_SERVICES *bs = g_uefi.bs;
    EFI_PHYSICAL_ADDRESS addr;
    uint64_t hdr_len, span, pages, kend, cursor;
    enum elf_error e;
    EFI_STATUS s;

    if (!cfg_resolve_path(path, S.kpath))
        return entry_error(L"kernel path too long", path, EFI_INVALID_PARAMETER);
    if (EFI_ERROR(s = kfile_open(&S.kf, S.kpath)))
        return entry_error(L"cannot open kernel", S.kpath, s);
    hdr_len = S.kf.size < sizeof(g_hdr_buf) ? S.kf.size : sizeof(g_hdr_buf);
    if (EFI_ERROR(s = kfile_read(&S.kf, 0, g_hdr_buf, hdr_len)))
        return entry_error(L"kernel read failed", S.kpath, s);
    if ((e = elf_parse(g_hdr_buf, hdr_len, S.kf.size, &S.elf)) != ELF_OK)
        return entry_error(L"invalid kernel", elf_strerror(e), EFI_LOAD_ERROR);

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
    return EFI_SUCCESS;
}

/* The whole file in one read, straight into its own page allocation. */
static EFI_STATUS load_module(const struct cfg_module *m)
{
    struct loaded_module *lm = &S.mods[S.nmod];
    char path[CFG_RESOLVED_MAX];
    struct kfile f = {0};
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS s;

    if (!cfg_resolve_path(m->path, path))
        return entry_error(L"module path too long", m->path, EFI_INVALID_PARAMETER);
    if (EFI_ERROR(s = kfile_open(&f, path))) {
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
    return EFI_SUCCESS;
}

static EFI_STATUS load_entry(uint32_t idx)
{
    const struct cfg_entry *e = &g_cfg.entry[idx];
    EFI_STATUS s;

    S.entry = idx;
    if (EFI_ERROR(s = load_kernel(e->kernel)))
        return s;
    for (uint32_t i = 0; i < e->nmod; i++)
        if (EFI_ERROR(s = load_module(&e->mod[i])))
            return s;
    S.cmdline = e->cmdline;
    S.cmdline_len = e->cmdline ? e->cmdline_len : 0;
    return EFI_SUCCESS;
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

/*
 * One allocation holds everything the kernel receives. Its size is computed
 * up front from upper bounds, so nothing is reallocated:
 *   page tables | 64 KiB stack | boot_info | GDT | modules | memory map | cmdline
 */
static EFI_STATUS prepare_handoff(uint64_t tsc_entry)
{
    uint64_t span = S.elf.vend - S.elf.vbase;
    uint64_t trampoline = (uintptr_t)boot_handoff & ~(uint64_t)(PAGE_SIZE - 1);
    EFI_PHYSICAL_ADDRESS addr;
    uint64_t size, *gdt, fb_lo = 0, fb_hi = 0;
    struct gdtr *gdtr;
    uint8_t *pool, *stack;
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
    S.pt_bound = pt_pages_bound(span, S.hhdm_size, S.hhdm_1g, S.la57, fb_hi - fb_lo);
    /* Each carve can split one entry into three; +2 margin. */
    S.mmap_cap = uefi_mmap_capacity(&S.mem->map) + 2 * S.ncarve + 2;
    size = (uint64_t)S.pt_bound * PAGE_SIZE + KERNEL_STACK_SIZE + sizeof(struct boot_info) + 64 +
           (uint64_t)S.nmod * sizeof(struct boot_module) + 16 +
           (uint64_t)S.mmap_cap * sizeof(struct boot_mem_entry) + S.cmdline_len + 1 + 64;
    S.arena_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (EFI_ERROR(s = g_uefi.bs->AllocatePages(AllocateAnyPages, EfiLoaderData, S.arena_pages, &addr))) {
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
    if (!pool || !stack || !S.bi || !gdt || !gdtr || !S.bmods || !S.mmap || !cmdline)
        return fail(L"boot data arena too small", NULL, EFI_OUT_OF_RESOURCES);

    if (!pt_init(&S.pt, pool, S.pt_bound, S.la57, S.nx) ||
        !pt_map(&S.pt, BOOT_HHDM_BASE, 0, S.hhdm_size, PT_W, S.hhdm_1g ? GiB(1) : MiB(2)) ||
        !map_kernel() ||
        (fb_hi > fb_lo && !pt_map(&S.pt, BOOT_HHDM_BASE + fb_lo, fb_lo, fb_hi - fb_lo, PT_W | PT_UC, MiB(2))) ||
        !pt_map(&S.pt, trampoline, trampoline, 2 * PAGE_SIZE, PT_X, PAGE_SIZE))
        return fail(L"page table construction failed", NULL, EFI_OUT_OF_RESOURCES);

    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFull; /* 0x08: present, DPL0, code, L=1 (64-bit) */
    gdt[2] = 0x00CF92000000FFFFull; /* 0x10: present, DPL0, data, writable */
    gdtr->limit = 3 * sizeof(uint64_t) - 1;
    gdtr->base = BOOT_HHDM_BASE + (uintptr_t)gdt;
    S.gdtr_virt = BOOT_HHDM_BASE + (uintptr_t)gdtr;
    S.stack_top_virt = BOOT_HHDM_BASE + (uintptr_t)stack + KERNEL_STACK_SIZE;

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
    uint32_t n, first;

    /* Without a valid table there is no console to report on: hand the
     * failure back to the firmware boot manager. */
    if (!system_table_valid(st))
        return EFI_LOAD_ERROR;
    g_uefi.image = image;
    g_uefi.st = st;
    g_uefi.bs = st->BootServices;
    con_init(st->ConOut);
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

    load_config();
    first = menu_choose(&g_cfg);
    PROF_MARK(PROF_CONFIG);

    /* The chosen entry first, then the others in order as fallbacks. */
    for (uint32_t i = 0; i < g_cfg.nentries; i++) {
        uint32_t idx = (first + i) % g_cfg.nentries;

        if (i) {
            con_puts(L"MYBOOT: trying entry ");
            con_put_dec(idx + 1);
            con_puts(L": ");
            con_put_ascii(g_cfg.entry[idx].title, CFG_TITLE_MAX);
            con_puts(L"\r\n");
        }
        if (!EFI_ERROR(s = load_entry(idx)))
            break;
        release_entry();
    }
    if (EFI_ERROR(s))
        return fail(g_cfg.nentries > 1 ? L"no entry could be loaded" : L"cannot boot", NULL, s);
    PROF_MARK(PROF_KERNEL);
    if (EFI_ERROR(s = prepare_handoff(tsc_entry)))
        return s;
    PROF_MARK(PROF_PAGING);

#if BOOT_LOG
    report_summary(&S);
#endif

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
    if (S.nx)
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE); /* before NX bits become live */
    write_cr0(read_cr0() | CR0_WP);                  /* kernel R/O pages are R/O to CPL0 too */
    S.bi->tsc_handoff = rdtsc();
    boot_handoff((uintptr_t)S.pt.root, S.gdtr_virt, S.stack_top_virt,
                 BOOT_HHDM_BASE + (uintptr_t)S.bi, S.elf.entry);
}
