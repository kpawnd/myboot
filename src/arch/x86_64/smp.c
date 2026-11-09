/*
 * Application processor startup. See include/arch/x86_64/smp.h.
 *
 * The sequence is the SDM's own (vol. 3A 9.4.4.1, "Typical BSP
 * initialization sequence"): INIT to all processors excluding self, a wait
 * (10 ms, or 10 us where Linux skips it), start-up IPI (SIPI) to all excluding self, 200 us, SIPI again. Using the
 * "all excluding self" shorthand means no IPI ever names a destination, so
 * none can reach the CPU running this whatever IDs the MADT holds; an INIT
 * to itself would stop the boot for good. There is no INIT de-assert: the
 * SDM drops it for every CPU since the Pentium 4, and where it still acts
 * it goes to every processor, this one included.
 *
 * A CPU missing from the list (disabled in the MADT) wakes too, finds no
 * entry and halts. All APs run the trampoline at once, which works because
 * it never writes to its own page: each writes only its own entry.
 */
#include "arch/x86_64/smp.h"
#include "arch/x86_64/cpu.h"
#include "lib/mem.h"

#define MADT_ENTRIES    44 /* SDT header + local APIC address + flags */
#define MADT_LAPIC      0
#define MADT_X2APIC     9
#define MADT_ENABLED    (1u << 0)

#define APIC_BASE_EXTD  (1ull << 10) /* x2APIC mode */
#define APIC_BASE_EN    (1ull << 11)
#define APIC_BASE_ADDR  0x000FFFFFFFFFF000ull
#define XAPIC_ID        0x020
#define XAPIC_ICR_LO    0x300
#define XAPIC_ICR_HI    0x310
#define X2APIC_ID       0x802u
#define X2APIC_ICR      0x830u
#define ICR_PENDING     (1u << 12)
#define ICR_ALL_BUT_SELF  (3u << 18) /* destination shorthand */
#define ICR_INIT          0x00004500u /* INIT, level assert */
#define ICR_SIPI          0x00004600u /* start-up; vector = page >> 12 */

#define SIPI_DELAY_US   200
#define PARK_WAIT_US    100000

/* smp_tramp.S */
extern const uint8_t smp_tramp_start[], smp_tramp_end[], smp_tramp_lm64[], smp_tramp_pm32[];

/* The trampoline's data block, at offset 8 of its page; smp_tramp.S
 * addresses it with these same offsets (D_*). */
struct __attribute__((packed)) tramp_data {
    uint16_t gdt32_limit;  /* 8 */
    uint32_t gdt32_base;   /* 10 */
    uint16_t stop;         /* 14: smp_tramp_params.stop */
    uint32_t far32_off;    /* 16: protected-mode entry, linear */
    uint16_t far32_sel;
    uint16_t pad1;
    uint32_t far64_off;    /* 24: long-mode entry, linear */
    uint16_t far64_sel;
    uint16_t pad2;
    uint64_t cr3;          /* 32 */
    uint64_t cr4;          /* 40 */
    uint64_t cr0;          /* 48 */
    uint64_t efer_set;     /* 56 */
    uint64_t pat;          /* 64 */
    uint64_t xcr0;         /* 72 */
    uint16_t gdt64_limit;  /* 80 */
    uint64_t gdt64_base;   /* 82 */
    uint16_t seen[3];      /* 90: APs that reached 16-, 32-, 64-bit code */
    uint64_t cpus;         /* 96 */
    uint64_t ncpus;        /* 104 */
};
#define TRAMP_DATA  8
#define TRAMP_GDT32 112 /* four descriptors, after the data */
_Static_assert(offsetof(struct tramp_data, cr3) == 32 - TRAMP_DATA, "tramp_data.cr3");
_Static_assert(offsetof(struct tramp_data, gdt64_limit) == 80 - TRAMP_DATA, "tramp_data.gdt64");
_Static_assert(offsetof(struct tramp_data, ncpus) == 104 - TRAMP_DATA, "tramp_data.ncpus");
_Static_assert(offsetof(struct tramp_data, stop) == 14 - TRAMP_DATA, "tramp_data.stop");
_Static_assert(offsetof(struct tramp_data, seen) == 90 - TRAMP_DATA, "tramp_data.seen");
_Static_assert(TRAMP_DATA + sizeof(struct tramp_data) == TRAMP_GDT32, "tramp_data size");

