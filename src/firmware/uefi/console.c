#include "firmware/uefi_console.h"

/* The only console state: the protocol pointer from the system table. */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_con;

void con_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out)
{
    g_con = out;
}

void con_puts(const CHAR16 *s)
{
    /* The spec types String as non-const, but OutputString never writes it. */
    g_con->OutputString(g_con, (CHAR16 *)s);
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
