/*
 * Intel CSME detection. Layouts follow Linux drivers/misc/mei/hw-me-regs.h
 * and hw-me.c (HECI registers, device IDs), hbm.h and hw.h (the HECI bus
 * messages), bus-fixup.c (client UUIDs, the MKHI version reply), and
 * coreboot's soc/intel/common/block/include/intelblocks/cse.h (HFSTS1
 * fields).
 *
 * csme_probe only reads configuration space. csme_query_version and
 * csme_hbm_session write to the device, and they are careful:
 *   - they ask only when HFSTS1 says the CSME is up in normal mode, and only
 *     through a memory BAR the firmware assigned (they never assign one);
 *   - they use the link only if the firmware's HECI driver left both sides
 *     ready and both buffers empty, and they never reset the link;
 *   - if memory decoding was off, they turn it on for the exchange and put
 *     the Command register back afterwards, whatever happened.
 * The HBM session ends with HOST_STOP, and Linux's mei_me driver resets the
 * link when it probes, so the kernel sees nothing of either.
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

/* MKHI */
#define MKHI_ADDR       0x07 /* its fixed address, the way firmware talks to it */
#define MKHI_GROUP_GEN  0xFF
#define MKHI_GET_FW_VER 0x02
#define MKHI_RESPONSE   0x80
#define REPLY_MIN       12 /* MKHI header + the code version block */
#define REPLY_MAX_DW    16 /* kept; longer replies are read and dropped */

/* HECI bus messages: ME address 0, host address 0. Byte 0 is the command;
 * a response is the request's command | 0x80. */
#define HBM_START       0x01
#define HBM_STOP        0x02
#define HBM_ENUM        0x04
#define HBM_PROPS       0x05
#define HBM_CONNECT     0x06
#define HBM_DISCONNECT  0x07
#define HBM_FLOW        0x08
#define HBM_RES         0x80
#define HBM_MAJOR       2
#define HBM_MINOR       0
#define HBM_ENUM_LEN    36 /* header + a 256-bit map of valid ME addresses */
#define HBM_PROPS_LEN   28 /* header + mei_client_properties */
#define HBM_FLOW_LEN    8
#define HOST_CLIENT     1  /* the one host address the loader uses */
#define MAX_SKIP        8  /* unrelated messages tolerated while waiting */

#define TIMEOUT_US 1000000
#define DRAIN_US   20000 /* for the optional HOST_STOP answer */
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

/* ---- the HECI link -------------------------------------------------------- */

struct heci {
    const struct csme_ops *ops;
    uint64_t bar;
    uint16_t cmd;       /* Command register as found */
    uint32_t waited_us; /* one budget for the whole call */
    int      credit;    /* ME address whose flow-control grant arrived early, -1 none */
};

static uint32_t rd(struct heci *h, uint32_t reg) { return h->ops->mmio_read32(h->bar + reg); }
static void wr(struct heci *h, uint32_t reg, uint32_t v) { h->ops->mmio_write32(h->bar + reg, v); }

/* Spend one poll interval; false once the budget is gone. */
static bool tick(struct heci *h)
{
    if (h->waited_us >= TIMEOUT_US)
        return false;
    h->ops->delay_us(POLL_US);
    h->waited_us += POLL_US;
    return true;
}

/* Slots written and not yet read; -1 when the pointers cannot be right. */
static int filled(uint32_t csr)
{
    uint32_t n = (uint8_t)(CSR_WP(csr) - CSR_RP(csr));
    return n > CSR_DEPTH(csr) ? -1 : (int)n;
}

static bool up(uint32_t csr) { return (csr & CSR_RDY) && !(csr & CSR_RST); }

/* Tell the ME the host buffer was written, or the ME buffer read. IS is
 * write-1-to-clear: it is masked so pending status stays pending (as in
 * Linux's mei_hcsr_set). */
static void host_signal(struct heci *h)
{
    wr(h, H_CSR, (rd(h, H_CSR) & ~CSR_IS) | CSR_IG);
}

