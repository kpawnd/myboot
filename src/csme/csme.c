/*
 * Intel CSME detection. Layouts follow Linux drivers/misc/mei/hw-me-regs.h
 * and hw-me.c (HECI registers, device IDs), drivers/misc/mei/bus-fixup.c
 * (the MKHI version reply), and coreboot's
 * soc/intel/common/block/include/intelblocks/cse.h (HFSTS1 fields).
 *
 * csme_probe only reads configuration space. csme_query_version is the one
 * place the loader writes to the device, and it is careful:
 *   - it asks only when HFSTS1 says the CSME is up in normal mode, and only
 *     through a memory BAR the firmware assigned (it never assigns one);
 *   - it uses the link only if the firmware's HECI driver left both sides
 *     ready and both buffers empty, and it never resets the link;
 *   - if memory decoding was off, it turns it on for the exchange and puts
 *     the Command register back afterwards, whatever happened.
 * Linux's mei_me driver resets the link when it probes, so the kernel sees
 * nothing of the exchange.
 */
#include <stdbool.h>
#include "csme/csme.h"
#include "lib/mem.h"

#define PCI_ID         0x00
#define PCI_COMMAND    0x04
#define PCI_CLASS_REV  0x08
#define PCI_BAR0       0x10
#define PCI_BAR0_HI    0x14
#define PCI_CMD_MEMORY 0x0002
#define VENDOR_INTEL   0x8086
#define CLASS_HECI     0x0780 /* communication controller, other */

/* HFSTS1. Bit 4 is left alone: manufacturing mode on older CSMEs, SPI
 * protection mode (the opposite sense) on newer ones. */
#define HFS1_STATE(v)  ((v) & 0xF)
#define HFS1_INIT_DONE (1u << 9)
#define HFS1_ERROR(v)  (((v) >> 12) & 0xF)
#define HFS1_MODE(v)   (((v) >> 16) & 0xF)
#define STATE_NORMAL   5
#define MODE_NORMAL    0

/* HECI registers, offsets into BAR0. Both CSRs have the same layout. */
#define H_CB_WW        0x00      /* host circular buffer: write window */
#define H_CSR          0x04
#define ME_CB_RW       0x08      /* ME circular buffer: read window */
#define ME_CSR_HA      0x0C
#define CSR_IS         (1u << 1) /* interrupt status, write 1 to clear */
#define CSR_IG         (1u << 2) /* interrupt generate: buffer written or read */
#define CSR_RDY        (1u << 3)
#define CSR_RST        (1u << 4)
#define CSR_RP(v)      (((v) >> 8) & 0xFF)
#define CSR_WP(v)      (((v) >> 16) & 0xFF)
#define CSR_DEPTH(v)   ((v) >> 24)

/* HECI message header */
#define HDR_ME_ADDR(v)   ((v) & 0xFF)
#define HDR_HOST_ADDR(v) (((v) >> 8) & 0xFF)
#define HDR_LEN(v)       (((v) >> 16) & 0x1FF)
#define HDR_COMPLETE     (1u << 31)

/* MKHI at its fixed address, the way firmware talks to it (host address 0) */
#define MKHI_ADDR       0x07
#define MKHI_GROUP_GEN  0xFF
#define MKHI_GET_FW_VER 0x02
#define MKHI_RESPONSE   0x80
#define REPLY_MIN       12 /* MKHI header + the code version block */
#define REPLY_MAX_DW    16 /* kept; longer replies are read and dropped */

#define TIMEOUT_US 1000000
#define POLL_US    50

static const uint16_t hfsts_off[6] = {0x40, 0x48, 0x60, 0x64, 0x68, 0x6C};

void csme_probe(const struct csme_ops *ops, struct boot_csme *out)
{
    uint32_t id = ops->cfg_read32(PCI_ID), cr, h1;

    memset(out, 0, sizeof(*out));
    if ((id & 0xFFFF) != VENDOR_INTEL)
        return;
    cr = ops->cfg_read32(PCI_CLASS_REV);
    if (cr >> 16 != CLASS_HECI)
        return;
    out->vendor_id = VENDOR_INTEL;
    out->device_id = (uint16_t)(id >> 16);
    out->revision_id = (uint8_t)cr;
    for (unsigned i = 0; i < 6; i++)
        out->hfsts[i] = ops->cfg_read32(hfsts_off[i]);
    h1 = out->hfsts[0];
    out->flags = BOOT_CSME_PRESENT | ((h1 & HFS1_INIT_DONE) ? BOOT_CSME_INIT_COMPLETE : 0);
    out->working_state = (uint8_t)HFS1_STATE(h1);
    out->error_code = (uint8_t)HFS1_ERROR(h1);
    out->op_mode = (uint8_t)HFS1_MODE(h1);
}

