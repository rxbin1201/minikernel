ENTRY(_start)

SECTIONS
{
    . = 0x7F8000000000; /* USER_BASE (siehe Kernel/paging.h) */

    .text   : { *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data   : { *(.data .data.*) }
    . = ALIGN(4096);
    .bss    : { *(COMMON) *(.bss .bss.*) }
    . = ALIGN(4096);

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