/* Map BAR0 and turn memory decoding on; anything but OK leaves the device
 * untouched. */
static uint8_t heci_open(struct heci *h, const struct csme_ops *ops, const struct boot_csme *out)
{
    uint32_t bar;

    h->ops = ops;
    h->waited_us = 0;
    h->credit = -1;
    if (out->working_state != STATE_NORMAL || out->op_mode != MODE_NORMAL ||
        !(out->flags & BOOT_CSME_INIT_COMPLETE))
        return BOOT_CSME_ST_NOT_NORMAL;
    bar = ops->cfg_read32(PCI_BAR0);
    if ((bar & 1) || bar == 0xFFFFFFFF) /* I/O BAR, or the device vanished */
        return BOOT_CSME_ST_NO_BAR;
    h->bar = bar & ~0xFull;
    if (((bar >> 1) & 3) == 2) /* 64-bit BAR */
        h->bar |= (uint64_t)ops->cfg_read32(PCI_BAR0_HI) << 32;
    if (!h->bar)
        return BOOT_CSME_ST_NO_BAR;
    h->cmd = (uint16_t)ops->cfg_read32(PCI_COMMAND);
    if (!(h->cmd & PCI_CMD_MEMORY))
        ops->cfg_write16(PCI_COMMAND, h->cmd | PCI_CMD_MEMORY);
    return BOOT_CSME_ST_OK;
}

static void heci_close(struct heci *h)
{
    if (!(h->cmd & PCI_CMD_MEMORY))
        h->ops->cfg_write16(PCI_COMMAND, h->cmd);
}

/* Both sides ready and both buffers empty: the firmware left the link idle. */
static uint8_t link_idle(struct heci *h)
{
    uint32_t hcsr = rd(h, H_CSR), mcsr = rd(h, ME_CSR_HA);

    if (!up(hcsr) || !up(mcsr) || CSR_DEPTH(hcsr) < 2)
        return BOOT_CSME_ST_NOT_READY;
    if (filled(hcsr) != 0 || filled(mcsr) != 0)
        return BOOT_CSME_ST_BUSY;
    return BOOT_CSME_ST_OK;
}

static uint8_t wait_filled(struct heci *h, uint32_t n)
{
    for (;;) {
        uint32_t csr = rd(h, ME_CSR_HA);
        int f = filled(csr);

        if (!up(csr))
            return BOOT_CSME_ST_NOT_READY;
        if (f < 0)
            return BOOT_CSME_ST_BAD_REPLY;
        if ((uint32_t)f >= n)
            return BOOT_CSME_ST_OK;
        if (!tick(h))
            return BOOT_CSME_ST_TIMEOUT;
    }
}

/* One message: waits until the ME has taken the previous one. */
static uint8_t heci_send(struct heci *h, uint8_t me, uint8_t host, const uint32_t *d, uint32_t len)
{
    uint32_t n = (len + 3) / 4, csr;
    int f;

    while ((f = filled(csr = rd(h, H_CSR))) != 0) {
        if (f < 0)
            return BOOT_CSME_ST_BAD_REPLY;
        if (!tick(h))
            return BOOT_CSME_ST_TIMEOUT;
    }
    if (!up(csr) || !up(rd(h, ME_CSR_HA)))
        return BOOT_CSME_ST_NOT_READY;
    if (n + 1 > CSR_DEPTH(csr))
        return BOOT_CSME_ST_NOT_READY;
    wr(h, H_CB_WW, me | ((uint32_t)host << 8) | (len << 16) | HDR_COMPLETE);
    for (uint32_t i = 0; i < n; i++)
        wr(h, H_CB_WW, d[i]);
    host_signal(h);
    return BOOT_CSME_ST_OK;
}

/* One message from the ME: its header, and the first REPLY_MAX_DW dwords
 * of its body in d (zero-padded; the rest is read and dropped). */
