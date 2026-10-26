/*
 * ACPI 6.5 section 5.2.5 (RSDP) and 5.2.6-5.2.8 (SDT header, RSDT, XSDT).
 *
 * Validation order matters: a range is checked against the memory map
 * before it is read, and a length field is bounded before it is used to
 * size the next read.
 */
#include "acpi/acpi.h"
#include "lib/mem.h"

#define RSDP_V1_LEN   20u
#define RSDP_V2_LEN   36u
#define SDT_HDR_LEN   36u
#define SDT_MAX_LEN   KiB(64) /* thousands of tables; anything larger is corrupt */

static uint8_t sum8(const uint8_t *p, uint64_t n)
{
    uint8_t s = 0;
    while (n--)
        s = (uint8_t)(s + *p++);
    return s;
}

static enum acpi_error check_sdt(uint64_t addr, const char *sig, phys_range_ok_fn ok, uint32_t *len)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;

    if (!ok(addr, SDT_HDR_LEN))
        return ACPI_ERR_ADDR;
    if (memcmp(p, sig, 4) != 0)
        return ACPI_ERR_SIG;
    *len = rd32(p + 4);
    if (*len < SDT_HDR_LEN || *len > SDT_MAX_LEN)
        return ACPI_ERR_LENGTH;
    if (!ok(addr, *len))
        return ACPI_ERR_ADDR;
    return sum8(p, *len) == 0 ? ACPI_OK : ACPI_ERR_CHECKSUM;
}

enum acpi_error acpi_parse(uint64_t rsdp, phys_range_ok_fn ok, struct acpi_info *out)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)rsdp;
    enum acpi_error err;
    uint64_t sdt = 0;
    uint32_t len;

    memset(out, 0, sizeof(*out));
    if (!ok(rsdp, RSDP_V1_LEN))
        return ACPI_ERR_ADDR;
    if (memcmp(p, "RSD PTR ", 8) != 0)
        return ACPI_ERR_SIG;
    if (sum8(p, RSDP_V1_LEN) != 0) /* v1 checksum covers the first 20 bytes */
        return ACPI_ERR_CHECKSUM;

    out->revision = p[15];
    if (out->revision >= 2) {
        if (!ok(rsdp, RSDP_V2_LEN))
            return ACPI_ERR_ADDR;
        len = rd32(p + 20);
        if (len < RSDP_V2_LEN || len > 4096)
            return ACPI_ERR_LENGTH;
        if (!ok(rsdp, len))
            return ACPI_ERR_ADDR;
        if (sum8(p, len) != 0) /* extended checksum covers the whole RSDP */
            return ACPI_ERR_CHECKSUM;
        sdt = rd64(p + 24);
    }

    if (sdt) {
        out->entry_size = 8;
        err = check_sdt(sdt, "XSDT", ok, &len);
    } else {
        sdt = rd32(p + 16);
        out->entry_size = 4;
        err = check_sdt(sdt, "RSDT", ok, &len);
    }
    if (err != ACPI_OK)
        return err;

    out->rsdp = rsdp;
    out->sdt = sdt;
    out->count = (len - SDT_HDR_LEN) / out->entry_size;
    return ACPI_OK;
}

uint64_t acpi_table(const struct acpi_info *a, uint32_t index)
{
    const uint8_t *e = (const uint8_t *)(uintptr_t)a->sdt + SDT_HDR_LEN + (uint64_t)index * a->entry_size;

    if (index >= a->count)
        return 0;
    return a->entry_size == 8 ? rd64(e) : rd32(e);
}

bool acpi_table_sig(uint64_t table, phys_range_ok_fn ok, char sig[4])
{
    if (!table || !ok(table, SDT_HDR_LEN))
        return false;
    memcpy(sig, (const void *)(uintptr_t)table, 4);
    return true;
}
