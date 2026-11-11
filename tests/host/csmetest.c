#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "csme/csme.h"

#define BAR       0xFE600000ull
#define DEPTH     128
#define H_CB_WW   0x00
#define H_CSR     0x04
#define ME_CB_RW  0x08
#define ME_CSR_HA 0x0C
#define CSR_IS    (1u << 1)
#define CSR_IG    (1u << 2)
#define CSR_RDY   (1u << 3)

enum reply { REPLY_OK, REPLY_NONE, REPLY_REFUSED, REPLY_WRONG_CLIENT };

#define MKHI_AT 7

static struct sim {
    uint8_t  cfg[256];
    uint32_t host_csr, me_csr;
    uint32_t hbuf[DEPTH], hn;
    uint32_t mbuf[DEPTH], mr, mw;
    enum reply reply;
    bool hbm_silent;
    bool mkhi_connect;
    bool early_flow;
    bool stop_silent;
    bool started, connected, host_credit, stopped;
    unsigned mkhi_hbm_answered;
    uint16_t cmd_initial;
    unsigned mmio_while_off, cfg_bad_writes, mmio_writes, is_cleared;
    uint64_t waited_us;
} S;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static bool mem_on(void) { return S.cfg[4] & 2; }

static uint32_t cfg_read32(uint16_t off) { return rd32(&S.cfg[off & 0xFC]); }

static void cfg_write16(uint16_t off, uint16_t v)
{
    if (off != 4)
        S.cfg_bad_writes++;
    memcpy(&S.cfg[off], &v, 2);
}

static uint32_t csr(uint32_t flags, uint32_t rp, uint32_t wp)
{
    return flags | ((rp & 0xFF) << 8) | ((wp & 0xFF) << 16) | ((uint32_t)DEPTH << 24);
}

static void me_send(uint32_t v) { S.mbuf[S.mw++ % DEPTH] = v; }

static void me_msg(uint8_t me, uint8_t host, const uint32_t *d, uint32_t len)
{
    me_send(me | ((uint32_t)host << 8) | (len << 16) | (1u << 31));
    for (uint32_t i = 0; i < (len + 3) / 4; i++)
        me_send(d[i]);
    S.me_csr |= CSR_IS;
    S.host_csr |= CSR_IS;
}

static void me_version(uint8_t me, uint8_t host)
{
    uint32_t d[7] = {0x82FFu | (S.reply == REPLY_REFUSED ? 0x89u << 24 : 0),
                     1 | (16u << 16),
                     2307 | (30u << 16),
                     0xEEEEEEEE, 0xEEEEEEEE, 0xEEEEEEEE, 0xEEEEEEEE};
    me_msg(me, host, d, 28);
}

static const struct { uint8_t addr; uint32_t uuid[4]; } clients[] = {
    {2, {0x12f80028, 0x4b2db4b7, 0xe046a8ac, 0x4c8165ff}},
    {MKHI_AT, {0x55213584, 0x49169a29, 0xb70fdfba, 0xeb2a68ed}},
    {40, {0x01020304, 0x05060708, 0x090a0b0c, 0x0d0e0f10}},
};

static void me_hbm(const uint32_t *d)
{
    uint8_t cmd = (uint8_t)d[0], me = (uint8_t)(d[0] >> 8);
    uint32_t r[9] = {0};

    r[0] = cmd | 0x80u;
    switch (cmd) {
    case 0x01:
        S.started = true;
        r[0] |= (2u << 8) | (2u << 16) | (1u << 24);
        me_msg(0, 0, r, 4);
        break;
    case 0x02:
        S.stopped = true;
        if (!S.stop_silent)
            me_msg(0, 0, r, 4);
        break;
    case 0x04:
        for (unsigned i = 0; i < 3; i++)
            r[1 + clients[i].addr / 32] |= 1u << (clients[i].addr % 32);
        me_msg(0, 0, r, 36);
        break;
    case 0x05:
        r[0] |= (uint32_t)me << 8;
        for (unsigned i = 0; i < 3; i++)
            if (clients[i].addr == me) {
                memcpy(&r[1], clients[i].uuid, 16);
                r[5] = 1 | (1u << 8) | ((me == MKHI_AT && !S.mkhi_connect ? MKHI_AT : 0u) << 16);
                r[6] = 2048;
            }
        me_msg(0, 0, r, 28);
        break;
    case 0x06: {
        uint32_t flow[2] = {0x08u | ((uint32_t)me << 8), 0};
        r[0] = d[0] | 0x80u;
        if (S.early_flow)
            me_msg(0, 0, flow, 8);
        me_msg(0, 0, r, 4);
        if (!S.early_flow)
            me_msg(0, 0, flow, 8);
        S.connected = true;
        break;
    }
    case 0x07:
        S.connected = false;
        r[0] = d[0] | 0x80u;
        me_msg(0, 0, r, 4);
        break;
    case 0x08:
        S.host_credit = true;
        break;
    }
}

