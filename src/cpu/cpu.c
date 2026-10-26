/*
 * CPU discovery from CPUID (Intel SDM vol. 2A "CPUID"; AMD APM vol. 3 E.3).
 *
 * Cost: about 15 CPUID executions. CPUID is serialising and can exit to the
 * hypervisor under virtualisation (~1-2 us each on real hardware VMs, more
 * under TCG), so every leaf is read once and the result cached.
 */
#include <stdbool.h>
#include "cpu/cpu.h"
#include "arch/x86_64/cpu.h"
#include "lib/mem.h"

static struct boot_cpu g_cpu;
static bool g_cpu_done;

static void set_if(uint64_t *f, uint32_t reg, unsigned bit, uint64_t flag)
{
    if (reg & (1u << bit))
        *f |= flag;
}

static void read_brand(struct boot_cpu *c)
{
    uint32_t r[4];
    char raw[48];
    unsigned i, skip = 0;

    for (i = 0; i < 3; i++) {
        cpuid(0x80000002u + i, 0, r);
        memcpy(raw + 16 * i, r, 16);
    }
    while (skip < 48 && raw[skip] == ' ')
        skip++;
    memcpy(c->brand, raw + skip, 48 - skip); /* tail stays zero-filled */
}

/* Leaf 15h gives TSC = crystal * EBX / EAX exactly; leaf 16h only the
 * nominal base frequency in MHz. Both are often absent (AMD, most VMs). */
static void read_tsc_freq(struct boot_cpu *c, uint32_t max_leaf)
{
    uint32_t r[4];

    if (max_leaf >= 0x15) {
        cpuid(0x15, 0, r);
        if (r[0] && r[1] && r[2]) {
            c->tsc_hz = (uint64_t)r[2] * r[1] / r[0];
            c->tsc_source = BOOT_TSC_CPUID_15H;
            return;
        }
    }
    if (max_leaf >= 0x16) {
        cpuid(0x16, 0, r);
        if (r[0] & 0xFFFF) {
            c->tsc_hz = (uint64_t)(r[0] & 0xFFFF) * 1000000u;
            c->tsc_source = BOOT_TSC_CPUID_16H;
        }
    }
}

/* Leaf 0Bh: level 0 is SMT (threads per core), level 1 is core (logical
 * processors per package). Only trusted when the level types match. */
static void read_topology(struct boot_cpu *c, uint32_t max_leaf)
{
    uint32_t r[4];

    if (max_leaf < 0x0B)
        return;
    cpuid(0x0B, 0, r);
    if (((r[2] >> 8) & 0xFF) != 1 || (r[1] & 0xFFFF) == 0)
        return;
    c->threads_per_core = (uint16_t)(r[1] & 0xFFFF);
    c->bsp_apic_id = r[3]; /* full 32-bit x2APIC ID */
    cpuid(0x0B, 1, r);
    if (((r[2] >> 8) & 0xFF) == 2)
        c->logical_per_package = (uint16_t)(r[1] & 0xFFFF);
}

