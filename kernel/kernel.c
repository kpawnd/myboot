/*
 * MYBOOT test kernel. Verifies every promise of boot protocol v1 from the
 * receiving side, prints the boot_info it was given to COM1, and (only
 * under a hypervisor) exits QEMU through isa-debug-exit so tests can be
 * automated: exit status 33 = all checks passed, 35 = a check failed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bootinfo/bootinfo.h"
#include "arch/x86_64/cpu.h"
#include "font8x8.h"

#define COM1          0x3F8
#define QEMU_EXIT     0xF4
#define PTE_ADDR      0x000FFFFFFFFFF000ull

extern const char __kernel_text[], __kernel_rodata[], __kernel_data[];

/* Data and BSS probes: the loader must copy .data and zero .bss. */
static volatile uint32_t data_probe = 0x5A5AA5A5;
static volatile uint8_t bss_probe[4096];
static const char rodata_probe[] = "read-only";

/* Entry: capture RSP before C touches it, then tail-jump so kmain sees the
 * loader's fake return address exactly as if it had been called. */
__asm__(".section .text\n"
        ".globl _start\n"
        "_start:\n"
        "    movq %rsp, %rsi\n"
        "    jmp kmain\n");

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* ---- serial output ---------------------------------------------------- */

static void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* no interrupts */
    outb(COM1 + 3, 0x80); /* DLAB on */
    outb(COM1 + 0, 0x01); /* divisor 1: 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7); /* FIFO on, cleared */
    outb(COM1 + 4, 0x03); /* DTR, RTS */
}

static void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
        ;
    outb(COM1, (uint8_t)c);
}

/* ---- framebuffer console -------------------------------------------------
 * Text goes into a character grid in .bss; con_flush() draws the whole grid
 * once. Scrolling moves bytes in the grid, never pixels, because reading
 * back an uncached framebuffer is very slow. */

#define CELL_W   8
#define CELL_H   10 /* 8-pixel glyph + 2 pixels of leading */
#define CON_COLS 256
#define CON_ROWS 128

enum { COL_TEXT, COL_OK, COL_FAIL, COL_TITLE };

static uint8_t con_chars[CON_ROWS][CON_COLS];
static uint8_t con_color[CON_ROWS][CON_COLS];
static uint32_t con_cols, con_rows, con_x, con_y;
static uint8_t con_cur = COL_TEXT;

static void con_init(const struct boot_info *bi)
{
    const struct boot_framebuffer *fb = &bi->framebuffer;

    if (!fb->base || fb->bpp != 32)
        return;
    con_cols = fb->width / CELL_W < CON_COLS ? fb->width / CELL_W : CON_COLS;
    con_rows = fb->height / CELL_H < CON_ROWS ? fb->height / CELL_H : CON_ROWS;
}

static void con_newline(void)
{
    con_x = 0;
    if (++con_y < con_rows)
        return;
    for (uint32_t r = 1; r < con_rows; r++)
        for (uint32_t c = 0; c < con_cols; c++) {
            con_chars[r - 1][c] = con_chars[r][c];
            con_color[r - 1][c] = con_color[r][c];
        }
    for (uint32_t c = 0; c < con_cols; c++)
        con_chars[con_rows - 1][c] = 0;
    con_y = con_rows - 1;
}

static void con_putc(char c)
{
    if (!con_rows)
        return;
    if (c == '\n') {
        con_newline();
        return;
    }
    if (con_x == con_cols)
        con_newline();
    con_chars[con_y][con_x] = (uint8_t)c;
    con_color[con_y][con_x++] = con_cur;
}

static uint32_t rgb(const struct boot_framebuffer *fb, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t v = 0;
    if (fb->red_size)
        v |= (uint32_t)(r >> (8 - fb->red_size)) << fb->red_shift;
    if (fb->green_size)
        v |= (uint32_t)(g >> (8 - fb->green_size)) << fb->green_shift;
    if (fb->blue_size)
        v |= (uint32_t)(b >> (8 - fb->blue_size)) << fb->blue_shift;
    return v;
}

static void con_flush(const struct boot_info *bi)
{
    const struct boot_framebuffer *fb = &bi->framebuffer;
    uint8_t *base = (uint8_t *)(bi->hhdm_base + fb->base);
    uint32_t fg[4], bg = rgb(fb, 0, 0, 0);

    if (!con_rows)
        return;
    fg[COL_TEXT] = rgb(fb, 0xC0, 0xC0, 0xC0);
    fg[COL_OK] = rgb(fb, 0x40, 0xE0, 0x40);
    fg[COL_FAIL] = rgb(fb, 0xFF, 0x50, 0x50);
    fg[COL_TITLE] = rgb(fb, 0xFF, 0xFF, 0xFF);
    for (uint32_t y = 0; y < fb->height; y++) {
        volatile uint32_t *px = (volatile uint32_t *)(base + (uint64_t)y * fb->pitch);
        uint32_t row = y / CELL_H, gy = y % CELL_H;

        for (uint32_t x = 0; x < fb->width; x++) {
            uint32_t col = x / CELL_W;
            uint8_t ch = 0, bits = 0;

            if (row < con_rows && col < con_cols)
                ch = con_chars[row][col];
            if (ch >= 0x20 && ch < 0x7F && gy < 8)
                bits = font8x8_basic[ch][gy];
            px[x] = (bits >> (x % CELL_W)) & 1 ? fg[con_color[row][col]] : bg;
        }
    }
}

