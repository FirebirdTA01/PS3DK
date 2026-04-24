/* SPU linker script for Spurs task ELFs.
 *
 * Places all code and data starting at LS offset 0x3000
 * (CELL_SPURS_TASK_TOP).  Bytes 0..0x3000 belong to the SPRX's code
 * and per-task control blocks at 0x2fb0 / 0x2fd0 - tasks must not
 * occupy that region.  Entry is our tiny crt in .text.__spurs_task_crt
 * which is forced first inside .text.
 */
OUTPUT_FORMAT("elf32-spu", "elf32-spu", "elf32-spu")
OUTPUT_ARCH(spu)
ENTRY(__spurs_task_start)

SECTIONS
{
    . = 0x3000;

    .text : {
        KEEP(*(.text.__spurs_task_crt))
        *(.text .text.* .gnu.linkonce.t.*)
    }

    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    }

    .ctors : { KEEP(*(.ctors)) KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*))) }
    .dtors : { KEEP(*(.dtors)) KEEP(*(SORT_BY_INIT_PRIORITY(.dtors.*))) }

    .data : {
        *(.data .data.* .gnu.linkonce.d.*)
    }

    .bss : {
        *(.dynbss)
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
    }

    /DISCARD/ : { *(.note.GNU-stack) *(.note.gnu.property) }
}
