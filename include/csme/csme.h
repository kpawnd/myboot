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

/* One ME client, from the HBM properties reply. */
struct csme_client {
    uint8_t  uuid[16];         /* as on the wire: first three fields little-endian */
    uint8_t  addr;             /* ME address */
    uint8_t  protocol_version;
    uint8_t  max_connections;
    uint8_t  fixed_address;    /* nonzero: reached without connecting */
    uint32_t max_msg_length;
};

#define CSME_MAX_CLIENTS 32

struct csme_clients {
    uint8_t  hbm_major, hbm_minor; /* the ME's HBM version */
    uint32_t count;                /* stored in c[]; boot_csme.client_count has all */
    struct csme_client c[CSME_MAX_CLIENTS];
};

/* Configuration-space reads only. Zeroes `out`, then fills it if a HECI
 * device answers. */
void csme_probe(const struct csme_ops *ops, struct boot_csme *out);

/* Ask the CSME for its firmware version: one MKHI message to its fixed
 * address. Call after csme_probe found one; sets out->version_status, and
 * fw_* on success. */
void csme_query_version(const struct csme_ops *ops, struct boot_csme *out);

/* An HBM session: start, enumerate the ME clients into `cl`, ask the MKHI
 * client for the firmware version through it, stop. Sets out->hbm_status,
 * mkhi_status and client_count, and fw_* if no version was read before. */
void csme_hbm_session(const struct csme_ops *ops, struct boot_csme *out, struct csme_clients *cl);

/* Names for the boot log; NULL when the value has no known name. */
const char *csme_platform_name(uint16_t device_id);
const char *csme_state_name(uint8_t working_state);
const char *csme_mode_name(uint8_t op_mode);
const char *csme_status_name(uint8_t status);
const char *csme_client_name(const uint8_t uuid[16]);

#endif
