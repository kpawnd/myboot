/* Intel CSME detection through its host interface, HECI1 (PCI 00:16.0). */
#ifndef CSME_CSME_H
#define CSME_CSME_H

#include <stdint.h>
#include "bootinfo/bootinfo.h"

struct csme_ops {
    uint32_t (*cfg_read32)(uint16_t off);
    void     (*cfg_write16)(uint16_t off, uint16_t v);
    uint32_t (*mmio_read32)(uint64_t addr);
    void     (*mmio_write32)(uint64_t addr, uint32_t v);
    void     (*delay_us)(uint32_t us);
};

struct csme_client {
    uint8_t  uuid[16];
    uint8_t  addr;
    uint8_t  protocol_version;
    uint8_t  max_connections;
    uint8_t  fixed_address;
    uint32_t max_msg_length;
};

#define CSME_MAX_CLIENTS 32

struct csme_clients {
    uint8_t  hbm_major, hbm_minor;
    uint32_t count;
    struct csme_client c[CSME_MAX_CLIENTS];
};

void csme_probe(const struct csme_ops *ops, struct boot_csme *out);

void csme_query_version(const struct csme_ops *ops, struct boot_csme *out);

void csme_hbm_session(const struct csme_ops *ops, struct boot_csme *out, struct csme_clients *cl);

const char *csme_platform_name(uint16_t device_id);
const char *csme_state_name(uint8_t working_state);
const char *csme_mode_name(uint8_t op_mode);
const char *csme_status_name(uint8_t status);
const char *csme_client_name(const uint8_t uuid[16]);

#endif