static void kputc(char c)
{
    serial_putc(c);
    con_putc(c);
}

static void kputs(const char *s) { while (*s) kputc(*s++); }

static void kputsn(const char *s, size_t n)
{
    for (size_t i = 0; i < n && s[i]; i++)
        kputc(s[i] >= 0x20 && s[i] < 0x7F ? s[i] : '?');
}

static void put_dec(uint64_t v)
{
    char buf[21];
    int i = 20;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    kputs(&buf[i]);
}

static void put_hex(uint64_t v, int digits)
{
    kputs("0x");
    for (int i = digits - 1; i >= 0; i--)
        kputc("0123456789ABCDEF"[(v >> (4 * i)) & 0xF]);
}

/* ---- checks ----------------------------------------------------------- */

static unsigned g_pass, g_fail;

static void check(bool ok, const char *what)
{
    con_cur = ok ? COL_OK : COL_FAIL;
    kputs(ok ? "  ok    " : "  FAIL  ");
    con_cur = COL_TEXT;
    kputs(what);
    kputc('\n');
    if (ok)
        g_pass++;
    else
        g_fail++;
}

struct mapping {
    bool present, writable, executable;
    uint64_t phys;
};

/* Walk the live page tables through the HHDM (Intel SDM 3A 4.5). */
static struct mapping lookup(const struct boot_info *bi, uint64_t va)
{
    struct mapping m = {false, true, true, 0};
    uint64_t cr3, table;
    int level = (bi->flags & BOOT_F_LA57) ? 5 : 4;

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    table = cr3 & PTE_ADDR;
    for (; level >= 1; level--) {
        unsigned shift = 12 + 9 * (unsigned)(level - 1);
        uint64_t e = ((volatile uint64_t *)(bi->hhdm_base + table))[(va >> shift) & 511];

        if (!(e & 1))
            return (struct mapping){0};
        m.writable = m.writable && (e & 2);
        if (e >> 63)
            m.executable = false;
        if (level == 1 || (level <= 3 && (e & 0x80))) {
            uint64_t mask = (1ull << shift) - 1;
            m.present = true;
            m.phys = ((e & PTE_ADDR) & ~mask) | (va & mask);
            return m;
        }
        table = e & PTE_ADDR;
    }
    return (struct mapping){0};
}

static const char *mem_type_name(uint32_t t)
{
    static const char *const names[] = {
        "?", "usable", "reserved", "acpi-reclaim", "acpi-nvs", "bad",
        "loader-reclaim", "kernel", "fw-runtime", "mmio", "persistent",
    };
    return t < sizeof(names) / sizeof(names[0]) ? names[t] : "?";
}

static void check_memory_map(const struct boot_info *bi)
{
    const struct boot_mem_entry *e = (const struct boot_mem_entry *)(bi->hhdm_base + bi->mmap);
    uint64_t bi_phys = (uint64_t)(uintptr_t)bi - bi->hhdm_base;
    uint64_t usable = 0, reclaim = 0;
    bool aligned = true, ordered = true, kernel_ok = false, bi_ok = false;

    kputs("memory map (");
    put_dec(bi->mmap_count);
    kputs(" entries):\n");
    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        uint64_t end = e[i].base + e[i].length;

        kputs("  ");
        put_hex(e[i].base, 16);
        kputs("-");
        put_hex(end - 1, 16);
        kputs(" ");
        kputs(mem_type_name(e[i].type));
        kputc('\n');

        if ((e[i].base | e[i].length) & 0xFFF || !e[i].length)
            aligned = false;
        if (i && e[i].base < e[i - 1].base + e[i - 1].length)
            ordered = false;
        if (e[i].type == BOOT_MEM_USABLE)
            usable += e[i].length;
        if (e[i].type == BOOT_MEM_BOOTLOADER_RECLAIMABLE) {
            reclaim += e[i].length;
            if (bi_phys >= e[i].base && bi_phys + sizeof(*bi) <= end)
                bi_ok = true;
        }
        if (e[i].type == BOOT_MEM_KERNEL && bi->kernel_phys_base >= e[i].base &&
            bi->kernel_phys_base + bi->kernel_size <= end)
            kernel_ok = true;
    }
    kputs("usable ");
    put_dec(usable >> 20);
    kputs(" MiB, loader-reclaimable ");
    put_dec(reclaim >> 10);
    kputs(" KiB\n");
    check(bi->mmap_count > 0 && bi->mmap_entry_size == sizeof(struct boot_mem_entry), "memory map present");
    check(aligned, "memory map entries 4 KiB aligned, non-empty");
    check(ordered, "memory map sorted, non-overlapping");
    check(kernel_ok, "kernel image covered by a KERNEL entry");
    check(bi_ok, "boot_info inside a BOOTLOADER_RECLAIMABLE entry");
}