static void me_process(void)
{
    uint32_t hdr = S.hbuf[0], me = hdr & 0xFF, host = (hdr >> 8) & 0xFF, len = (hdr >> 16) & 0x1FF;
    bool whole = S.hn == 1 + (len + 3) / 4 && (hdr >> 31);
    const uint32_t *d = &S.hbuf[1];

    S.hn = 0;
    if (!whole)
        return;
    if (me == 0 && host == 0) {
        if (!S.hbm_silent)
            me_hbm(d);
    } else if (me == MKHI_AT && host == 0) {
        if (len == 4 && d[0] == 0x02FFu && S.reply != REPLY_NONE)
            me_version(S.reply == REPLY_WRONG_CLIENT ? 0x08 : MKHI_AT, 0);
    } else if (me == MKHI_AT && S.started && !S.stopped) {
        if (len == 4 && d[0] == 0x02FFu && (!S.mkhi_connect || (S.connected && S.host_credit))) {
            S.mkhi_hbm_answered++;
            me_version(MKHI_AT, (uint8_t)host);
        }
    }
}

static uint32_t mmio_read32(uint64_t a)
{
    if (!mem_on()) {
        S.mmio_while_off++;
        return 0xFFFFFFFF;
    }
    switch (a - BAR) {
    case H_CSR:     return csr(S.host_csr, 0, S.hn);
    case ME_CSR_HA: return csr(S.me_csr, S.mr, S.mw);
    case ME_CB_RW:  return S.mr < S.mw ? S.mbuf[S.mr++ % DEPTH] : 0xFFFFFFFF;
    default:        return 0;
    }
}

static void mmio_write32(uint64_t a, uint32_t v)
{
    if (!mem_on()) {
        S.mmio_while_off++;
        return;
    }
    S.mmio_writes++;
    if (a - BAR == H_CB_WW && S.hn < DEPTH) {
        S.hbuf[S.hn++] = v;
    } else if (a - BAR == H_CSR) {
        if (v & CSR_IS) {
            S.is_cleared++;
            S.host_csr &= ~CSR_IS;
        }
        if ((v & CSR_IG) && S.hn)
            me_process();
    }
}

static void delay_us(uint32_t us) { S.waited_us += us; }

static const struct csme_ops ops = {cfg_read32, cfg_write16, mmio_read32, mmio_write32, delay_us};

static void sim_reset(enum reply reply)
{
    memset(&S, 0, sizeof(S));
    wr32(&S.cfg[0x00], 0xA0E08086);
    wr32(&S.cfg[0x04], 0x00100000);
    wr32(&S.cfg[0x08], 0x07800030);
    wr32(&S.cfg[0x10], (uint32_t)BAR | 0x4);
    wr32(&S.cfg[0x14], 0);
    wr32(&S.cfg[0x40], 0x90000245);
    wr32(&S.cfg[0x48], 0x32280106);
    wr32(&S.cfg[0x60], 0x00000020);
    S.host_csr = S.me_csr = CSR_RDY;
    S.reply = reply;
}

static unsigned g_pass, g_fail;

static void check(bool ok, const char *name, const char *what)
{
    printf("%s %s: %s\n", ok ? "ok  " : "FAIL", name, what);
    if (ok)
        g_pass++;
    else
        g_fail++;
}

