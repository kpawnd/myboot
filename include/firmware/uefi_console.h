/* Diagnostic output through UEFI ConOut. */
#ifndef FIRMWARE_UEFI_CONSOLE_H
#define FIRMWARE_UEFI_CONSOLE_H

#include <stddef.h>
#include <stdint.h>
#include "firmware/uefi.h"
#include "core/util.h"

void con_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out);
#define CON_PICK_ROWS 60
void con_pick_mode(void);

#define CON_MODES_MAX 16
struct con_mode { uint32_t number, cols, rows; };
uint32_t con_modes(const struct con_mode **modes, uint32_t *current);
void con_puts(const CHAR16 *s);

#define CON_LOG_MAX KiB(16)
void        con_log_pause(int paused);
const char *con_log(uint32_t *len);
void con_put_dec(uint64_t v);
void con_put_hex(uint64_t v);
void con_put_ascii(const char *s, size_t max);

void con_size(uint32_t *cols, uint32_t *rows);
void con_clear(void);
void con_goto(uint32_t col, uint32_t row);
void con_attr(uint32_t attr);
void con_cursor(int visible);
void con_put_field(const char *s, size_t len, size_t width);

#endif
