/*
 * lv2-crti.S - .init / .fini prologues with compact-OPD function
 *              descriptors.
 *
 * The C runtime calls _init and _fini at startup and exit. Each needs
 * a function descriptor in .opd plus its actual code in .init / .fini.
 * lv2-crtn.S provides the matching epilogue that closes both sections.
 *
 * Descriptor layout is the 8-byte compact form [entry_ea_32, toc_ea_32]
 * with 4-byte alignment - matches what GCC emits under POWERPC_CELL64LV2
 * for rs6000_elf_declare_function_name, so the final .opd section ends
 * up uniformly compact and the binutils skip-OPD-edit path triggers.
 *
 * PSL1GHT's stock lv2-crti.o uses `.quad ._init, .TOC.@tocbase, 0` and
 * marks .opd "ax" - that produces 24-byte ELFv1 descriptors and drags
 * the final .opd alignment up to 8, which defeats the compact path.
 */

#include <ppc-asm.h>

	.section ".opd","aw"
	.align 2
	.globl _init
	.type _init, @function
	.size _init, 8
_init:
	.long ._init, .TOC.

	.section ".init","ax"
	.globl ._init
	.type ._init, @function
._init:
	mflr	r0
	std	r0, 16(r1)
	stdu	r1, -128(r1)

	.section ".opd","aw"
	.align 2
	.globl _fini
	.type _fini, @function
	.size _fini, 8
_fini:
	.long ._fini, .TOC.

	.section ".fini","ax"
	.globl ._fini
	.type ._fini, @function
._fini:
	mflr	r0
	std	r0, 16(r1)
	stdu	r1, -128(r1)
