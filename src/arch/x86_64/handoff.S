/*
 * boot_handoff: switch to the kernel's address space and jump to it.
 *
 *   void boot_handoff(uint64_t cr3,          RCX
 *                     uint64_t gdtr_virt,    RDX
 *                     uint64_t stack_top,    R8
 *                     uint64_t bootinfo,     R9
 *                     uint64_t entry);       [RSP+40] (MS x64 ABI: 5th arg on
 *                                            the stack above 32 bytes of
 *                                            shadow space + return address)
 *
 * This code runs from the loader image, which the new page tables map
 * identity (an 8 KiB window), so instruction fetch survives the CR3 write.
 * Nothing else of the loader is mapped afterwards: the old stack is gone,
 * so every argument is taken into registers first.
 */
    .text
    .globl boot_handoff
boot_handoff:
    cli
    movq    40(%rsp), %rax      /* entry, read while the old stack is mapped */
    movq    %rcx, %cr3          /* new address space; also flushes the TLB */
    movq    %r8, %rsp           /* kernel stack (HHDM address) */
    lgdt    (%rdx)              /* our GDT, via its HHDM address */

    /* Reload CS with a far return: push selector, then target. */
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

    movq    %r9, %rdi           /* SysV first argument: boot_info */
    xorl    %ebp, %ebp          /* terminate frame-pointer chains */
    pushq   $0                  /* fake return address: RSP+8 is 16-aligned */
    jmpq    *%rax
