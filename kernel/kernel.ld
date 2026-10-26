/*
 * Test kernel layout: three PT_LOAD segments with distinct permissions so
 * the loader's W^X mapping can be verified from inside the kernel.
 * Linked in the top 2 GiB (-mcmodel=kernel), as the boot protocol requires.
 */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);  /* R-X */
    rodata PT_LOAD FLAGS(4);  /* R-- */
    data   PT_LOAD FLAGS(6);  /* RW- */
}

SECTIONS
{
    . = 0xFFFFFFFF80000000;

    __kernel_text = .;
    .text : { *(.text .text.*) } :text

    . = ALIGN(4096);
    __kernel_rodata = .;
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(4096);
    __kernel_data = .;
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.* COMMON) } :data
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
