/*
 * crt0.S - native CellOS Lv-2 process entry.
 *
 * Replaces PSL1GHT's crt0.S. The only behavioural delta is the `__start`
 * OPD descriptor: PSL1GHT emits the 24-byte ELFv1 form with
 *   .quad ._start, .TOC.@tocbase, 0
 * which mixes with the 8-byte compact descriptors our toolchain emits
 * elsewhere and drags the final .opd alignment up to 8, defeating the
 * binutils skip-OPD-edit trigger.
 *
 * Build-time: linked with crt1.o to produce lv2-crt0.o (see
 * scripts/build-runtime-lv2.sh).
 */

#include <ppc-asm.h>

#define LOAD_DWORD(reg, sym)		\
	li	reg,(sym)@higher;		\
	oris	reg,reg,(sym)@highest;	\
	sldi	reg,reg,32;				\
	oris	reg,reg,(sym)@h;		\
	ori	reg,reg,(sym)@l

	.globl ._start
._start:
	/* Standard PPU process prolog: save LR in the caller's frame, push a
	 * 128-byte activation record, anchor __libc_stack_end, then hand
	 * control to _initialize. The binutils compact-OPD fix in
	 * patches/ppu/binutils-2.42/0002-elf64-ppc-skip-opd-edit-for-compact-opd
	 * teaches the linker to redirect `bl foo' calls whose target
	 * resolves to a compact OPD entry, so a plain `bl' lands on the
	 * callee's code section (._initialize) rather than the OPD data. */
	LOAD_DWORD(r2,__libc_stack_end)
	std	r1,0(r2)
	mflr	r0
	stdu	r1,-128(r1)
	std	r0,112(r1)
	li	r2,0
	std	r2,0(r1)
	/* Load the callee TOC from __start's 8-byte compact OPD descriptor.
	 * Classic PPC64 ELFv1 had TOC at offset 8 inside a 24-byte descriptor
	 * (`ld r2, 8(r2)'); compact OPD puts it at offset 4 inside an 8-byte
	 * descriptor, and reading 8 bytes at +8 overruns the descriptor into
	 * whatever follows in .opd. Use lwz at +4 instead. */
	LOAD_DWORD(r2,__start)
	lwz	r2,4(r2)
	bl	_initialize
	ld	r0,112(r1)
	addi	r1,r1,128
	mtlr	r0
	blr

	.section ".bss","aw"
	.globl __libc_stack_end
__libc_stack_end:
	.quad	0

	.section ".rodata","a"
	.globl _start
_start:
	.long ._start
	.long ._start
	.quad 0,0

	/* __start's function descriptor - compact 8-byte [entry_ea,TOC]. */
	.section ".opd","aw"
	.align 2
	.globl __start
	.type __start, @function
	.size __start, 8
__start:
	.long ._start, .TOC.
