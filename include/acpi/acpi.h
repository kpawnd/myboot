/*
 * ACPI root discovery: validates the RSDP and the XSDT/RSDT it points to.
 * Tables are read in place (zero-copy) and never written.
 */
#ifndef ACPI_ACPI_H
#define ACPI_ACPI_H

#include <stdbool.h>
#include <stdint.h>
#include "core/util.h"

struct acpi_info {
    uint64_t rsdp;       /* physical, 0 if absent/invalid */
    uint64_t sdt;        /* XSDT if available, else RSDT */
    uint32_t revision;   /* RSDP revision: 0 = ACPI 1.0, 2 = 2.0+ */
    uint32_t entry_size; /* 8 for XSDT, 4 for RSDT */
    uint32_t count;      /* table pointers in the SDT */
};

enum acpi_error { ACPI_OK = 0, ACPI_ERR_ADDR, ACPI_ERR_SIG, ACPI_ERR_CHECKSUM, ACPI_ERR_LENGTH };

enum acpi_error acpi_parse(uint64_t rsdp, phys_range_ok_fn ok, struct acpi_info *out);
uint64_t acpi_table(const struct acpi_info *a, uint32_t index);
/* Copies the 4-byte signature of the table at `table`; false if unreadable. */
bool acpi_table_sig(uint64_t table, phys_range_ok_fn ok, char sig[4]);

#endif
