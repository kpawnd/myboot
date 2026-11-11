/* AP startup trampoline (Intel SDM vol. */
    .set D_GDT32,    8
    .set D_STOP,     14
    .set D_FAR32,    16
    .set D_FAR64,    24
    .set D_CR3,      32
    .set D_CR4,      40
    .set D_CR0,      48
    .set D_EFER,     56
    .set D_PAT,      64
    .set D_XCR0,     72
    .set D_GDT64,    80
    .set D_SEEN16,   90
    .set D_SEEN32,   92
    .set D_SEEN64,   94
    .set D_CPUS,     96
    .set D_NCPUS,    104
    .set GDT32,      112

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
    .quad   0x00CF9A000000FFFF
    .quad   0x00CF92000000FFFF
    .quad   0x00AF9A000000FFFF
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
    orl     $1, %eax
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
    movl    D_CR4(%ebx), %eax
    movl    %eax, %cr4
    movl    D_CR3(%ebx), %eax
    movl    %eax, %cr3
    movl    $0xC0000080, %ecx
    rdmsr
    orl     D_EFER(%ebx), %eax
    wrmsr
    movl    $0x277, %ecx
    movl    D_PAT(%ebx), %eax
    movl    D_PAT+4(%ebx), %edx
    wrmsr
    movl    D_CR0(%ebx), %eax
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
dead:
    cli
    hlt
    jmp     dead

found:
    movq    CPU_STACK(%rsi), %rsp
    pushq   $0x08
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
    pushq   $0
    jmpq    *%rax
smp_tramp_end:
