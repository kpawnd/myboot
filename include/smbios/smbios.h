/* SMBIOS entry-point validation and string lookup (DMTF DSP0134 3.7). */
#ifndef SMBIOS_SMBIOS_H
#define SMBIOS_SMBIOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/util.h"

struct smbios_info {
    uint64_t entry;
    uint64_t table;
    uint32_t table_len;
    uint8_t  major, minor;
};

enum smbios_error { SMBIOS_OK = 0, SMBIOS_ERR_ADDR, SMBIOS_ERR_SIG, SMBIOS_ERR_CHECKSUM, SMBIOS_ERR_LENGTH };

enum smbios_error smbios_parse(uint64_t entry, bool v3, phys_range_ok_fn ok, struct smbios_info *out);

const char *smbios_string(const struct smbios_info *s, uint8_t type, uint8_t field, size_t *len);

#endif
