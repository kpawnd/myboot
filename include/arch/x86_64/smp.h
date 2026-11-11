/* Application processor startup (Intel SDM vol. */
#ifndef ARCH_X86_64_SMP_H
#define ARCH_X86_64_SMP_H

#include <stdbool.h>
#include <stdint.h>
#include "bootinfo/bootinfo.h"

#define SMP_MAX_CPUS      1024
#define SMP_AP_STACK_SIZE 0x4000

struct smp_madt_cpu {
    uint32_t apic_id;
    uint32_t acpi_uid;
};

uint32_t smp_parse_madt(const uint8_t *madt, uint32_t len, uint32_t bsp_apic_id, struct smp_madt_cpu *out,
                        uint32_t cap);

struct smp_tramp_params {
    uint64_t cr3;
    uint64_t cr0, cr4;
    uint64_t efer_set;
    uint64_t pat;
    uint64_t xcr0;
    uint64_t gdtr_base;
    uint16_t gdtr_limit;
    uint64_t cpus;
    uint64_t ncpus;
    uint16_t stop;
};

void smp_tramp_install(uint64_t page, const struct smp_tramp_params *p);

void smp_tramp_seen(uint64_t page, uint32_t seen[3]);

enum { SMP_STAGE_LAPIC = 1, SMP_STAGE_INIT, SMP_STAGE_INIT_WAIT, SMP_STAGE_SIPI1, SMP_STAGE_SIPI2 };
#define SMP_STAGE_TICK 100
uint32_t smp_start_aps(uint64_t page, struct boot_smp_cpu *cpus, uint32_t n, uint64_t tsc_hz,
                       uint32_t init_delay_us, bool *x2apic, uint32_t *self, void (*progress)(int stage));

#endif