/* ---- the version query ------------------------------------------------- */

struct heci {
    const struct csme_ops *ops;
    uint64_t bar;
    uint32_t waited_us; /* one budget for the whole exchange */
};

/* Slots written and not yet read; -1 when the pointers cannot be right. */
static int filled(uint32_t csr)
{
    uint32_t n = (uint8_t)(CSR_WP(csr) - CSR_RP(csr));
    return n > CSR_DEPTH(csr) ? -1 : (int)n;
}

/* Tell the ME the host buffer was written, or the ME buffer read. IS is
 * write-1-to-clear: it is masked so pending status stays pending (as in
 * Linux's mei_hcsr_set). */
static void host_signal(struct heci *h)
{
    uint32_t csr = h->ops->mmio_read32(h->bar + H_CSR);
    h->ops->mmio_write32(h->bar + H_CSR, (csr & ~CSR_IS) | CSR_IG);
}

static uint8_t wait_filled(struct heci *h, uint32_t n)
{
    for (;;) {
        int f = filled(h->ops->mmio_read32(h->bar + ME_CSR_HA));

        if (f < 0)
            return BOOT_CSME_VER_BAD_REPLY;
        if ((uint32_t)f >= n)
            return BOOT_CSME_VER_OK;
        if (h->waited_us >= TIMEOUT_US)
            return BOOT_CSME_VER_TIMEOUT;
        h->ops->delay_us(POLL_US);
        h->waited_us += POLL_US;
    }
}

static uint8_t exchange(struct heci *h, struct boot_csme *out)
{
    const struct csme_ops *ops = h->ops;
    uint32_t hcsr = ops->mmio_read32(h->bar + H_CSR);
    uint32_t mcsr = ops->mmio_read32(h->bar + ME_CSR_HA);
    uint32_t hdr, len, n, d[REPLY_MAX_DW] = {0};
    uint8_t st;

    if (!(hcsr & CSR_RDY) || (hcsr & CSR_RST) || !(mcsr & CSR_RDY) || (mcsr & CSR_RST) ||
        CSR_DEPTH(hcsr) < 2)
        return BOOT_CSME_VER_NOT_READY;
    if (filled(hcsr) != 0 || filled(mcsr) != 0)
        return BOOT_CSME_VER_BUSY;

    ops->mmio_write32(h->bar + H_CB_WW, MKHI_ADDR | (4u << 16) | HDR_COMPLETE);
    ops->mmio_write32(h->bar + H_CB_WW, MKHI_GROUP_GEN | (MKHI_GET_FW_VER << 8));
    host_signal(h);

    if ((st = wait_filled(h, 1)) != BOOT_CSME_VER_OK)
        return st;
    hdr = ops->mmio_read32(h->bar + ME_CB_RW);
    len = HDR_LEN(hdr);
    n = (len + 3) / 4;
    if (n + 1 > CSR_DEPTH(mcsr)) /* header + body could never arrive in one piece */
        return BOOT_CSME_VER_BAD_REPLY;
    if ((st = wait_filled(h, n)) != BOOT_CSME_VER_OK)
        return st;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = ops->mmio_read32(h->bar + ME_CB_RW);
        if (i < REPLY_MAX_DW)
            d[i] = v;
    }
    host_signal(h);

    if (HDR_ME_ADDR(hdr) != MKHI_ADDR || HDR_HOST_ADDR(hdr) != 0 || !(hdr & HDR_COMPLETE) ||
        len < REPLY_MIN || (d[0] & 0xFF) != MKHI_GROUP_GEN ||
        ((d[0] >> 8) & 0xFF) != (MKHI_GET_FW_VER | MKHI_RESPONSE))
        return BOOT_CSME_VER_BAD_REPLY;
    if (d[0] >> 24)
        return BOOT_CSME_VER_REFUSED;
    /* Code version block: u16 minor, u8 major, u8 platform, u16 build,
     * u16 hotfix. Recovery and FITC version blocks follow; unused. */
    out->fw_minor = (uint16_t)d[1];
    out->fw_major = (uint8_t)(d[1] >> 16);
    out->fw_build = (uint16_t)d[2];
    out->fw_hotfix = (uint16_t)(d[2] >> 16);
    return BOOT_CSME_VER_OK;
}

