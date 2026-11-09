/*
 * Application processor startup (Intel SDM vol. 3A 9.4 "Multiple-processor
 * initialization", 10.6 "Issuing interprocessor interrupts"; ACPI 6.5
 * 5.2.12 "Multiple APIC Description Table").
 *
 * Before ExitBootServices: smp_parse_madt lists the CPUs and
 * smp_tramp_install copies the trampoline (smp_tramp.S) to a page below
 * 1 MiB with everything it needs. After: smp_start_aps sends INIT and
 * SIPIs; each AP enters long mode on the kernel's page tables, finds its
 * own entry by APIC ID, and parks there (bootinfo.h, "Application
 * processors").
 */
#ifndef ARCH_X86_64_SMP_H
#define ARCH_X86_64_SMP_H

#include <stdbool.h>
#include <stdint.h>
#include "bootinfo/bootinfo.h"

#define SMP_MAX_CPUS      1024
#define SMP_AP_STACK_SIZE 0x4000 /* 16 KiB */

struct smp_madt_cpu {
    uint32_t apic_id;
    uint32_t acpi_uid;
};

/* Enabled CPUs of the MADT at `madt` (`len` bytes, checksum already
 * verified), the BSP first; IDs listed twice (xAPIC and x2APIC entries)
 * count once. At most `cap` are stored; returns how many there are. */
uint32_t smp_parse_madt(const uint8_t *madt, uint32_t len, uint32_t bsp_apic_id, struct smp_madt_cpu *out,
                        uint32_t cap);

/* What the trampoline loads on every AP. */
struct smp_tramp_params {
    uint64_t cr3;        /* kernel page-table root; must be below 4 GiB */
    uint64_t cr0, cr4;
    uint64_t efer_set;   /* EFER bits to set (LME, NXE) */
    uint64_t pat;
    uint64_t xcr0;       /* 0: leave XCR0 alone (no OSXSAVE) */
    uint64_t gdtr_base;  /* kernel GDT, virtual */
    uint16_t gdtr_limit;
    uint64_t cpus;       /* virtual address of boot_smp_cpu[ncpus] */
    uint64_t ncpus;
    uint16_t stop;       /* 0, or halt the APs early (smp_tramp.S, D_STOP) */
};

/* Copy the trampoline to `page` (4 KiB aligned, below 1 MiB, identity
 * mapped now and in the kernel's tables) and fill in its data. */
void smp_tramp_install(uint64_t page, const struct smp_tramp_params *p);

/* How many APs reached the trampoline's 16-, 32- and 64-bit code. */
void smp_tramp_seen(uint64_t page, uint32_t seen[3]);

/* After ExitBootServices, with interrupts off: wake every other CPU (the
 * IPIs are broadcast) and wait until each BOOT_SMP_OFF entry of `cpus`
 * has parked or the time is up. `cpus` is the loader's (identity) view of the array the
 * trampoline was given. `init_delay_us` is the wait between INIT and the
 * first SIPI: the SDM's 10 ms, or as little as Linux uses on CPUs that
 * need none. Returns the number parked; *x2apic says how the IPIs went
 * and *self is this CPU's APIC ID as its local APIC reports it.
 * `progress`, if set, is called at each SMP_STAGE_*, and with
 * SMP_STAGE_TICK + ms each millisecond of a wait: a heartbeat. */
enum { SMP_STAGE_LAPIC = 1, SMP_STAGE_INIT, SMP_STAGE_INIT_WAIT, SMP_STAGE_SIPI1, SMP_STAGE_SIPI2 };
#define SMP_STAGE_TICK 100
uint32_t smp_start_aps(uint64_t page, struct boot_smp_cpu *cpus, uint32_t n, uint64_t tsc_hz,
                       uint32_t init_delay_us, bool *x2apic, uint32_t *self, void (*progress)(int stage));

#endif
