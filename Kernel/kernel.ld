ENTRY(_start)

SECTIONS
{
    . = 0x100000;
    __kernel_start = .;

    __text_start = .;
    .text   : { *(.text .text.*) }
    . = ALIGN(4096);
    __text_end = .;

    __rodata_start = .;
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    __rodata_end = .;

    __data_start = .;
    .data   : { *(.data .data.*) }
    . = ALIGN(4096);
    .bss    : { *(COMMON) *(.bss .bss.*) }
    . = ALIGN(4096);
    __data_end = .;

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
