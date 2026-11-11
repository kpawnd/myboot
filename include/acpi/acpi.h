/* ACPI root discovery: validates the RSDP and the XSDT/RSDT it points to. */
#ifndef ACPI_ACPI_H
#define ACPI_ACPI_H

#include <stdbool.h>
#include <stdint.h>
#include "core/util.h"

struct acpi_info {
    uint64_t rsdp;
    uint64_t sdt;
    uint32_t revision;
    uint32_t entry_size;
    uint32_t count;
};

enum acpi_error { ACPI_OK = 0, ACPI_ERR_ADDR, ACPI_ERR_SIG, ACPI_ERR_CHECKSUM, ACPI_ERR_LENGTH };

enum acpi_error acpi_parse(uint64_t rsdp, phys_range_ok_fn ok, struct acpi_info *out);
uint64_t acpi_table(const struct acpi_info *a, uint32_t index);
bool acpi_table_sig(uint64_t table, phys_range_ok_fn ok, char sig[4]);
uint64_t acpi_find_table(const struct acpi_info *a, const char sig[4], phys_range_ok_fn ok, uint32_t *len);

#endif