static void check_invariants(const char *name)
{
    check(S.mmio_while_off == 0, name, "no MMIO while memory decoding is off");
    check(S.cfg_bad_writes == 0, name, "no configuration write but Command");
    check((uint16_t)rd32(&S.cfg[4]) == S.cmd_initial, name, "Command register restored");
    check(S.is_cleared == 0, name, "interrupt status left alone");
}

static struct csme_clients cl;

static struct boot_csme run(void)
{
    struct boot_csme c;

    S.cmd_initial = (uint16_t)rd32(&S.cfg[4]);
    csme_probe(&ops, &c);
    csme_query_version(&ops, &c);
    csme_hbm_session(&ops, &c, &cl);
    return c;
}

static void check_session(const char *name)
{
    check(S.started && S.stopped && !S.connected, name, "HBM started, stopped, nothing left connected");
    check(cl.count == 3 && cl.hbm_major == 2 && cl.hbm_minor == 2 && cl.c[1].addr == MKHI_AT &&
              cl.c[1].max_msg_length == 2048, name, "clients enumerated");
    check(csme_client_name(cl.c[0].uuid) && !strcmp(csme_client_name(cl.c[0].uuid), "AMTHI") &&
              csme_client_name(cl.c[1].uuid) && !strcmp(csme_client_name(cl.c[1].uuid), "MKHI (fixed)") &&
              !csme_client_name(cl.c[2].uuid), name, "clients named by UUID");
}