static uint8_t heci_recv(struct heci *h, uint32_t *hdr, uint32_t d[REPLY_MAX_DW])
{
    uint32_t n;
    uint8_t st;

    memset(d, 0, REPLY_MAX_DW * 4);
    if ((st = wait_filled(h, 1)) != BOOT_CSME_ST_OK)
        return st;
    *hdr = rd(h, ME_CB_RW);
    n = (HDR_LEN(*hdr) + 3) / 4;
    if (n + 1 > CSR_DEPTH(rd(h, ME_CSR_HA))) /* header + body could never arrive in one piece */
        return BOOT_CSME_ST_BAD_REPLY;
    if ((st = wait_filled(h, n)) != BOOT_CSME_ST_OK)
        return st;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = rd(h, ME_CB_RW);
        if (i < REPLY_MAX_DW)
            d[i] = v;
    }
    host_signal(h);
    return (*hdr & HDR_COMPLETE) ? BOOT_CSME_ST_OK : BOOT_CSME_ST_BAD_REPLY;
}

/* The next message from `me` to `host` (for HBM, also with command `cmd`),
 * skipping up to MAX_SKIP others. A flow-control grant skipped on the way
 * is remembered in h->credit. */
static uint8_t recv_for(struct heci *h, uint8_t me, uint8_t host, int cmd, uint32_t d[REPLY_MAX_DW],
                        uint32_t *len)
{
    for (unsigned i = 0; i <= MAX_SKIP; i++) {
        uint32_t hdr;
        uint8_t st = heci_recv(h, &hdr, d);
        bool hbm = HDR_ME_ADDR(hdr) == 0 && HDR_HOST_ADDR(hdr) == 0;

        if (st != BOOT_CSME_ST_OK)
            return st;
        if (HDR_ME_ADDR(hdr) == me && HDR_HOST_ADDR(hdr) == host &&
            (cmd < 0 || (d[0] & 0xFF) == (uint32_t)cmd)) {
            *len = HDR_LEN(hdr);
            return BOOT_CSME_ST_OK;
        }
        if (hbm && (d[0] & 0xFF) == HBM_FLOW)
            h->credit = (int)((d[0] >> 8) & 0xFF);
    }
    return BOOT_CSME_ST_BAD_REPLY;
}

/* Check an MKHI GET_FW_VERSION reply in d; fill fw_* if `out` has none yet. */
static uint8_t parse_fw_ver(const uint32_t *d, uint32_t len, struct boot_csme *out)
{
    if (len < REPLY_MIN || (d[0] & 0xFF) != MKHI_GROUP_GEN ||
        ((d[0] >> 8) & 0xFF) != (MKHI_GET_FW_VER | MKHI_RESPONSE))
        return BOOT_CSME_ST_BAD_REPLY;
    if (d[0] >> 24)
        return BOOT_CSME_ST_REFUSED;
    if (out->flags & BOOT_CSME_VERSION_VALID)
        return BOOT_CSME_ST_OK;
    /* Code version block: u16 minor, u8 major, u8 platform, u16 build,
     * u16 hotfix. Recovery and FITC version blocks follow; unused. */
    out->fw_minor = (uint16_t)d[1];
    out->fw_major = (uint8_t)(d[1] >> 16);
    out->fw_build = (uint16_t)d[2];
    out->fw_hotfix = (uint16_t)(d[2] >> 16);
    out->flags |= BOOT_CSME_VERSION_VALID;
    return BOOT_CSME_ST_OK;
}

/* ---- the version query at MKHI's fixed address ---------------------------- */

static uint8_t fixed_exchange(struct heci *h, struct boot_csme *out)
{
    uint32_t req = MKHI_GROUP_GEN | (MKHI_GET_FW_VER << 8), hdr, d[REPLY_MAX_DW];
    uint8_t st;

    if ((st = link_idle(h)) != BOOT_CSME_ST_OK || (st = heci_send(h, MKHI_ADDR, 0, &req, 4)) != BOOT_CSME_ST_OK ||
        (st = heci_recv(h, &hdr, d)) != BOOT_CSME_ST_OK)
        return st;
    if (HDR_ME_ADDR(hdr) != MKHI_ADDR || HDR_HOST_ADDR(hdr) != 0)
        return BOOT_CSME_ST_BAD_REPLY;
    return parse_fw_ver(d, HDR_LEN(hdr), out);
}