const struct boot_cpu *discover_cpu(void)
{
    struct boot_cpu *c = &g_cpu;
    uint32_t r[4], max_leaf, max_ext;

    if (g_cpu_done)
        return c;
    g_cpu_done = true;

    cpuid(0, 0, r);
    max_leaf = r[0];
    memcpy(c->vendor + 0, &r[1], 4); /* EBX, EDX, ECX order */
    memcpy(c->vendor + 4, &r[3], 4);
    memcpy(c->vendor + 8, &r[2], 4);

    if (max_leaf >= 1) {
        uint32_t base_family, base_model;

        cpuid(1, 0, r);
        base_family = (r[0] >> 8) & 0xF;
        base_model = (r[0] >> 4) & 0xF;
        c->stepping = r[0] & 0xF;
        c->family = base_family == 0xF ? base_family + ((r[0] >> 20) & 0xFF) : base_family;
        c->model = (base_family == 6 || base_family == 0xF)
                       ? (((r[0] >> 16) & 0xF) << 4) | base_model
                       : base_model;
        c->bsp_apic_id = r[1] >> 24;
        if (r[3] & (1u << 28)) /* HTT: EBX[23:16] is a valid count */
            c->logical_per_package = (uint16_t)((r[1] >> 16) & 0xFF);

        set_if(&c->features, r[3], 0, BOOT_CPU_FPU);
        set_if(&c->features, r[3], 4, BOOT_CPU_TSC);
        set_if(&c->features, r[3], 9, BOOT_CPU_APIC);
        set_if(&c->features, r[3], 26, BOOT_CPU_SSE2);
        set_if(&c->features, r[2], 0, BOOT_CPU_SSE3);
        set_if(&c->features, r[2], 9, BOOT_CPU_SSSE3);
        set_if(&c->features, r[2], 19, BOOT_CPU_SSE41);
        set_if(&c->features, r[2], 20, BOOT_CPU_SSE42);
        set_if(&c->features, r[2], 21, BOOT_CPU_X2APIC);
        set_if(&c->features, r[2], 23, BOOT_CPU_POPCNT);
        set_if(&c->features, r[2], 25, BOOT_CPU_AES);
        set_if(&c->features, r[2], 26, BOOT_CPU_XSAVE);
        set_if(&c->features, r[2], 28, BOOT_CPU_AVX);
        set_if(&c->features, r[2], 30, BOOT_CPU_RDRAND);
        set_if(&c->features, r[2], 31, BOOT_CPU_HYPERVISOR);
    }

    if (max_leaf >= 7) {
        cpuid(7, 0, r);
        set_if(&c->features, r[1], 3, BOOT_CPU_BMI1);
        set_if(&c->features, r[1], 5, BOOT_CPU_AVX2);
        set_if(&c->features, r[1], 8, BOOT_CPU_BMI2);
        set_if(&c->features, r[1], 9, BOOT_CPU_ERMS);
        set_if(&c->features, r[1], 16, BOOT_CPU_AVX512F);
        set_if(&c->features, r[1], 29, BOOT_CPU_SHA);
        set_if(&c->features, r[2], 16, BOOT_CPU_LA57);
    }

    read_topology(c, max_leaf);
    read_tsc_freq(c, max_leaf);

    cpuid(0x80000000u, 0, r);
    max_ext = r[0];
    if (max_ext >= 0x80000001u) {
        cpuid(0x80000001u, 0, r);
        set_if(&c->features, r[3], 20, BOOT_CPU_NX);
        set_if(&c->features, r[3], 26, BOOT_CPU_PDPE1GB);
        set_if(&c->features, r[3], 27, BOOT_CPU_RDTSCP);
    }
    if (max_ext >= 0x80000004u)
        read_brand(c);
    if (max_ext >= 0x80000007u) {
        cpuid(0x80000007u, 0, r);
        set_if(&c->features, r[3], 8, BOOT_CPU_INVARIANT_TSC);
    }
    if (max_ext >= 0x80000008u) {
        cpuid(0x80000008u, 0, r);
        c->phys_addr_bits = (uint8_t)(r[0] & 0xFF);
        c->virt_addr_bits = (uint8_t)((r[0] >> 8) & 0xFF);
    } else {
        c->phys_addr_bits = 36; /* architectural defaults without the leaf */
        c->virt_addr_bits = 48;
    }

    /* Hypervisor leaves exist only when CPUID.1:ECX[31] says so; reading
     * them on bare metal returns data from the highest basic leaf. */
    if (c->features & BOOT_CPU_HYPERVISOR) {
        cpuid(0x40000000u, 0, r);
        memcpy(c->hypervisor + 0, &r[1], 4);
        memcpy(c->hypervisor + 4, &r[2], 4);
        memcpy(c->hypervisor + 8, &r[3], 4);
        if (!c->tsc_hz && r[0] >= 0x40000010u) {
            uint32_t h[4];
            cpuid(0x40000010u, 0, h); /* EAX = TSC kHz (VMware/KVM convention) */
            if (h[0]) {
                c->tsc_hz = (uint64_t)h[0] * 1000u;
                c->tsc_source = BOOT_TSC_HYPERVISOR;
            }
        }
    }
    return c;
}

void cpu_set_tsc_hz(uint64_t hz, uint32_t source)
{
    if (!g_cpu.tsc_hz) {
        g_cpu.tsc_hz = hz;
        g_cpu.tsc_source = source;
    }
}