static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool listed(const struct smp_madt_cpu *out, uint32_t n, uint32_t cap, uint32_t id)
{
    for (uint32_t i = 0; i < n && i < cap; i++)
        if (out[i].apic_id == id)
            return true;
    return false;
}

uint32_t smp_parse_madt(const uint8_t *madt, uint32_t len, uint32_t bsp_apic_id, struct smp_madt_cpu *out,
                        uint32_t cap)
{
    uint32_t n = 0;
    bool bsp_seen = false;

    /* Slot 0 is the BSP's, filled in when its entry turns up. */
    if (cap)
        out[0] = (struct smp_madt_cpu){bsp_apic_id, 0};
    n = 1;
    for (uint32_t off = MADT_ENTRIES; off + 2 <= len;) {
        const uint8_t *e = madt + off;
        uint32_t id, uid, flags;

        if (e[1] < 2 || off + e[1] > len)
            break;
        off += e[1];
        if (e[0] == MADT_LAPIC && e[1] >= 8) {
            uid = e[2];
            id = e[3];
            flags = rd32le(e + 4);
            if (id == 0xFF) /* the broadcast ID: never a CPU */
                continue;
        } else if (e[0] == MADT_X2APIC && e[1] >= 16) {
            id = rd32le(e + 4);
            flags = rd32le(e + 8);
            uid = rd32le(e + 12);
        } else {
            continue;
        }
        /* Online-capable but not enabled means hot-pluggable: absent now. */
        if (!(flags & MADT_ENABLED))
            continue;
        if (id == bsp_apic_id) {
            if (!bsp_seen && cap)
                out[0].acpi_uid = uid;
            bsp_seen = true;
            continue;
        }
        if (listed(out, n, cap, id))
            continue;
        if (n < cap)
            out[n] = (struct smp_madt_cpu){id, uid};
        n++;
    }
    return n;
}

void smp_tramp_install(uint64_t page, const struct smp_tramp_params *p)
{
    uint8_t *dst = (uint8_t *)(uintptr_t)page;
    struct tramp_data *d = (struct tramp_data *)(dst + TRAMP_DATA);

    memcpy(dst, smp_tramp_start, (size_t)(smp_tramp_end - smp_tramp_start));
    d->gdt32_limit = 4 * 8 - 1;
    d->gdt32_base = (uint32_t)page + TRAMP_GDT32;
    d->far32_off = (uint32_t)(page + (uint64_t)(smp_tramp_pm32 - smp_tramp_start));
    d->far32_sel = 0x08;
    d->far64_off = (uint32_t)(page + (uint64_t)(smp_tramp_lm64 - smp_tramp_start));
    d->far64_sel = 0x18;
    d->cr3 = p->cr3;
    d->cr4 = p->cr4;
    d->cr0 = p->cr0;
    d->efer_set = p->efer_set;
    d->pat = p->pat;
    d->xcr0 = p->xcr0;
    d->gdt64_limit = p->gdtr_limit;
    d->gdt64_base = p->gdtr_base;
    d->cpus = p->cpus;
    d->ncpus = p->ncpus;
    d->stop = p->stop;
    d->seen[0] = d->seen[1] = d->seen[2] = 0;
}

void smp_tramp_seen(uint64_t page, uint32_t seen[3])
{
    volatile const struct tramp_data *d = (volatile const struct tramp_data *)(uintptr_t)(page + TRAMP_DATA);

    for (int i = 0; i < 3; i++)
        seen[i] = d->seen[i];
}

/* ---- after ExitBootServices ----------------------------------------------- */

static void delay_us(uint64_t us, uint64_t tsc_hz)
{
    uint64_t t0 = rdtsc(), ticks = us * (tsc_hz / 1000000);

    while (rdtsc() - t0 < ticks)
        __asm__ volatile("pause");
}

