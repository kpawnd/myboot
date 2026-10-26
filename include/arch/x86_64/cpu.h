/*
 * x86-64 CPU primitives. Header-only and inline: each is one or two
 * instructions, so a call would cost more than the work.
 */
#ifndef ARCH_X86_64_CPU_H
#define ARCH_X86_64_CPU_H

#include <stdint.h>

#define CR0_WP    (1ull << 16)
#define CR4_LA57  (1ull << 12)
#define MSR_EFER  0xC0000080u
#define EFER_NXE  (1ull << 11)

/*
 * Read the time-stamp counter. LFENCE first so earlier instructions have
 * completed before the counter is sampled (Intel SDM vol. 2B, RDTSC);
 * without it the timestamp can be taken before the code being measured
 * retires. LFENCE is architectural on every x86-64 CPU (part of SSE2).
 *
 * Units are TSC ticks, not time: see boot_cpu.tsc_hz.
 */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* CPUID with an explicit subleaf (ECX). r[] = { EAX, EBX, ECX, EDX }. */
static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t r[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                     : "a"(leaf), "c"(subleaf));
}

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

static inline void cpu_cli(void) { __asm__ volatile("cli" : : : "memory"); }

/* Stop forever without spinning: HLT sleeps until the next interrupt, then
 * we halt again. Interrupts stay as the firmware left them. */
__attribute__((noreturn)) static inline void cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile("hlt");
}

#endif
