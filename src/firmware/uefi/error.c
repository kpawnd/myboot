/*
 * The boot log, in every build configuration. One line per step, tagged the
 * way systemd tags its boot messages:
 *
 *   [  OK  ] Loaded kernel \EFI\MYBOOT\KERNEL.ELF (46 KiB)
 *   [FAILED] cannot open module: \EFI\MYBOOT\NOPE.MOD (not found)
 *   [ WARN ] BOOT.CFG line 3: unknown directive
 *            Booting 'Test kernel'
 *
 * Progress: a file load first shows "[  ..  ] Loading kernel X", cut to one
 * screen row, and is rewritten in place ("\r") as the OK or FAILED line once
 * the result is known. Only the final line goes into the log the kernel
 * receives.
 *
 * Errors and warnings are also kept, so the menu and the recovery screen
 * can show them again after clearing the screen. Each record holds its
 * formatted line, because the detail (often a path) usually lives in a
 * stack buffer.
 */
#include "firmware/uefi_boot.h"
#include "firmware/uefi_console.h"

#define TAG_LEN 9 /* "[  OK  ] " */

struct line {
    char     buf[BOOT_ERROR_LINE];
    uint32_t n;
    uint8_t  tag;
};

static struct line g_err[BOOT_ERRORS_MAX];
static uint32_t g_nerr;
static bool g_dropped;

static struct line g_line;   /* the blog_* line being built */
static struct line g_prog;   /* the open progress line: "kernel X" */
static bool g_prog_open;

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

static void put_size(struct line *l, uint64_t bytes)
{
    if (bytes < 10240) {
        put_num(l, bytes, 10);
        put_str(l, " bytes");
    } else if (bytes < (1u << 20)) {
        put_num(l, (bytes + 1023) >> 10, 10);
        put_str(l, " KiB");
    } else {
        uint64_t tenths = (bytes * 10 + (1u << 19)) >> 20;
        put_num(l, tenths / 10, 10);
        put_str(l, ".");
        put_num(l, tenths % 10, 10);
        put_str(l, " MiB");
    }
}

/* ---- output --------------------------------------------------------------- */

static void put_tag(uint8_t tag)
{
    static const struct { const CHAR16 *text; uint32_t attr; } tags[] = {
        [TAG_INFO]   = {L"         ", EFI_LIGHTGRAY},
        [TAG_OK]     = {L"  OK  ",    EFI_LIGHTGREEN},
        [TAG_WARN]   = {L" WARN ",    EFI_YELLOW},
        [TAG_FAILED] = {L"FAILED",    EFI_LIGHTRED},
        [TAG_BUSY]   = {L"  ..  ",    EFI_LIGHTGRAY},
    };

    if (tag == TAG_NONE)
        return;
    if (tag == TAG_INFO) {
        con_puts(tags[tag].text);
        return;
    }
    con_puts(L"[");
    con_attr(tags[tag].attr);
    con_puts(tags[tag].text);
    con_attr(EFI_LIGHTGRAY);
    con_puts(L"] ");
}

static void emit(const struct line *l)
{
    put_tag(l->tag);
    con_put_ascii(l->buf, l->n);
    con_puts(L"\r\n");
}

/* The open progress line, rewritten in place as FAILED. Out of the log:
 * the error that follows says the same with its reason. */
static void close_progress_failed(void)
{
    if (!g_prog_open)
        return;
    con_log_pause(1);
    con_puts(L"\r");
    put_tag(TAG_FAILED);
    con_puts(L"Loading ");
    con_put_ascii(g_prog.buf, g_prog.n);
    con_puts(L"\r\n");
    con_log_pause(0);
    g_prog_open = false;
}

/* ---- errors ----------------------------------------------------------------- */

static struct line *next_slot(uint8_t tag)
{
    static struct line overflow;
    struct line *l = &overflow;

    if (g_nerr < BOOT_ERRORS_MAX)
        l = &g_err[g_nerr++];
    else
        g_dropped = true;
    l->n = 0;
    l->tag = tag;
    return l;
}

static void record(struct line *l)
{
    l->buf[l->n] = 0;
    close_progress_failed();
    emit(l);
}

void boot_error(const CHAR16 *what, const char *detail, EFI_STATUS status)
{
    struct line *l = next_slot(TAG_FAILED);

    put_wstr(l, what);
    if (detail) {
        put_str(l, ": ");
        put_str(l, detail);
    }
    if (status) {
        const char *name = boot_status_name(status);

        put_str(l, " (");
        if (EFI_ERROR(status) && name)
            put_str(l, name);
        else {
            put_str(l, "status ");
            put_num(l, status, 16);
        }
        put_str(l, ")");
    }
    record(l);
}

