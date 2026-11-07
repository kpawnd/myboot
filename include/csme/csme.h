/*
 * Intel CSME detection through its host interface, HECI1 (PCI 00:16.0).
 * Firmware-independent: configuration space, MMIO and delays come from the
 * caller, so the same code runs against a simulated device on the host
 * (tests/host/csmetest.c).
 */
#ifndef CSME_CSME_H
#define CSME_CSME_H

#include <stdint.h>
#include "bootinfo/bootinfo.h"

struct csme_ops {
    uint32_t (*cfg_read32)(uint16_t off);              /* 00:16.0, off 4-aligned */
    void     (*cfg_write16)(uint16_t off, uint16_t v); /* 00:16.0, off 2-aligned */
    uint32_t (*mmio_read32)(uint64_t addr);
    void     (*mmio_write32)(uint64_t addr, uint32_t v);
    void     (*delay_us)(uint32_t us);
};

/* Configuration-space reads only. Zeroes `out`, then fills it if a HECI
 * device answers. */
void csme_probe(const struct csme_ops *ops, struct boot_csme *out);

/* Ask the CSME for its firmware version: one MKHI message over HECI. The
 * only function here that writes to the device. Call after csme_probe found
 * one; sets out->version_status, and fw_* on success. */
void csme_query_version(const struct csme_ops *ops, struct boot_csme *out);

/* Names for the boot log; NULL when the value has no known name. */
const char *csme_platform_name(uint16_t device_id);
const char *csme_state_name(uint8_t working_state);
const char *csme_mode_name(uint8_t op_mode);
const char *csme_version_status_name(uint8_t status);

#endif