void csme_query_version(const struct csme_ops *ops, struct boot_csme *out)
{
    struct heci h;

    if (!(out->flags & BOOT_CSME_PRESENT))
        return;
    if ((out->version_status = heci_open(&h, ops, out)) != BOOT_CSME_ST_OK)
        return;
    out->version_status = fixed_exchange(&h, out);
    heci_close(&h);
}

/* ---- the HBM session ------------------------------------------------------ */

/* One HBM request (len bytes of req) and its response, at least min_len
 * bytes, into d. */
static uint8_t hbm(struct heci *h, const uint32_t *req, uint32_t len, uint32_t *d, uint32_t min_len)
{
    uint32_t got;
    uint8_t st;

    if ((st = heci_send(h, 0, 0, req, len)) != BOOT_CSME_ST_OK ||
        (st = recv_for(h, 0, 0, (int)((req[0] & 0xFF) | HBM_RES), d, &got)) != BOOT_CSME_ST_OK)
        return st;
    return got < min_len ? BOOT_CSME_ST_BAD_REPLY : BOOT_CSME_ST_OK;
}

/* {cmd, me_addr, host_addr, 0}: the shape of most HBM requests */
static uint32_t hbm_req(uint8_t cmd, uint8_t me, uint8_t host)
{
    return cmd | ((uint32_t)me << 8) | ((uint32_t)host << 16);
}

/* GET_FW_VERSION to the MKHI client, the way Linux's bus-fixup asks:
 * a fixed-address client is used directly, any other is connected to,
 * given a flow-control credit, and waited on for one. */
static uint8_t client_version(struct heci *h, const struct csme_client *c, struct boot_csme *out)
{
    uint32_t mkhi = MKHI_GROUP_GEN | (MKHI_GET_FW_VER << 8), d[REPLY_MAX_DW], req[2], len;
    bool connect = !c->fixed_address;
    uint8_t st, st2;

    if (connect) {
        req[0] = hbm_req(HBM_CONNECT, c->addr, HOST_CLIENT);
        if ((st = hbm(h, req, 4, d, 4)) != BOOT_CSME_ST_OK)
            return st;
        if (((d[0] >> 8) & 0xFFFF) != (c->addr | (HOST_CLIENT << 8)))
            return BOOT_CSME_ST_BAD_REPLY;
        if (d[0] >> 24)
            return BOOT_CSME_ST_REFUSED;
        req[0] = hbm_req(HBM_FLOW, c->addr, HOST_CLIENT);
        req[1] = 0;
        if ((st = heci_send(h, 0, 0, req, HBM_FLOW_LEN)) != BOOT_CSME_ST_OK)
            goto disconnect;
        while (h->credit != c->addr) {
            if ((st = recv_for(h, 0, 0, HBM_FLOW, d, &len)) != BOOT_CSME_ST_OK)
                goto disconnect;
            h->credit = (int)((d[0] >> 8) & 0xFF);
        }
    }
    if ((st = heci_send(h, c->addr, HOST_CLIENT, &mkhi, 4)) == BOOT_CSME_ST_OK &&
        (st = recv_for(h, c->addr, HOST_CLIENT, -1, d, &len)) == BOOT_CSME_ST_OK)
        st = parse_fw_ver(d, len, out);
disconnect:
    if (!connect)
        return st;
    req[0] = hbm_req(HBM_DISCONNECT, c->addr, HOST_CLIENT);
    st2 = hbm(h, req, 4, d, 4);
    return st != BOOT_CSME_ST_OK ? st : st2;
}

/* Not every CSME answers HOST_STOP (Arrow Lake's does not), and Linux does
 * not wait for it. Give the answer DRAIN_US to arrive, reading whatever
 * comes so the ME buffer is left empty; silence is not a failure. */
