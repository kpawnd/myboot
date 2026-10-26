/*
 * SMBIOS: DSP0134 section 5.2.1 (32-bit entry point "_SM_"), 5.2.2 (64-bit
 * entry point "_SM3_") and 6.1 (structure layout: formatted area of
 * `length` bytes followed by NUL-terminated strings and a final NUL).
 */
#include "smbios/smbios.h"
#include "lib/mem.h"

#define SMBIOS_MAX_TABLE MiB(1)

static uint8_t sum8(const uint8_t *p, uint32_t n)
{
    uint8_t s = 0;
    while (n--)
        s = (uint8_t)(s + *p++);
    return s;
}

enum smbios_error smbios_parse(uint64_t entry, bool v3, phys_range_ok_fn ok, struct smbios_info *out)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)entry;
    uint32_t len;

    memset(out, 0, sizeof(*out));
    if (!ok(entry, v3 ? 24 : 31))
        return SMBIOS_ERR_ADDR;

    if (v3) {
        if (memcmp(p, "_SM3_", 5) != 0)
            return SMBIOS_ERR_SIG;
        len = p[6];
        if (len < 24 || len > 32 || !ok(entry, len))
            return SMBIOS_ERR_LENGTH;
        if (sum8(p, len) != 0)
            return SMBIOS_ERR_CHECKSUM;
        out->major = p[7];
        out->minor = p[8];
        out->table_len = rd32(p + 12);
        out->table = rd64(p + 16);
    } else {
        if (memcmp(p, "_SM_", 4) != 0 || memcmp(p + 16, "_DMI_", 5) != 0)
            return SMBIOS_ERR_SIG;
        len = p[5];
        if (len < 31 || len > 32 || !ok(entry, len))
            return SMBIOS_ERR_LENGTH;
        if (sum8(p, len) != 0 || sum8(p + 16, 15) != 0)
            return SMBIOS_ERR_CHECKSUM;
        out->major = p[6];
        out->minor = p[7];
        out->table_len = rd16(p + 22);
        out->table = rd32(p + 24);
    }
    if (out->table_len < 4 || out->table_len > SMBIOS_MAX_TABLE || !ok(out->table, out->table_len)) {
        memset(out, 0, sizeof(*out));
        return SMBIOS_ERR_LENGTH;
    }
    out->entry = entry;
    return SMBIOS_OK;
}

const char *smbios_string(const struct smbios_info *s, uint8_t type, uint8_t field, size_t *len)
{
    const uint8_t *t = (const uint8_t *)(uintptr_t)s->table;
    uint32_t off = 0, end = s->table_len;

    if (!s->entry)
        return NULL;
    while (off + 4 <= end) {
        uint8_t stype = t[off], slen = t[off + 1];
        uint32_t str, next;

        if (slen < 4 || off + slen > end)
            return NULL;
        /* The string set ends at the first double NUL after the formatted
         * area; a structure without strings still has two NULs. */
        for (next = off + slen; next + 1 < end && (t[next] || t[next + 1]); next++)
            ;
        if (next + 1 >= end)
            return NULL;

        if (stype == type) {
            uint8_t idx = field < slen ? t[off + field] : 0;
            if (idx == 0)
                return NULL;
            for (str = off + slen; str < next; ) {
                uint32_t n = 0;
                while (str + n < next + 1 && t[str + n])
                    n++;
                if (--idx == 0) {
                    *len = n;
                    return n ? (const char *)t + str : NULL;
                }
                str += n + 1;
            }
            return NULL;
        }
        if (stype == 127) /* end-of-table structure */
            return NULL;
        off = next + 2;
    }
    return NULL;
}