void csme_query_version(const struct csme_ops *ops, struct boot_csme *out)
{
    struct heci h = {ops, 0, 0};
    uint32_t bar;
    uint16_t cmd;

    if (!(out->flags & BOOT_CSME_PRESENT))
        return;
    if (out->working_state != STATE_NORMAL || out->op_mode != MODE_NORMAL ||
        !(out->flags & BOOT_CSME_INIT_COMPLETE)) {
        out->version_status = BOOT_CSME_VER_NOT_NORMAL;
        return;
    }
    bar = ops->cfg_read32(PCI_BAR0);
    if ((bar & 1) || bar == 0xFFFFFFFF) { /* I/O BAR, or the device vanished */
        out->version_status = BOOT_CSME_VER_NO_BAR;
        return;
    }
    h.bar = bar & ~0xFull;
    if (((bar >> 1) & 3) == 2) /* 64-bit BAR */
        h.bar |= (uint64_t)ops->cfg_read32(PCI_BAR0_HI) << 32;
    if (!h.bar) {
        out->version_status = BOOT_CSME_VER_NO_BAR;
        return;
    }

    cmd = (uint16_t)ops->cfg_read32(PCI_COMMAND);
    if (!(cmd & PCI_CMD_MEMORY))
        ops->cfg_write16(PCI_COMMAND, cmd | PCI_CMD_MEMORY);
    out->version_status = exchange(&h, out);
    if (!(cmd & PCI_CMD_MEMORY))
        ops->cfg_write16(PCI_COMMAND, cmd);
    if (out->version_status == BOOT_CSME_VER_OK)
        out->flags |= BOOT_CSME_VERSION_VALID;
}

/* ---- names ---------------------------------------------------------------- */

/* HECI1 device IDs from Linux hw-me-regs.h, by chipset. */
static const struct { uint16_t id; const char *name; } platforms[] = {
    {0x9D3A, "Sunrise Point-LP"}, {0xA13A, "Sunrise Point-H"},  {0xA2BA, "Union Point"},
    {0x9DE0, "Cannon Point-LP"},  {0xA360, "Cannon Point-H"},   {0x02E0, "Comet Lake-LP"},
    {0x06E0, "Comet Lake-H"},     {0x34E0, "Ice Lake-LP"},      {0xA0E0, "Tiger Lake-LP"},
    {0x43E0, "Tiger Lake-H"},     {0x7AE8, "Alder Lake-S"},     {0x51E0, "Alder Lake-P"},
    {0x54E0, "Alder Lake-N"},     {0x7A68, "Raptor Lake-S"},    {0x7E70, "Meteor Lake"},
    {0x7770, "Arrow Lake-H"},     {0x7F68, "Arrow Lake-S"},     {0xA870, "Lunar Lake"},
    {0xE370, "Panther Lake-H"},   {0xE470, "Panther Lake-P"},   {0x4DE0, "Jasper Lake"},
    {0xA3BA, "Comet Lake-V"},     {0x06E4, "Comet Lake-H"},     {0x38E0, "Ice Lake-N"},
    {0x4B70, "Elkhart Lake"},     {0x1BE0, "Emmitsburg"},
};

const char *csme_platform_name(uint16_t device_id)
{
    for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++)
        if (platforms[i].id == device_id)
            return platforms[i].name;
    return NULL;
}

const char *csme_state_name(uint8_t s)
{
    static const char *const names[] = {
        [0] = "reset", [1] = "initializing", [2] = "recovery", [3] = "test",
        [4] = "disabled", [5] = "normal",
        [6] = "disable wait", [7] = "transition", [8] = "invalid",
    };
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : NULL;
}

const char *csme_mode_name(uint8_t m)
{
    static const char *const names[] = {
        [0] = "normal", [2] = "debug", [3] = "soft temporary disable",
        [4] = "security override (jumper)", [5] = "security override (HECI)",
    };
    return m < sizeof(names) / sizeof(names[0]) ? names[m] : NULL;
}

const char *csme_version_status_name(uint8_t s)
{
    static const char *const names[] = {
        [BOOT_CSME_VER_NONE] = "not asked",
        [BOOT_CSME_VER_OK] = "ok",
        [BOOT_CSME_VER_NOT_NORMAL] = "CSME not in normal state",
        [BOOT_CSME_VER_NO_BAR] = "no memory BAR assigned",
        [BOOT_CSME_VER_NOT_READY] = "HECI link not ready",
        [BOOT_CSME_VER_BUSY] = "HECI busy",
        [BOOT_CSME_VER_TIMEOUT] = "no reply within 1 s",
        [BOOT_CSME_VER_BAD_REPLY] = "malformed reply",
        [BOOT_CSME_VER_REFUSED] = "request refused",
    };
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : NULL;
}
