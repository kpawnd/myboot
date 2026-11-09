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
#include "core/util.h"

void con_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out);
/* Pick the firmware text mode. Most firmware drives the display at the
 * resolution that mode needs, so a mode as large as the panel avoids a
 * stretched 800x600 picture; but the firmware font is a fixed 8x19, so on
 * a high-resolution panel the largest mode makes text tiny. The choice is
 * the largest mode of at most CON_PICK_ROWS rows, else the largest of all.
 * Clears the screen when it changes the mode. */
#define CON_PICK_ROWS 60
void con_pick_mode(void);

/* The text modes con_pick_mode saw, for the boot log. */
#define CON_MODES_MAX 16
struct con_mode { uint32_t number, cols, rows; };
uint32_t con_modes(const struct con_mode **modes, uint32_t *current);
void con_puts(const CHAR16 *s);

/* Everything printed is also kept as ASCII for the kernel, except while
 * paused (full-screen menus). */
#define CON_LOG_MAX KiB(16)
void        con_log_pause(int paused);
const char *con_log(uint32_t *len); /* not NUL-terminated */
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
