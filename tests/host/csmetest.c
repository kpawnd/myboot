/*
 * Host test for src/csme/csme.c: runs csme_probe and csme_query_version
 * against a simulated HECI1 device. The simulation holds 256 bytes of
 * configuration space and the four HECI registers, and an ME that answers
 * GET_FW_VERSION the way a test case asks (correctly, never, with an error
 * result, with a reply for someone else).
 *
 * Besides each case's result, every case checks what the code must never
 * do: touch MMIO while memory decoding is off, write any configuration
 * register but Command, or leave Command changed.
 */
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

static struct sim {
    uint8_t  cfg[256];
    uint32_t host_csr, me_csr;     /* RDY/IS bits; pointers are computed */
    uint32_t hbuf[DEPTH], hn;      /* host -> ME, not yet consumed */
    uint32_t mbuf[DEPTH], mr, mw;  /* ME -> host */
    enum reply reply;
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

/* The ME reads the host buffer when the host sets IG. */
static void me_process(void)
{
    uint32_t hdr = S.hbuf[0], mkhi = S.hbuf[1];
    uint8_t me = S.reply == REPLY_WRONG_CLIENT ? 0x08 : 0x07;

    if (S.hn != 2 || hdr != (0x07u | (4u << 16) | (1u << 31)) || mkhi != 0x02FFu || S.reply == REPLY_NONE) {
        S.hn = 0;
        return;
    }
    S.hn = 0;
    me_send(me | (28u << 16) | (1u << 31));
    me_send(0x82FFu | (S.reply == REPLY_REFUSED ? 0x89u << 24 : 0));
    me_send(1 | (16u << 16));    /* minor 1, major 16, platform 0 */
    me_send(2307 | (30u << 16)); /* build 2307, hotfix 30 */
    for (int i = 0; i < 4; i++)  /* recovery and FITC blocks */
        me_send(0xEEEEEEEE);
    S.me_csr |= CSR_IS;
    S.host_csr |= CSR_IS;
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

/* A Tiger Lake-LP HECI in normal state, memory decoding off, link ready. */
static void sim_reset(enum reply reply)
{
    memset(&S, 0, sizeof(S));
    wr32(&S.cfg[0x00], 0xA0E08086);
    wr32(&S.cfg[0x04], 0x00100000);            /* Command 0, Status: capabilities list */
    wr32(&S.cfg[0x08], 0x07800030);            /* class 0780, revision 0x30 */
    wr32(&S.cfg[0x10], (uint32_t)BAR | 0x4);   /* 64-bit memory BAR */
    wr32(&S.cfg[0x14], 0);
    wr32(&S.cfg[0x40], 0x90000245);            /* normal, init complete, mode 0 */
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

/* What every case must hold, whatever its result. */
static void check_invariants(const char *name)
{
    check(S.mmio_while_off == 0, name, "no MMIO while memory decoding is off");
    check(S.cfg_bad_writes == 0, name, "no configuration write but Command");
    check((uint16_t)rd32(&S.cfg[4]) == S.cmd_initial, name, "Command register restored");
    check(S.is_cleared == 0, name, "interrupt status left alone");
}

static struct boot_csme run(void)
{
    struct boot_csme c;

    S.cmd_initial = (uint16_t)rd32(&S.cfg[4]);
    csme_probe(&ops, &c);
    csme_query_version(&ops, &c);
    return c;
}

int main(void)
{
    struct boot_csme c;
    static const uint8_t zero[sizeof(struct boot_csme)];

    sim_reset(REPLY_OK);
    memset(S.cfg, 0xFF, sizeof(S.cfg)); /* nothing at 00:16.0 */
    c = run();
    check(!memcmp(&c, zero, sizeof(c)), "absent", "all fields zero");
    check(S.mmio_writes == 0, "absent", "no MMIO");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x08], 0x01060130); /* an Intel SATA controller, not HECI */
    c = run();
    check(c.vendor_id == 0 && c.flags == 0, "other device", "not taken for HECI");

    sim_reset(REPLY_OK);
    c = run();
    check(c.vendor_id == 0x8086 && c.device_id == 0xA0E0 && c.revision_id == 0x30, "normal", "IDs");
    check(c.hfsts[0] == 0x90000245 && c.hfsts[1] == 0x32280106 && c.hfsts[2] == 0x20, "normal", "raw HFSTS");
    check(c.working_state == 5 && c.op_mode == 0 && c.error_code == 0, "normal", "HFSTS1 decoded");
    check(c.version_status == BOOT_CSME_VER_OK && (c.flags & BOOT_CSME_VERSION_VALID), "normal", "version read");
    check(c.fw_major == 16 && c.fw_minor == 1 && c.fw_hotfix == 30 && c.fw_build == 2307, "normal", "version 16.1.30.2307");
    check(S.mr == S.mw, "normal", "whole reply consumed");
    check_invariants("normal");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x04], 0x00100006); /* decoding already on, bus master on */
    c = run();
    check(c.version_status == BOOT_CSME_VER_OK, "decoding on", "version read");
    check_invariants("decoding on");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x40], 0x90030245); /* soft temporary disable */
    c = run();
    check(c.op_mode == 3 && c.version_status == BOOT_CSME_VER_NOT_NORMAL, "disabled", "not asked");
    check(S.mmio_writes == 0, "disabled", "no MMIO");
    check_invariants("disabled");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x40], 0x90000045); /* init not complete */
    c = run();
    check(!(c.flags & BOOT_CSME_INIT_COMPLETE) && c.version_status == BOOT_CSME_VER_NOT_NORMAL,
          "initializing", "not asked");

    sim_reset(REPLY_OK);
    wr32(&S.cfg[0x10], 0x4); /* BAR never assigned */
    c = run();
    check(c.version_status == BOOT_CSME_VER_NO_BAR && S.mmio_writes == 0, "no BAR", "not asked, no MMIO");
    check_invariants("no BAR");

    sim_reset(REPLY_OK);
    S.host_csr = 0; /* firmware never brought the host side up */
    c = run();
    check(c.version_status == BOOT_CSME_VER_NOT_READY && S.mmio_writes == 0, "not ready", "no write, no reset");
    check_invariants("not ready");

    sim_reset(REPLY_OK);
    me_send(0x12345678); /* a stale message from the ME */
    c = run();
    check(c.version_status == BOOT_CSME_VER_BUSY && S.mmio_writes == 0, "busy", "no write");
    check_invariants("busy");

    sim_reset(REPLY_NONE);
    c = run();
    check(c.version_status == BOOT_CSME_VER_TIMEOUT && !(c.flags & BOOT_CSME_VERSION_VALID), "timeout",
          "reported");
    check(S.waited_us >= 1000000 && S.waited_us < 1100000, "timeout", "gave up after 1 s");
    check_invariants("timeout");

    sim_reset(REPLY_REFUSED);
    c = run();
    check(c.version_status == BOOT_CSME_VER_REFUSED && c.fw_major == 0, "refused", "reported, no version");
    check_invariants("refused");

    sim_reset(REPLY_WRONG_CLIENT);
    c = run();
    check(c.version_status == BOOT_CSME_VER_BAD_REPLY && c.fw_major == 0, "wrong client", "rejected");
    check_invariants("wrong client");

    check(csme_platform_name(0xA0E0) && !strcmp(csme_platform_name(0xA0E0), "Tiger Lake-LP") &&
          !csme_platform_name(0x1234), "names", "platform lookup");
    check(csme_version_status_name(BOOT_CSME_VER_REFUSED + 1) == NULL && csme_mode_name(15) == NULL &&
          csme_state_name(200) == NULL, "names", "out-of-range values have no name");

    printf("%u passed, %u failed\n", g_pass, g_fail);
    return g_fail != 0;
}