struct lapic {
    bool x2apic;
    volatile uint32_t *mmio;
};

static uint32_t lapic_id(const struct lapic *l)
{
    return l->x2apic ? (uint32_t)rdmsr(X2APIC_ID) : l->mmio[XAPIC_ID / 4] >> 24;
}

/* Wait (briefly) until the xAPIC has sent the last IPI; x2APIC has no
 * delivery status to wait on. */
static void icr_idle(const struct lapic *l, uint64_t tsc_hz)
{
    for (int i = 0; !l->x2apic && i < 1000 && (l->mmio[XAPIC_ICR_LO / 4] & ICR_PENDING); i++)
        delay_us(1, tsc_hz);
}

/* To every processor but this one: no destination field. */
static void broadcast(const struct lapic *l, uint32_t icr, uint64_t tsc_hz)
{
    icr_idle(l, tsc_hz);
    if (l->x2apic) {
        wrmsr(X2APIC_ICR, ICR_ALL_BUT_SELF | icr);
    } else {
        l->mmio[XAPIC_ICR_HI / 4] = 0;
        l->mmio[XAPIC_ICR_LO / 4] = ICR_ALL_BUT_SELF | icr;
    }
    icr_idle(l, tsc_hz);
}

static uint32_t parked(const struct boot_smp_cpu *cpus, uint32_t n)
{
    uint32_t k = 0;

    for (uint32_t i = 0; i < n; i++)
        if (((volatile const struct boot_smp_cpu *)&cpus[i])->state == BOOT_SMP_PARKED)
            k++;
    return k;
}

static void mark(void (*progress)(int), int stage)
{
    if (progress)
        progress(stage);
}

/* delay_us with a heartbeat each millisecond; *ms counts across waits. */
static void wait_us(uint64_t us, uint64_t tsc_hz, void (*progress)(int), uint32_t *ms)
{
    for (; us >= 1000; us -= 1000) {
        delay_us(1000, tsc_hz);
        mark(progress, SMP_STAGE_TICK + (int)(*ms)++);
    }
    delay_us(us, tsc_hz);
}

uint32_t smp_start_aps(uint64_t page, struct boot_smp_cpu *cpus, uint32_t n, uint64_t tsc_hz,
                       uint32_t init_delay_us, bool *x2apic, uint32_t *self, void (*progress)(int stage))
{
    uint64_t base = rdmsr(MSR_APIC_BASE);
    struct lapic l = {(base & APIC_BASE_EXTD) != 0, (volatile uint32_t *)(uintptr_t)(base & APIC_BASE_ADDR)};
    uint32_t want = 0, ms = 0, sipi = ICR_SIPI | (uint32_t)(page >> 12);

    *x2apic = l.x2apic;
    *self = 0;
    if (!(base & APIC_BASE_EN) || !tsc_hz || page >= 0x100000 || (page & 0xFFF))
        return 0;
    *self = lapic_id(&l);
    for (uint32_t i = 0; i < n; i++)
        if (cpus[i].state == BOOT_SMP_OFF && cpus[i].apic_id != *self)
            want++;
    if (!want)
        return 0;
    mark(progress, SMP_STAGE_LAPIC);
    broadcast(&l, ICR_INIT, tsc_hz);
    mark(progress, SMP_STAGE_INIT);
    wait_us(init_delay_us, tsc_hz, progress, &ms);
    mark(progress, SMP_STAGE_INIT_WAIT);
    broadcast(&l, sipi, tsc_hz);
    delay_us(SIPI_DELAY_US, tsc_hz);
    mark(progress, SMP_STAGE_SIPI1);
    if (parked(cpus, n) < want) /* a second SIPI is ignored by APs already running */
        broadcast(&l, sipi, tsc_hz);
    mark(progress, SMP_STAGE_SIPI2);
    for (uint64_t waited = 0; parked(cpus, n) < want && waited < PARK_WAIT_US; waited += 1000)
        wait_us(1000, tsc_hz, progress, &ms);
    return parked(cpus, n);
}
