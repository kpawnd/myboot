/* CPU discovery (CPUID only: no MSR writes, no AP startup). */
#ifndef CPU_CPU_H
#define CPU_CPU_H

#include <stdint.h>
#include "bootinfo/bootinfo.h"

const struct boot_cpu *discover_cpu(void);

void cpu_set_tsc_hz(uint64_t hz, uint32_t source);

#endif
