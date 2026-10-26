/*
 * Diagnostic output through UEFI ConOut. Valid only before ExitBootServices.
 *
 * Strings are passed as UTF-16 (L"" literals) straight to the firmware, so
 * there is no conversion and no buffer except small stack buffers for
 * numbers and ASCII firmware strings. Use "\r\n": ConOut does not translate.
 */
#ifndef FIRMWARE_UEFI_CONSOLE_H
#define FIRMWARE_UEFI_CONSOLE_H

#include <stddef.h>
#include <stdint.h>
#include "firmware/uefi.h"

void con_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out);
void con_puts(const CHAR16 *s);
void con_put_dec(uint64_t v);
void con_put_hex(uint64_t v); /* "0x" + minimal digits */
/* ASCII from firmware tables: stops at NUL or `max`, non-printables -> '?'. */
void con_put_ascii(const char *s, size_t max);

/* Full-screen drawing (menu, editor). Positions are 0-based; text never
 * wraps because callers pad or cut every line to the screen width. */
void con_size(uint32_t *cols, uint32_t *rows); /* 80x25 if the mode is unknown */
void con_clear(void);
void con_goto(uint32_t col, uint32_t row);
void con_attr(uint32_t attr);                  /* EFI_* colours */
void con_cursor(int visible);
/* Up to `len` ASCII chars of s, then spaces, `width` columns in total. */
void con_put_field(const char *s, size_t len, size_t width);

#endif
