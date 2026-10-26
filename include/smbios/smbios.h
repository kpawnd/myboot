/*
 * SMBIOS entry-point validation and string lookup (DMTF DSP0134 3.7).
 * Zero-copy: strings are returned as pointers into the firmware table.
 */
#ifndef SMBIOS_SMBIOS_H
#define SMBIOS_SMBIOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/util.h"

struct smbios_info {
    uint64_t entry;     /* physical address of the entry point, 0 if none */
    uint64_t table;     /* physical address of the structure table */
    uint32_t table_len; /* bytes (3.x: maximum size) */
    uint8_t  major, minor;
};

enum smbios_error { SMBIOS_OK = 0, SMBIOS_ERR_ADDR, SMBIOS_ERR_SIG, SMBIOS_ERR_CHECKSUM, SMBIOS_ERR_LENGTH };

/* v3: entry is a 64-bit "_SM3_" entry point; otherwise 32-bit "_SM_". */
enum smbios_error smbios_parse(uint64_t entry, bool v3, phys_range_ok_fn ok, struct smbios_info *out);

/*
 * String `field` (byte offset of a string-number field) of the first
 * structure of `type`. Returns NULL if absent; *len receives the length.
 */
const char *smbios_string(const struct smbios_info *s, uint8_t type, uint8_t field, size_t *len);

#endif
