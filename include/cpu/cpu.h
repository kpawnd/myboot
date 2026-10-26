/*
 * CPU discovery (CPUID only: no MSR writes, no AP startup).
 * Firmware-independent; runs once and caches its result.
 */
#ifndef CPU_CPU_H
#define CPU_CPU_H

#include <stdint.h>
#include "bootinfo/bootinfo.h"

const struct boot_cpu *discover_cpu(void);

/* Record a TSC frequency measured by the firmware backend when CPUID did
 * not provide one. Ignored if a CPUID-derived value already exists. */
void cpu_set_tsc_hz(uint64_t hz, uint32_t source);

#endif