static void check_machine_state(const struct boot_info *bi, uint64_t entry_rsp)
{
    uint64_t rflags, cr0, cr4, efer = rdmsr(MSR_EFER);
    uint16_t cs, ss;
    bool nx = (bi->flags & BOOT_F_NX) != 0;

    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ss, %0" : "=r"(ss));

    check(!(rflags & (1u << 9)), "interrupts disabled");
    check((entry_rsp + 8) % 16 == 0 && *(const uint64_t *)entry_rsp == 0,
          "stack aligned as after CALL, return address 0");
    check((cr0 & (1ull << 31)) && (cr0 & CR0_WP) && (cr4 & (1ull << 5)) && (efer & (1ull << 10)),
          "CR0.PG, CR0.WP, CR4.PAE, EFER.LMA set");
    check(!!(efer & EFER_NXE) == nx, "EFER.NXE matches BOOT_F_NX");
    check(!!(cr4 & CR4_LA57) == !!(bi->flags & BOOT_F_LA57), "CR4.LA57 matches BOOT_F_LA57");
    check(cs == 0x08 && ss == 0x10, "CS = 0x08, SS = 0x10");
}

/* First and last byte of the framebuffer must be mapped to themselves:
 * with no IDT, touching an unmapped page would triple-fault the machine. */
static bool fb_mapped(const struct boot_info *bi)
{
    const struct boot_framebuffer *fb = &bi->framebuffer;
    uint64_t last = fb->base + (uint64_t)fb->pitch * fb->height - 1;
    struct mapping a = lookup(bi, bi->hhdm_base + fb->base);
    struct mapping z = lookup(bi, bi->hhdm_base + last);

    return a.present && a.writable && a.phys == fb->base &&
           z.present && z.writable && z.phys == last;
}

static void check_mappings(const struct boot_info *bi)
{
    struct mapping t = lookup(bi, (uintptr_t)__kernel_text);
    struct mapping r = lookup(bi, (uintptr_t)__kernel_rodata);
    struct mapping d = lookup(bi, (uintptr_t)__kernel_data);
    struct mapping h = lookup(bi, bi->hhdm_base);
    bool nx = (bi->flags & BOOT_F_NX) != 0;
    bool bss_zero = true;

    for (size_t i = 0; i < sizeof(bss_probe); i++)
        bss_zero = bss_zero && bss_probe[i] == 0;

    check(t.present && !t.writable && t.executable, ".text mapped read-only, executable");
    check(r.present && !r.writable && (!nx || !r.executable), ".rodata mapped read-only, no-execute");
    check(d.present && d.writable && (!nx || !d.executable), ".data/.bss mapped writable, no-execute");
    check(t.phys == bi->kernel_phys_base + ((uintptr_t)__kernel_text - bi->kernel_virt_base) &&
          d.phys == bi->kernel_phys_base + ((uintptr_t)__kernel_data - bi->kernel_virt_base),
          "kernel physically contiguous at kernel_phys_base");
    check(data_probe == 0x5A5AA5A5 && rodata_probe[0] == 'r', ".data and .rodata loaded from the file");
    check(bss_zero, ".bss zeroed");
    check(h.present && h.phys == 0 && h.writable && (!nx || !h.executable), "HHDM maps physical 0, RW, NX");
    check((uintptr_t)bi >= bi->hhdm_base && (uintptr_t)bi < bi->hhdm_base + bi->hhdm_size,
          "boot_info pointer inside the HHDM");
    if (bi->framebuffer.base)
        check(fb_mapped(bi), "framebuffer mapped writable at hhdm_base + base");
}

