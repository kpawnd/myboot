/*
 * Exception stubs for the loader's IDT after ExitBootServices, when the
 * firmware's handlers are gone. Vector i's stub is at exc_stubs + 16 * i;
 * it calls exc_report(i) (MS x64 ABI), which never returns.
 */
    .text
    .globl exc_stubs
    .p2align 4
exc_stubs:
    .set vec, 0
    .rept 32
    .p2align 4
    movl    $vec, %ecx
    jmp     exc_common
    .set vec, vec + 1
    .endr
exc_common:
    andq    $-16, %rsp
    subq    $32, %rsp
    call    exc_report
1:  cli
    hlt
    jmp     1b
