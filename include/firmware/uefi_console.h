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

#endif
