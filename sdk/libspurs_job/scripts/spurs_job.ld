/*
 * spurs_job.ld - SPU linker script for cellSpursJobMain2 binaries.
 *
 * Layout requirements imposed by the SPRX job dispatcher:
 *   - _start must sit at the binary's entry offset (LS offset 0 once
 *     the binary is loaded).  Our trampoline lives in .before_text;
 *     the script anchors that section at the start of the image.
 *   - .bss must be bracketed by __bss_start / _end so that
 *     cellSpursJobInitialize knows the clear range.
 *
 * Job binaries are tiny: no .ctors/.dtors, no exception tables, no
 * crt0.  Default newlib linker scripts assume a hosted runtime and
 * pull in start files - drive the SPU GCC link with -nostartfiles -T
 * spurs_job.ld to use this layout.
 */

OUTPUT_FORMAT("elf32-spu")
OUTPUT_ARCH(spu)
ENTRY(_start)

SECTIONS
{
    . = 0;

    /* Entry trampoline.  KEEP guards against -Wl,--gc-sections
     * dropping it just because nothing in .text references it
     * statically (the SPRX dispatcher resolves the symbol at
     * load time, not link time). */
    .before_text : ALIGN(16) {
        KEEP(*(.before_text))
        . = ALIGN(16);
    } =0x40200000

    .text : ALIGN(16) {
        *(.text)
        *(.text.*)
        . = ALIGN(16);
    } =0x40200000

    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(16);
    } =0x00000000

    .data : ALIGN(16) {
        *(.data)
        *(.data.*)
        . = ALIGN(16);
    } =0x00000000

    /* PROVIDE so a job binary that defines its own __bss_start
     * (rare) can override; default points at the .bss section
     * begin. */
    . = ALIGN(16);
    __bss_start = .;

    .bss : ALIGN(16) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    . = ALIGN(16);
    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