static void check_firmware_tables(const struct boot_info *bi)
{
    if (bi->acpi_rsdp) {
        const char *p = (const char *)(bi->hhdm_base + bi->acpi_rsdp);
        bool ok = bi->acpi_rsdp + 8 <= bi->hhdm_size;
        for (int i = 0; ok && i < 8; i++)
            ok = p[i] == "RSD PTR "[i];
        check(ok, "ACPI RSDP readable through the HHDM");
    }
    if (bi->smbios_entry) {
        const char *p = (const char *)(bi->hhdm_base + bi->smbios_entry);
        bool ok = bi->smbios_entry + 5 <= bi->hhdm_size &&
                  p[0] == '_' && p[1] == 'S' && p[2] == 'M' && (p[3] == '_' || p[3] == '3');
        check(ok, "SMBIOS entry point readable through the HHDM");
    }
}

static void print_info(const struct boot_info *bi, uint64_t tsc_kernel)
{
    const struct boot_cpu *c = &bi->cpu;
    uint64_t hz = c->tsc_hz;

    kputs("loader         : MYBOOT ");
    kputsn(bi->loader_version, sizeof(bi->loader_version));
    kputs(", firmware ");
    kputsn(bi->firmware_vendor, sizeof(bi->firmware_vendor));
    kputs("\ncpu            : ");
    kputsn(c->brand[0] ? c->brand : c->vendor, c->brand[0] ? sizeof(c->brand) : sizeof(c->vendor));
    kputs("\nkernel         : virt ");
    put_hex(bi->kernel_virt_base, 16);
    kputs(" phys ");
    put_hex(bi->kernel_phys_base, 16);
    kputs(" size ");
    put_dec(bi->kernel_size >> 10);
    kputs(" KiB\nhhdm           : ");
    put_hex(bi->hhdm_base, 16);
    kputs(", ");
    put_dec(bi->hhdm_size >> 20);
    kputs(" MiB\nacpi rsdp      : ");
    put_hex(bi->acpi_rsdp, 8);
    kputs(" (revision ");
    put_dec(bi->acpi_revision);
    kputs(")\nsmbios         : ");
    put_hex(bi->smbios_entry, 8);
    kputs("\nframebuffer    : ");
    if (bi->framebuffer.base) {
        put_dec(bi->framebuffer.width);
        kputc('x');
        put_dec(bi->framebuffer.height);
        kputs(" at ");
        put_hex(bi->framebuffer.base, 8);
    } else {
        kputs("none");
    }
    kputs("\ncmdline        : ");
    kputs(bi->cmdline ? (const char *)(bi->hhdm_base + bi->cmdline) : "(none)");
    kputs("\nloader reads   : ");
    put_dec(bi->loader_reads);
    kputs(" reads, ");
    put_dec(bi->loader_bytes_read);
    kputs(" bytes\n");
    if (hz) {
        kputs("loader time    : ");
        put_dec((bi->tsc_handoff - bi->tsc_loader_entry) * 1000000 / hz);
        kputs(" us entry->kernel, of which ");
        put_dec((bi->tsc_handoff - bi->tsc_exit_start) * 1000000 / hz);
        kputs(" us final map + ExitBootServices\n");
    } else {
        /* Release loaders skip TSC calibration: report raw ticks. */
        kputs("loader time    : ");
        put_dec(bi->tsc_handoff - bi->tsc_loader_entry);
        kputs(" TSC ticks entry->kernel (frequency unknown)\n");
    }
    kputs("handoff jump   : ");
    put_dec(tsc_kernel - bi->tsc_handoff);
    kputs(" TSC ticks\n");
}

__attribute__((noreturn)) void kmain(const struct boot_info *bi, uint64_t entry_rsp)
{
    uint64_t tsc_kernel = rdtsc();
    bool valid = bi->magic == BOOT_INFO_MAGIC && bi->version == BOOT_INFO_VERSION &&
                 bi->size >= sizeof(*bi);

    serial_init();
    if (valid)
        con_init(bi); /* only trust framebuffer fields of a valid boot_info */
    con_cur = COL_TITLE;
    kputs("\nkernel: MYBOOT test kernel running\n");
    con_cur = COL_TEXT;
    check(valid, "boot_info magic/version/size");
    if (valid) {
        print_info(bi, tsc_kernel);
        check_machine_state(bi, entry_rsp);
        check_mappings(bi);
        check_memory_map(bi);
        check_firmware_tables(bi);
    }
    con_cur = g_fail ? COL_FAIL : COL_OK;
    kputs("kernel: ");
    put_dec(g_pass);
    kputs(" checks passed, ");
    put_dec(g_fail);
    kputs(g_fail ? " FAILED\n" : " failed\n");
    if (valid && con_rows && fb_mapped(bi))
        con_flush(bi);

    /* isa-debug-exit exists only in our QEMU setup; never poke an unknown
     * port on real hardware. */
    if (bi->magic == BOOT_INFO_MAGIC && (bi->cpu.features & BOOT_CPU_HYPERVISOR))
        outb(QEMU_EXIT, g_fail ? 0x11 : 0x10);
    cpu_cli();
    cpu_halt_forever();
}
