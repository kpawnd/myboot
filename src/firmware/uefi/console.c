#include <stdbool.h>
#include "firmware/uefi_console.h"

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_con;

/* Copy of the boot messages for the kernel (boot_info.loader_log): plain
 * ASCII, '\n' line ends. Full-screen menus pause it, since their output is
 * drawing, not log. When full it keeps the beginning, which explains what
 * happened first, and marks the cut. */
static char     g_log[CON_LOG_MAX];
static uint32_t g_log_len;
static bool     g_log_paused, g_log_full;

void con_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out)
{
    g_con = out;
}

static void log_append(const CHAR16 *s)
{
    static const char cut[] = "[log truncated]\n";

    for (; *s && !g_log_full; s++) {
        if (*s == L'\r')
            continue;
        if (g_log_len + sizeof(cut) >= CON_LOG_MAX) {
            for (const char *c = cut; *c; c++)
                g_log[g_log_len++] = *c;
            g_log_full = true;
            break;
        }
        g_log[g_log_len++] = (*s == L'\n' || (*s >= 0x20 && *s < 0x7F)) ? (char)*s : '?';
    }
}

void con_puts(const CHAR16 *s)
{
    if (!g_log_paused)
        log_append(s);
    /* The spec types String as non-const, but OutputString never writes it. */
    g_con->OutputString(g_con, (CHAR16 *)s);
}

void con_log_pause(int paused)
{
    g_log_paused = paused != 0;
}

const char *con_log(uint32_t *len)
{
    *len = g_log_len;
    return g_log;
}

void con_put_dec(uint64_t v)
{
    CHAR16 buf[21]; /* 2^64-1 has 20 digits, plus the terminator */
    unsigned i = 20;

    buf[i] = 0;
    do {
        buf[--i] = (CHAR16)(L'0' + v % 10);
        v /= 10;
    } while (v);
    con_puts(&buf[i]);
}

void con_put_hex(uint64_t v)
{
    static const CHAR16 digits[] = L"0123456789ABCDEF";
    CHAR16 buf[19]; /* "0x" + 16 digits + terminator */
    unsigned i = 18;

    buf[i] = 0;
    do {
        buf[--i] = digits[v & 0xF];
        v >>= 4;
    } while (v);
    buf[--i] = L'x';
    buf[--i] = L'0';
    con_puts(&buf[i]);
}

void con_put_ascii(const char *s, size_t max)
{
    CHAR16 buf[65];
    size_t n = 0;

    for (size_t i = 0; i < max && s[i]; i++) {
        uint8_t c = (uint8_t)s[i];
        buf[n++] = (c >= 0x20 && c < 0x7F) ? c : L'?';
        if (n == 64) {
            buf[n] = 0;
            con_puts(buf);
            n = 0;
        }
    }
    if (n) {
        buf[n] = 0;
        con_puts(buf);
    }
}

void con_size(uint32_t *cols, uint32_t *rows)
{
    UINTN c = 0, r = 0;

    if (!g_con->Mode || EFI_ERROR(g_con->QueryMode(g_con, (UINTN)g_con->Mode->Mode, &c, &r)) ||
        c < 40 || r < 10 || c > 1000 || r > 1000) {
        c = 80;
        r = 25;
    }
    *cols = (uint32_t)c;
    *rows = (uint32_t)r;
}

void con_clear(void) { g_con->ClearScreen(g_con); }
void con_goto(uint32_t col, uint32_t row) { g_con->SetCursorPosition(g_con, col, row); }
void con_attr(uint32_t attr) { g_con->SetAttribute(g_con, attr); }
/* Not every console can hide its cursor; failure is harmless. */
void con_cursor(int visible) { g_con->EnableCursor(g_con, visible ? 1 : 0); }

void con_put_field(const char *s, size_t len, size_t width)
{
    CHAR16 buf[65];
    size_t n = 0;

    for (size_t i = 0; i < width; i++) {
        uint8_t c = (i < len && s[i]) ? (uint8_t)s[i] : ' ';
        if (i < len && !s[i])
            len = i; /* stop reading s at its NUL */
        buf[n++] = (c >= 0x20 && c < 0x7F) ? c : L'?';
        if (n == 64) {
            buf[n] = 0;
            con_puts(buf);
            n = 0;
        }
    }
    if (n) {
        buf[n] = 0;
        con_puts(buf);
    }
}
