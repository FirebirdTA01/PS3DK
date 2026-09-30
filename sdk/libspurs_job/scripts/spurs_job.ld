/*
 * spurs_job.ld - SPU linker script for cellSpursJobMain2 binaries.
 *
 * Layout requirements imposed by the SPRX job dispatcher:
 *   - _start must sit at the binary's entry offset (image offset 0x10).
 *     Our trampoline lives in .before_text; the script anchors that
 *     section at the start of the image.
 *   - .bss must be bracketed by __bss_start / _end so that
 *     cellSpursJobInitialize knows the clear range.
 *
 * No exception tables and no crt0; .ctors/.dtors and .fixup are kept for
 * jobs linked with -mspurs-job-initialize (see libspurs_jq job_runtime.c).
 * Default newlib linker scripts assume a hosted runtime and
 * pull in start files - drive the SPU GCC link with -nostartfiles -T
 * spurs_job.ld to use this layout.
 */

OUTPUT_FORMAT("elf32-spu")
OUTPUT_ARCH(spu)
ENTRY(_start)

PHDRS
{
    /* Two LOAD segments + one NOTE.  We emit the NOTE phdr explicitly
     * so .note.spu_name lands in its own PT_NOTE segment as the
     * jobbin2-wrapper tool expects. */
    text  PT_LOAD;
    data  PT_LOAD;
    note  PT_NOTE;
}

SECTIONS
{
    /* Image base.  A job without the job CRT (-mspurs-job) runs where
     * the job manager loads BINARY2 images, LS 0x4c00: its job_start.o
     * defines __spurs_job_base so that addresses stored in its data are
     * right with no run-time relocation.  A job with the job CRT
     * (-mspurs-job-initialize) is linked at 0 and relocates itself from
     * its .fixup table (libspurs_jq job_runtime.c).  Layout from the
     * base: .SpuGUID (16 bytes), then .before_text + _start at +0x10. */
    . = DEFINED(__spurs_job_base) ? __spurs_job_base : 0;

    /* .SpuGUID (16 bytes) - the jobbin2-wrapper tool reads this named
     * section to compute the per-binary SPU GUID.  Placed first so it
     * occupies vaddr 0..0xf and pushes _start to vaddr 0x10. */
    .SpuGUID : {
        KEEP(*(.SpuGUID))
        . = ALIGN(16);
    } :text

    /* Entry trampoline.  KEEP guards against -Wl,--gc-sections
     * dropping it just because nothing in .text references it
     * statically (the SPRX dispatcher resolves the symbol at
     * load time, not link time). */
    .before_text : ALIGN(16) {
        KEEP(*(.before_text))
        . = ALIGN(16);
        /* End of the job CRT header: its _end / __bss_start words are
         * image offsets the job-queue kernel reads, so the job runtime
         * must never relocate them. */
        __job_crt_header_end = .;
    } :text =0x40200000

    .text : ALIGN(16) {
        *(.text)
        *(.text.*)
        . = ALIGN(16);
    } :text =0x40200000

    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(16);
    } :text =0x00000000

    /* Words holding absolute addresses, listed by ld --emit-fixups (PIC
     * jobs with vtables or pointer tables); the job runtime relocates
     * them to the load address.  Ends with a zero record. */
    .fixup : ALIGN(16) {
        __fixup_start = .;
        KEEP(*(.fixup))
        . = ALIGN(16);
    } :text =0x00000000

    /* The jobbin2-wrapper tool requires LOAD-data's vaddr to equal
     * LOAD-code's end aligned to 0x80, so bump . to 0x80-alignment
     * before the data segment starts. */
    . = ALIGN(0x80);

    .data : ALIGN(0x80) {
        /* KEEP _cell_spu_ls_param: defined in .data by the
         * CELL_SPU_LS_PARAM macro, referenced only by the
         * jobbin2-wrapper tool at build time, so without KEEP
         * --gc-sections drops it. */
        KEEP(*(.data))
        *(.data.*)
        . = ALIGN(16);
    } :data =0x00000000

    /* Global constructors and destructors, run by the job runtime's
     * _init / _fini for -mspurs-job-initialize jobs. */
    .ctors : ALIGN(4) {
        __ctors_start = .;
        KEEP(*(SORT(.ctors.*)))
        KEEP(*(.ctors))
        __ctors_end = .;
    } :data
    .dtors : ALIGN(4) {
        __dtors_start = .;
        KEEP(*(SORT(.dtors.*)))
        KEEP(*(.dtors))
        __dtors_end = .;
    } :data
    .init_array : ALIGN(4) {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :data
    .fini_array : ALIGN(4) {
        __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } :data

    /* Companion `.cell_spu_ls_param` section (8 bytes: heap, stack)
     * the jobbin2-wrapper tool reads to populate metadata.  Non-
     * loadable - read at build time only. */
    .cell_spu_ls_param : {
        KEEP(*(.cell_spu_ls_param))
    }

    /* PROVIDE so a job binary that defines its own __bss_start
     * (rare) can override; default points at the .bss section
     * begin. */
    . = ALIGN(16);
    __bss_start = .;

    .bss : ALIGN(16) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } :data

    . = ALIGN(16);
    _end = .;

    /* PT_NOTE segment carrying the spu_name note (type=1). */
    .note.spu_name : {
        KEEP(*(.note.spu_name))
    } :note

    /DISCARD/ : {
        *(.comment)
        *(.note)
        *(.note.[!s]*)         /* discard everything but .note.spu_name */
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