static void drain_stop(struct heci *h)
{
    uint32_t hdr, d[REPLY_MAX_DW];

    if (h->waited_us < TIMEOUT_US - DRAIN_US)
        h->waited_us = TIMEOUT_US - DRAIN_US;
    while (heci_recv(h, &hdr, d) == BOOT_CSME_ST_OK)
        if (HDR_ME_ADDR(hdr) == 0 && HDR_HOST_ADDR(hdr) == 0 && (d[0] & 0xFF) == (HBM_STOP | HBM_RES))
            break;
}

/* MKHI as Linux finds it: the "MKHIF fix" client, 55213584-9a29-4916-badf-0fb7ed682aeb */
static const uint8_t uuid_mkhi_fix[16] = {0x84, 0x35, 0x21, 0x55, 0x29, 0x9a, 0x16, 0x49,
                                          0xba, 0xdf, 0x0f, 0xb7, 0xed, 0x68, 0x2a, 0xeb};

static uint8_t session(struct heci *h, struct boot_csme *out, struct csme_clients *cl)
{
    uint32_t req = hbm_req(HBM_START, 0, 0) | (HBM_MINOR << 8) | (HBM_MAJOR << 16);
    uint32_t d[REPLY_MAX_DW], valid[8];
    const struct csme_client *mkhi = NULL;
    uint8_t st, st2;

    if ((st = link_idle(h)) != BOOT_CSME_ST_OK || (st = hbm(h, &req, 4, d, 4)) != BOOT_CSME_ST_OK)
        return st;
    /* {cmd, me_max_version minor, major, host_version_supported}. When
     * unsupported the ME speaks its own, lower version; 1.0 has all this
     * session needs. */
    cl->hbm_minor = (uint8_t)(d[0] >> 8);
    cl->hbm_major = (uint8_t)(d[0] >> 16);
    if (!(d[0] >> 24) && cl->hbm_major < 1) {
        st = BOOT_CSME_ST_REFUSED;
        goto stop;
    }

    req = hbm_req(HBM_ENUM, 0, 0);
    if ((st = hbm(h, &req, 4, d, HBM_ENUM_LEN)) != BOOT_CSME_ST_OK)
        goto stop;
    memcpy(valid, &d[1], sizeof(valid));
    for (uint32_t a = 1; a < 256; a++) {
        struct csme_client *c = &cl->c[cl->count];

        if (!((valid[a / 32] >> (a % 32)) & 1))
            continue;
        req = hbm_req(HBM_PROPS, (uint8_t)a, 0);
        if ((st = hbm(h, &req, 4, d, HBM_PROPS_LEN)) != BOOT_CSME_ST_OK)
            goto stop;
        if (((d[0] >> 8) & 0xFF) != a) {
            st = BOOT_CSME_ST_BAD_REPLY;
            goto stop;
        }
        if ((d[0] >> 16) & 0xFF) /* status: the client went away */
            continue;
        if (out->client_count < 255)
            out->client_count++;
        if (cl->count == CSME_MAX_CLIENTS)
            continue;
        /* {cmd, me_addr, status, reserved}, then uuid[16], protocol_version,
         * max_number_of_connections, fixed_address, flags, max_msg_length */
        memcpy(c->uuid, &d[1], 16);
        c->addr = (uint8_t)a;
        c->protocol_version = (uint8_t)d[5];
        c->max_connections = (uint8_t)(d[5] >> 8);
        c->fixed_address = (uint8_t)(d[5] >> 16);
        c->max_msg_length = d[6];
        if (!memcmp(c->uuid, uuid_mkhi_fix, 16))
            mkhi = c;
        cl->count++;
    }

    out->mkhi_status = mkhi ? client_version(h, mkhi, out) : BOOT_CSME_ST_NONE;
    if (out->mkhi_status == BOOT_CSME_ST_TIMEOUT || out->mkhi_status == BOOT_CSME_ST_NOT_READY)
        return BOOT_CSME_ST_OK; /* the link is gone or wedged; HOST_STOP would not get through */
stop:
    if (st == BOOT_CSME_ST_TIMEOUT || st == BOOT_CSME_ST_NOT_READY)
        return st;
    req = hbm_req(HBM_STOP, 0, 0); /* reason 0: driver stop request */
    if ((st2 = heci_send(h, 0, 0, &req, 4)) == BOOT_CSME_ST_OK)
        drain_stop(h);
    return st != BOOT_CSME_ST_OK ? st : st2;
}

