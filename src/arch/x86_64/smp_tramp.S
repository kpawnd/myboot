/*
 * AP startup trampoline (Intel SDM vol. 3A 9.9 "Mode switching"). Never run
 * in place: smp_tramp_install copies [smp_tramp_start, smp_tramp_end) to a
 * page below 1 MiB and fills in the data block. A SIPI with that page's
 * number starts an AP in real mode at CS = page >> 4, IP = 0.
 *
 *   real mode -> protected mode (flat, this page's GDT)
 *             -> CR4, CR3 (the kernel's tables), EFER.LME, PAT, CR0.PG:
 *                long mode, still on this page (identity-mapped there)
 *             -> 64-bit: kernel GDT, find our boot_smp_cpu by APIC ID,
 *                its stack, XCR0, mark it parked, spin on goto_address.
 *
 * Every AP runs this at once, so the page is written only by locked
 * increments of the D_SEEN* counters (how many APs reached 16-, 32- and
 * 64-bit code); otherwise each AP writes only its own boot_smp_cpu entry.
 * D_STOP, if not 0, halts every AP at that point instead (1: 16-bit,
 * 2: 32-bit, 3: 64-bit, 4: just before parking), for diagnosis.
 * EBX/R15 hold the page's linear address throughout.
 *
 * The data block's layout is struct tramp_data in smp.c; the offsets
 * below must match it.
 */
    .set D_GDT32,    8    /* limit:16, base:32 */
    .set D_STOP,     14
    .set D_FAR32,    16   /* offset:32, selector:16 */
    .set D_FAR64,    24
    .set D_CR3,      32
    .set D_CR4,      40
    .set D_CR0,      48
    .set D_EFER,     56
    .set D_PAT,      64
    .set D_XCR0,     72
    .set D_GDT64,    80   /* limit:16, base:64 */
    .set D_SEEN16,   90
    .set D_SEEN32,   92
    .set D_SEEN64,   94
    .set D_CPUS,     96
    .set D_NCPUS,    104
    .set GDT32,      112

    /* struct boot_smp_cpu */
    .set CPU_SIZE,   64
    .set CPU_ID,     0
    .set CPU_STATE,  8
    .set CPU_STACK,  16
    .set CPU_GOTO,   24
    .set PARKED,     2

    .text
    .globl smp_tramp_start, smp_tramp_end, smp_tramp_pm32, smp_tramp_lm64

    .code16
smp_tramp_start:
    jmp     start16
    .org    GDT32
    .quad   0
    .quad   0x00CF9A000000FFFF      /* 0x08: 32-bit code, flat */
    .quad   0x00CF92000000FFFF      /* 0x10: data, flat */
    .quad   0x00AF9A000000FFFF      /* 0x18: 64-bit code */
start16:
    cli
    cld
    xorl    %ebx, %ebx
    movw    %cs, %bx
    movw    %bx, %ds
    shll    $4, %ebx
    lock incw (D_SEEN16)
    cmpw    $1, (D_STOP)
    je      halt16
    lgdtl   (D_GDT32)
    movl    %cr0, %eax
    orl     $1, %eax                /* PE */
    movl    %eax, %cr0
    ljmpl   *(D_FAR32)
halt16:
    cli
    hlt
    jmp     halt16

    .code32
smp_tramp_pm32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    lock incw D_SEEN32(%ebx)
    cmpw    $2, D_STOP(%ebx)
    je      halt32
    movl    D_CR4(%ebx), %eax       /* PAE (and LA57) before paging */
    movl    %eax, %cr4
    movl    D_CR3(%ebx), %eax
    movl    %eax, %cr3
    movl    $0xC0000080, %ecx       /* EFER: LME, NXE */
    rdmsr
    orl     D_EFER(%ebx), %eax
    wrmsr
    movl    $0x277, %ecx            /* PAT, before any PTE is used */
    movl    D_PAT(%ebx), %eax
    movl    D_PAT+4(%ebx), %edx
    wrmsr
    movl    D_CR0(%ebx), %eax       /* PG: long mode is now active */
    movl    %eax, %cr0
    ljmpl   *D_FAR64(%ebx)
halt32:
    cli
    hlt
    jmp     halt32

    .code64
smp_tramp_lm64:
    movl    %ebx, %r15d
    lock incw D_SEEN64(%r15)
    cmpw    $3, D_STOP(%r15)
    je      dead
    lgdt    D_GDT64(%r15)

    /* Our APIC ID: the x2APIC ID from leaf 0Bh when it is there, as the
     * loader's MADT list and boot_cpu.bsp_apic_id use it, else CPUID.1. */
    xorl    %eax, %eax
    cpuid
    cmpl    $0x0B, %eax
    jb      1f
    movl    $0x0B, %eax
    xorl    %ecx, %ecx
    cpuid
    testl   %ebx, %ebx
    jz      1f
    movl    %edx, %edi
    jmp     2f
1:  movl    $1, %eax
    cpuid
    shrl    $24, %ebx
    movl    %ebx, %edi
2:  movq    D_CPUS(%r15), %rsi
    movq    D_NCPUS(%r15), %rcx
3:  testq   %rcx, %rcx
    jz      dead
    cmpl    %edi, CPU_ID(%rsi)
    je      found
    addq    $CPU_SIZE, %rsi
    decq    %rcx
    jmp     3b
dead:                               /* not in the list: nothing to run */
    cli
    hlt
    jmp     dead

found:
    movq    CPU_STACK(%rsi), %rsp
    pushq   $0x08                   /* reload CS from the kernel GDT */
    leaq    4f(%rip), %rax
    pushq   %rax
    lretq
4:  movl    $0x10, %eax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs
    movq    D_XCR0(%r15), %rax
    testq   %rax, %rax
    jz      5f
    movq    %rax, %rdx
    shrq    $32, %rdx
    xorl    %ecx, %ecx
    xsetbv
5:  cmpw    $4, D_STOP(%r15)
    je      dead
    movl    $PARKED, CPU_STATE(%rsi)
6:  pause
    movq    CPU_GOTO(%rsi), %rax
    testq   %rax, %rax
    jz      6b
    movq    %rsi, %rdi
    xorl    %ebp, %ebp
    pushq   $0                      /* fake return address, as for the BSP */
    jmpq    *%rax
smp_tramp_end:
