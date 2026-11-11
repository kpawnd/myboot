/* boot_handoff: switch to the kernel's address space and jump to it. */
    .text
    .globl boot_handoff
boot_handoff:
    cli
    movq    40(%rsp), %rax
    movq    %rcx, %cr3
    movq    %r8, %rsp
    lgdt    (%rdx)

    pushq   $0x08
    leaq    1f(%rip), %r10
    pushq   %r10
    lretq
1:
    movl    $0x10, %r11d
    movw    %r11w, %ds
    movw    %r11w, %es
    movw    %r11w, %ss
    movw    %r11w, %fs
    movw    %r11w, %gs

    movq    %r9, %rdi
    xorl    %ebp, %ebp
    pushq   $0
    jmpq    *%rax
