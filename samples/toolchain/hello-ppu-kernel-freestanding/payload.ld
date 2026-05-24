OUTPUT_FORMAT("elf64-powerpc")
OUTPUT_ARCH(powerpc:common64)
ENTRY(_start)

PHDRS
{
    load PT_LOAD FLAGS(7);
}

SECTIONS
{
    . = 0;
    __image_start = .;

    .text : ALIGN(16)
    {
        KEEP(*(.text.start))
        *(.text .text.*)
    } :load

    .rodata : ALIGN(16)
    {
        *(.rodata .rodata.*)
    } :load

    .data : ALIGN(16)
    {
        KEEP(*(.data.keep))
        *(.data .data.*)
    } :load

    .opd : ALIGN(8)
    {
        KEEP(*(.opd))
    } :load

    .toc : ALIGN(8)
    {
        __toc_start = .;
        *(.toc .toc.*)
        *(.got .got.*)
        __toc_end = .;
    } :load

    __toc = ADDR(.toc) + 0x8000;
    .TOC. = __toc;

    .bss : ALIGN(16)
    {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        __bss_end = .;
    } :load

    __image_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
