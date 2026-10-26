/*
 * Boot messages, in every build configuration.
 *
 * Progress: "Loading kernel X ... 46 KiB", one line per file, so the user
 * sees what the loader is doing. If an error arrives while such a line is
 * open, the line ends in "failed" and the error follows on its own line.
 *
 * Errors: printed when they happen and kept, so the menu and the recovery
 * screen can show them again after clearing the screen. Each record holds
 * its formatted line, because the detail (often a path) usually lives in a
 * stack buffer.
 */
#include "firmware/uefi_boot.h"
#include "firmware/uefi_console.h"

static char g_err[BOOT_ERRORS_MAX][BOOT_ERROR_LINE];
static uint32_t g_nerr;
static bool g_dropped;
static bool g_line_open; /* a progress line awaits its result */

struct line {
    char  *buf;
    size_t n;
};

static void put_str(struct line *l, const char *s)
{
    for (; *s && l->n + 1 < BOOT_ERROR_LINE; s++)
        l->buf[l->n++] = *s;
}

/* UEFI messages are L"" literals of plain ASCII. */
static void put_wstr(struct line *l, const CHAR16 *s)
{
    for (; *s && l->n + 1 < BOOT_ERROR_LINE; s++)
        l->buf[l->n++] = *s < 0x7F ? (char)*s : '?';
}

static void put_num(struct line *l, uint64_t v, unsigned base)
{
    char d[20];
    unsigned i = 0;

    do {
        d[i++] = "0123456789ABCDEF"[v % base];
        v /= base;
    } while (v);
    if (base == 16)
        put_str(l, "0x");
    while (i && l->n + 1 < BOOT_ERROR_LINE)
        l->buf[l->n++] = d[--i];
}

/* The statuses file and memory operations return (UEFI Appendix D). */
static const char *status_name(EFI_STATUS s)
{
    switch (s & ~EFI_ERROR_BIT) {
    case 1:  return "load error";
    case 2:  return "invalid parameter";
    case 3:  return "unsupported";
    case 5:  return "buffer too small";
    case 7:  return "device error";
    case 9:  return "out of memory";
    case 10: return "volume corrupted";
    case 12: return "no media";
    case 13: return "media changed";
    case 14: return "not found";
    case 15: return "access denied";
    case 21: return "aborted";
    default: return NULL;
    }
}

static void record(struct line *l)
{
    if (g_line_open) {
        con_puts(L"failed\r\n");
        g_line_open = false;
    }
    l->buf[l->n] = 0;
    con_put_ascii(l->buf, BOOT_ERROR_LINE);
    con_puts(L"\r\n");
}

static struct line next_slot(void)
{
    static char overflow[BOOT_ERROR_LINE];

    if (g_nerr < BOOT_ERRORS_MAX)
        return (struct line){g_err[g_nerr++], 0};
    g_dropped = true;
    return (struct line){overflow, 0};
}

void boot_error(const CHAR16 *what, const char *detail, EFI_STATUS status)
{
    struct line l = next_slot();

    put_str(&l, "MYBOOT: error: ");
    put_wstr(&l, what);
    if (detail) {
        put_str(&l, ": ");
        put_str(&l, detail);
    }
    if (status) {
        put_str(&l, " (");
        if (EFI_ERROR(status) && status_name(status))
            put_str(&l, status_name(status));
        else {
            put_str(&l, "status ");
            put_num(&l, status, 16);
        }
        put_str(&l, ")");
    }
    record(&l);
}

void boot_error_line(const char *file, uint32_t line, const char *msg)
{
    struct line l = next_slot();

    put_str(&l, "MYBOOT: ");
    put_str(&l, file);
    put_str(&l, " line ");
    put_num(&l, line, 10);
    put_str(&l, ": ");
    put_str(&l, msg);
    record(&l);
}

void boot_error_more(const char *file, uint32_t count)
{
    struct line l = next_slot();

    put_str(&l, "MYBOOT: ");
    put_str(&l, file);
    put_str(&l, ": ");
    put_num(&l, count, 10);
    put_str(&l, " more error(s) not shown");
    record(&l);
}

void boot_progress(const CHAR16 *what, const char *path)
{
    con_puts(what);
    if (path) {
        con_puts(L" ");
        con_put_ascii(path, BOOT_ERROR_LINE);
    }
    con_puts(L" ... ");
    g_line_open = true;
}

void boot_progress_done(uint64_t bytes)
{
    char buf[BOOT_ERROR_LINE];
    struct line l = {buf, 0};

    if (bytes < 10240) {
        put_num(&l, bytes, 10);
        put_str(&l, " bytes");
    } else if (bytes < (1u << 20)) {
        put_num(&l, (bytes + 1023) >> 10, 10);
        put_str(&l, " KiB");
    } else {
        uint64_t tenths = (bytes * 10 + (1u << 19)) >> 20;
        put_num(&l, tenths / 10, 10);
        put_str(&l, ".");
        put_num(&l, tenths % 10, 10);
        put_str(&l, " MiB");
    }
    l.buf[l.n] = 0;
    con_put_ascii(l.buf, BOOT_ERROR_LINE);
    con_puts(L"\r\n");
    g_line_open = false;
}

uint32_t boot_errors_count(void)
{
    return g_nerr;
}

uint32_t boot_errors_draw(uint32_t row, uint32_t max_rows, uint32_t width)
{
    uint32_t n = 0;

    for (; n < g_nerr && n < max_rows; n++) {
        con_goto(0, row + n);
        con_put_field(g_err[n], BOOT_ERROR_LINE, width);
    }
    if (n < max_rows && (g_dropped || n < g_nerr)) {
        con_goto(0, row + n);
        con_put_field("MYBOOT: further errors not shown", 64, width);
        n++;
    }
    return n;
}

void boot_errors_clear(void)
{
    g_nerr = 0;
    g_dropped = false;
}
