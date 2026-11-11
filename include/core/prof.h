/* Boot-phase timing points. */
#ifndef CORE_PROF_H
#define CORE_PROF_H

#include <stdint.h>
#include "core/config.h"
#include "arch/x86_64/cpu.h"

enum prof_point {
    PROF_ENTRY,
    PROF_CPU,
    PROF_TSC_CAL,
    PROF_MEMORY,
    PROF_ACPI,
    PROF_SMBIOS,
    PROF_GRAPHICS,
    PROF_STORAGE,
    PROF_CSME,
    PROF_CONFIG,
    PROF_KERNEL,
    PROF_PAGING,
    PROF_COUNT
};

#if BOOT_PROFILE
extern uint64_t g_prof[PROF_COUNT];
#define PROF_MARK(p)   (g_prof[(p)] = rdtsc())
#define PROF_SET(p, t) (g_prof[(p)] = (t))
#else
#define PROF_MARK(p)   ((void)0)
#define PROF_SET(p, t) ((void)0)
#endif

#endif
