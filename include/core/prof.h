/*
 * Boot-phase timing points. With BOOT_PROFILE=0 every macro expands to
 * nothing: no array, no RDTSC, no code.
 *
 * Each point records the TSC when a phase *ends*; phase i lasted
 * g_prof[i] - g_prof[i - 1].
 */
#ifndef CORE_PROF_H
#define CORE_PROF_H

#include <stdint.h>
#include "core/config.h"
#include "arch/x86_64/cpu.h"

enum prof_point {
    PROF_ENTRY,     /* loader entry */
    PROF_CPU,       /* CPUID discovery */
    PROF_TSC_CAL,   /* TSC calibration (only when CPUID gives no frequency) */
    PROF_MEMORY,    /* first memory map read */
    PROF_ACPI,      /* configuration table scan + RSDP/XSDT validation */
    PROF_SMBIOS,
    PROF_GRAPHICS,  /* GOP query */
    PROF_STORAGE,   /* boot device path + BlockIo */
    PROF_KERNEL,    /* open, read, validate and load the kernel ELF */
    PROF_PAGING,    /* arena, page tables, boot_info */
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