void csme_hbm_session(const struct csme_ops *ops, struct boot_csme *out, struct csme_clients *cl)
{
    struct heci h;

    memset(cl, 0, sizeof(*cl));
    if (!(out->flags & BOOT_CSME_PRESENT))
        return;
    if ((out->hbm_status = heci_open(&h, ops, out)) != BOOT_CSME_ST_OK)
        return;
    out->hbm_status = session(&h, out, cl);
    heci_close(&h);
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

const char *csme_status_name(uint8_t s)
{
    static const char *const names[] = {
        [BOOT_CSME_ST_NONE] = "not asked",
        [BOOT_CSME_ST_OK] = "ok",
        [BOOT_CSME_ST_NOT_NORMAL] = "CSME not in normal state",
        [BOOT_CSME_ST_NO_BAR] = "no memory BAR assigned",
        [BOOT_CSME_ST_NOT_READY] = "HECI link not ready",
        [BOOT_CSME_ST_BUSY] = "HECI busy",
        [BOOT_CSME_ST_TIMEOUT] = "no reply within 1 s",
        [BOOT_CSME_ST_BAD_REPLY] = "malformed reply",
        [BOOT_CSME_ST_REFUSED] = "request refused",
    };
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : NULL;
}

/* ME client UUIDs from Linux bus-fixup.c and the mei client drivers,
 * fields as in uuid_le: three little-endian, then eight bytes. */
static const struct { uint32_t a; uint16_t b, c; uint8_t d[8]; const char *name; } clients[] = {
    {0x55213584, 0x9a29, 0x4916, {0xba, 0xdf, 0x0f, 0xb7, 0xed, 0x68, 0x2a, 0xeb}, "MKHI (fixed)"},
    {0x8e6a6715, 0x9abc, 0x4043, {0x88, 0xef, 0x9e, 0x39, 0xc6, 0xf6, 0x3e, 0x0f}, "MKHI"},
    {0x12f80028, 0xb4b7, 0x4b2d, {0xac, 0xa8, 0x46, 0xe0, 0xff, 0x65, 0x81, 0x4c}, "AMTHI"},
    {0x05b79a6f, 0x4628, 0x4d7f, {0x89, 0x9d, 0xa9, 0x15, 0x14, 0xcb, 0x32, 0xab}, "watchdog"},
    {0xb638ab7e, 0x94e2, 0x4ea2, {0xa5, 0x52, 0xd1, 0xc5, 0x4b, 0x62, 0x7f, 0x04}, "HDCP"},
    {0xfbf6fcf1, 0x96cf, 0x4e2e, {0xa6, 0xa6, 0x1b, 0xab, 0x8c, 0xbe, 0x36, 0xb1}, "PAVP"},
    {0x0bb17a78, 0x2a8e, 0x4c50, {0x94, 0xd4, 0x50, 0x26, 0x67, 0x23, 0x77, 0x5c}, "NFC HCI"},
    {0xd2de1625, 0x382d, 0x417d, {0x48, 0xa4, 0xef, 0xab, 0xba, 0x8a, 0x12, 0x06}, "NFC info"},
    {0xbb579a2e, 0xcc54, 0x4450, {0xb1, 0xd0, 0x5e, 0x75, 0x20, 0xdc, 0xad, 0x25}, "DAL"},
};

const char *csme_client_name(const uint8_t u[16])
{
    for (unsigned i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
        uint32_t a = u[0] | (u[1] << 8) | (u[2] << 16) | ((uint32_t)u[3] << 24);

        if (a == clients[i].a && (u[4] | (u[5] << 8)) == clients[i].b &&
            (u[6] | (u[7] << 8)) == clients[i].c && !memcmp(u + 8, clients[i].d, 8))
            return clients[i].name;
    }
    return NULL;
}
