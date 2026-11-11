/* x86-64 CPU primitives. */
#ifndef ARCH_X86_64_CPU_H
#define ARCH_X86_64_CPU_H

#include <stdint.h>

#define CR0_MP         (1ull << 1)
#define CR0_EM         (1ull << 2)
#define CR0_NE         (1ull << 5)
#define CR0_WP         (1ull << 16)
#define CR4_PAE        (1ull << 5)
#define CR4_OSFXSR     (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_LA57       (1ull << 12)
#define CR4_OSXSAVE    (1ull << 18)
#define MSR_APIC_BASE  0x1Bu
#define MSR_PAT        0x277u
#define MSR_EFER       0xC0000080u
#define EFER_LME       (1ull << 8)
#define EFER_NXE       (1ull << 11)

#define PAT_LINUX      0x0407050600070106ull
#define XCR0_X87       (1ull << 0)
#define XCR0_SSE       (1ull << 1)
#define XCR0_AVX       (1ull << 2)
#define XCR0_AVX512    (7ull << 5)

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t r[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                     : "a"(leaf), "c"(subleaf));
}

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void write_cr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }
static inline void wbinvd(void) { __asm__ volatile("wbinvd" : : : "memory"); }

static inline uint64_t xgetbv0(void)
{
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

static inline void xsetbv0(uint64_t v)
{
    __asm__ volatile("xsetbv" : : "c"(0), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

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

static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static inline void cpu_cli(void) { __asm__ volatile("cli" : : : "memory"); }

__attribute__((noreturn)) static inline void cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile("hlt");
}

#endif
