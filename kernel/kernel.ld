ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
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