void boot_error_line(const char *file, uint32_t line, const char *msg)
{
    struct line *l = next_slot(TAG_WARN);

    put_str(l, file);
    put_str(l, " line ");
    put_num(l, line, 10);
    put_str(l, ": ");
    put_str(l, msg);
    record(l);
}

void boot_error_more(const char *file, uint32_t count)
{
    struct line *l = next_slot(TAG_WARN);

    put_str(l, file);
    put_str(l, ": ");
    put_num(l, count, 10);
    put_str(l, " more error(s) not shown");
    record(l);
}

/* The statuses file and memory operations return (UEFI Appendix D). */
const char *boot_status_name(EFI_STATUS s)
{
    if (s & EFI_OEM_BIT) /* FS_STATUS: our own filesystem drivers */
        return fs_strerror((enum fs_error)(s & 0xFF));
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
    case 26: return "security violation: Secure Boot rejected it";
    default: return NULL;
    }
}

/* ---- progress ----------------------------------------------------------------- */

void boot_progress(const CHAR16 *what, const char *path)
{
    uint32_t cols, rows, room, len = 0;

    close_progress_failed();
    g_prog.n = 0;
    put_wstr(&g_prog, what);
    put_str(&g_prog, " ");
    put_str(&g_prog, path);
    g_prog.buf[g_prog.n] = 0;
    g_prog_open = true;

    /* One screen row, so "\r" returns to its start: keep the end of a long
     * path, which names the file. */
    con_size(&cols, &rows);
    room = cols - 1 - TAG_LEN - 8; /* "Loading " */
    while (path[len])
        len++;
    con_log_pause(1);
    put_tag(TAG_BUSY);
    con_puts(L"Loading ");
    con_puts(what);
    con_puts(L" ");
    for (const CHAR16 *w = what; *w; w++)
        room--;
    room--;
    if (len > room && room > 3) {
        con_puts(L"...");
        con_put_ascii(path + len - (room - 3), room - 3);
    } else {
        con_put_ascii(path, len);
    }
    con_log_pause(0);
}

void boot_progress_done(uint64_t bytes)
{
    if (!g_prog_open)
        return;
    g_prog_open = false;
    con_log_pause(1);
    con_puts(L"\r");
    con_log_pause(0);
    blog_str("Loaded ");
    blog_str(g_prog.buf);
    blog_str(" (");
    put_size(&g_line, bytes);
    blog_str(")");
    blog_end(TAG_OK);
}

/* ---- other lines ---------------------------------------------------------- */

void blog_str(const char *s) { put_str(&g_line, s); }
void blog_wstr(const CHAR16 *s) { put_wstr(&g_line, s); }
void blog_dec(uint64_t v) { put_num(&g_line, v, 10); }
void blog_hex(uint64_t v) { put_num(&g_line, v, 16); }
void blog_size(uint64_t bytes) { put_size(&g_line, bytes); }

/* ASCII from firmware tables or the config: non-printables become '?'. */
void blog_ascii(const char *s, size_t max)
{
    for (size_t i = 0; i < max && s[i] && g_line.n + 1 < BOOT_ERROR_LINE; i++)
        g_line.buf[g_line.n++] = (s[i] >= 0x20 && s[i] < 0x7F) ? s[i] : '?';
}

void blog_end(enum boot_tag tag)
{
    close_progress_failed();
    g_line.buf[g_line.n] = 0;
    g_line.tag = (uint8_t)tag;
    emit(&g_line);
    g_line.n = 0;
}

/* ---- kept errors -------------------------------------------------------- */

uint32_t boot_errors_count(void)
{
    return g_nerr;
}

uint32_t boot_errors_draw(uint32_t row, uint32_t max_rows, uint32_t width)
{
    uint32_t n = 0;

    if (width <= TAG_LEN)
        return 0;
    for (; n < g_nerr && n < max_rows; n++) {
        con_goto(0, row + n);
        put_tag(g_err[n].tag);
        con_put_field(g_err[n].buf, g_err[n].n, width - TAG_LEN);
    }
    if (n < max_rows && (g_dropped || n < g_nerr)) {
        con_goto(0, row + n);
        put_tag(TAG_WARN);
        con_put_field("further errors not shown", 64, width - TAG_LEN);
        n++;
    }
    return n;
}

void boot_errors_clear(void)
{
    g_nerr = 0;
    g_dropped = false;
}