int main(void)
{
    struct boot_csme c;
    static const uint8_t zero[sizeof(struct boot_csme)];

    sim_reset(REPLY_OK);
    memset(S.cfg, 0xFF, sizeof(S.cfg));
    c = run();
    check(!memcmp(&c, zero, sizeof(c)), "absent", "all fields zero");
    check(S.mmio_writes == 0, "absent", "no MMIO");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x08], 0x01060130);
    c = run();
    check(c.vendor_id == 0 && c.flags == 0, "other device", "not taken for HECI");

    sim_reset(REPLY_OK);
    c = run();
    check(c.vendor_id == 0x8086 && c.device_id == 0xA0E0 && c.revision_id == 0x30, "normal", "IDs");
    check(c.hfsts[0] == 0x90000245 && c.hfsts[1] == 0x32280106 && c.hfsts[2] == 0x20, "normal", "raw HFSTS");
    check(c.working_state == 5 && c.op_mode == 0 && c.error_code == 0, "normal", "HFSTS1 decoded");
    check(c.version_status == BOOT_CSME_ST_OK && (c.flags & BOOT_CSME_VERSION_VALID), "normal", "version read");
    check(c.fw_major == 16 && c.fw_minor == 1 && c.fw_hotfix == 30 && c.fw_build == 2307, "normal", "version 16.1.30.2307");
    check(S.mr == S.mw, "normal", "every reply consumed");
    check(c.hbm_status == BOOT_CSME_ST_OK && c.client_count == 3, "normal", "HBM session ok");
    check(c.mkhi_status == BOOT_CSME_ST_OK && S.mkhi_hbm_answered == 1, "normal", "MKHI answered over HBM");
    check_session("normal");
    check_invariants("normal");

    sim_reset(REPLY_OK);
    S.mkhi_connect = true;
    c = run();
    check(c.mkhi_status == BOOT_CSME_ST_OK && S.host_credit, "connect", "connected, credit given, answered");
    check_session("connect");
    check_invariants("connect");

    sim_reset(REPLY_OK);
    S.mkhi_connect = S.early_flow = true;
    c = run();
    check(c.mkhi_status == BOOT_CSME_ST_OK && S.mr == S.mw, "early grant", "grant before response kept");
    check_session("early grant");
    check_invariants("early grant");

    sim_reset(REPLY_OK);
    S.stop_silent = true;
    c = run();
    check(c.hbm_status == BOOT_CSME_ST_OK && c.mkhi_status == BOOT_CSME_ST_OK, "silent stop", "session ok");
    check(S.waited_us >= 20000 && S.waited_us < 25000, "silent stop", "waited 20 ms for the answer, no more");
    check_session("silent stop");
    check_invariants("silent stop");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x04], 0x00100006);
    c = run();
    check(c.version_status == BOOT_CSME_ST_OK, "decoding on", "version read");
    check_invariants("decoding on");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x40], 0x90030245);
    c = run();
    check(c.op_mode == 3 && c.version_status == BOOT_CSME_ST_NOT_NORMAL &&
          c.hbm_status == BOOT_CSME_ST_NOT_NORMAL && c.client_count == 0, "disabled", "not asked");
    check(S.mmio_writes == 0, "disabled", "no MMIO");
    check_invariants("disabled");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x40], 0x90000045);
    c = run();
    check(!(c.flags & BOOT_CSME_INIT_COMPLETE) && c.version_status == BOOT_CSME_ST_NOT_NORMAL,
          "initializing", "not asked");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x10], 0x4);
    c = run();
    check(c.version_status == BOOT_CSME_ST_NO_BAR && c.hbm_status == BOOT_CSME_ST_NO_BAR && S.mmio_writes == 0,
          "no BAR", "not asked, no MMIO");
    check_invariants("no BAR");

    sim_reset(REPLY_OK);
    S.host_csr = 0;
    c = run();
    check(c.version_status == BOOT_CSME_ST_NOT_READY && c.hbm_status == BOOT_CSME_ST_NOT_READY &&
          S.mmio_writes == 0, "not ready", "no write, no reset");
    check_invariants("not ready");

    sim_reset(REPLY_OK);
    me_send(0x12345678);
    c = run();
    check(c.version_status == BOOT_CSME_ST_BUSY && c.hbm_status == BOOT_CSME_ST_BUSY && S.mmio_writes == 0,
          "busy", "no write");
    check_invariants("busy");

    sim_reset(REPLY_NONE);
    c = run();
    check(c.version_status == BOOT_CSME_ST_TIMEOUT, "fixed closed", "fixed address timed out");
    check(S.waited_us >= 1000000 && S.waited_us < 1100000, "fixed closed", "gave up after 1 s");
    check(c.mkhi_status == BOOT_CSME_ST_OK && (c.flags & BOOT_CSME_VERSION_VALID) && c.fw_major == 16,
          "fixed closed", "version read over HBM");
    check_session("fixed closed");
    check_invariants("fixed closed");

    sim_reset(REPLY_OK);
    S.hbm_silent = true;
    c = run();
    check(c.version_status == BOOT_CSME_ST_OK && c.hbm_status == BOOT_CSME_ST_TIMEOUT && c.client_count == 0 &&
          c.mkhi_status == BOOT_CSME_ST_NONE, "no HBM", "fixed address ok, session timed out");
    check(S.waited_us >= 1000000 && S.waited_us < 1100000, "no HBM", "gave up after 1 s");
    check_invariants("no HBM");

    sim_reset(REPLY_REFUSED);
    c = run();
    check(c.version_status == BOOT_CSME_ST_REFUSED && c.mkhi_status == BOOT_CSME_ST_REFUSED && c.fw_major == 0 &&
          !(c.flags & BOOT_CSME_VERSION_VALID), "refused", "reported on both paths, no version");
    check_session("refused");
    check_invariants("refused");

    sim_reset(REPLY_WRONG_CLIENT);
    c = run();
    check(c.version_status == BOOT_CSME_ST_BAD_REPLY, "wrong client", "rejected");
    check(c.mkhi_status == BOOT_CSME_ST_OK && c.fw_major == 16, "wrong client", "version read over HBM");
    check_invariants("wrong client");

    check(csme_platform_name(0xA0E0) && !strcmp(csme_platform_name(0xA0E0), "Tiger Lake-LP") &&
          !csme_platform_name(0x1234), "names", "platform lookup");
    check(csme_status_name(BOOT_CSME_ST_REFUSED + 1) == NULL && csme_mode_name(15) == NULL &&
          csme_state_name(200) == NULL, "names", "out-of-range values have no name");

    printf("%u passed, %u failed\n", g_pass, g_fail);
    return g_fail != 0;
}
