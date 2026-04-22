/*
 * compact_target.S — a callee function defined with a hand-emitted
 * 8-byte compact function descriptor, to probe whether the Lv-2
 * loader and our toolchain cooperate to call through it correctly.
 *
 * Shape of the descriptor matches reference SDK post-link output:
 *   [+0] 32-bit entry EA
 *   [+4] 32-bit module TOC base EA (.TOC.@tocbase truncated)
 *
 * The function takes one int and returns it + 100. The caller
 * (main.c) invokes it via inline-asm 2-read lwz sequence to confirm
 * a compact descriptor is callable end-to-end.
 */

	.section ".text","ax",@progbits
	.globl .compact_target
	.type  .compact_target, @function
	.align 2
.compact_target:
	/* r3 = arg. Return r3 + 100. We deliberately do NOT read r2,
	 * so the caller is free to pass any TOC value (including 0).
	 * Using raw register numbers — no ppc-asm.h dependency. */
	addi	3, 3, 100
	blr
	.size .compact_target, . - .compact_target

	/* 8-byte compact descriptor. Placed in .opd with WA flags.
	 * For this probe, the TOC field is a placeholder .long 0;
	 * compact_target doesn't use r2 so any value works. When the
	 * real compact-opd migration lands, the linker resolves a
	 * TLSGD marker here to the module TOC base. */
	.section ".opd","aw",@progbits
	.align 2
	.globl compact_target
	.type  compact_target, @object
	.size  compact_target, 8
compact_target:
	.long .compact_target          /* +0: entry EA (32-bit) */
	.long 0                        /* +4: TOC placeholder (unused by callee) */
