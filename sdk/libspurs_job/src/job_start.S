/*
 * job_start.S - SPU spurs-job entry trampoline.
 *
 * Dispatcher branches here with $0 = return-to-dispatcher, $1 = job
 * stack pointer, $3 = CellSpursJobContext2*, $4 = CellSpursJob256*.
 * Byte-identical to the reference job_start.o for -mspurs-job mode:
 * a 4-instruction xori-dance trampoline that tail-calls cellSpursJobMain2.
 * The xori pair leaves $80 bitwise unchanged so caller-saved state
 * round-trips intact; the linker zero-pads .before_text out to its
 * 32-byte alignment.
 *
 * The 16-byte .SpuGUID section that the linker script anchors at
 * vaddr 0 (pushing _start to vaddr 0x10) is also emitted here so
 * every job binary that links libspurs_job picks one up automatically;
 * without it the binary entry point would land at 0 and dispatcher
 * checks that require e_entry != 0 reject the image with PERM.
 */

	.section	.SpuGUID, "ax", @progbits
	.balign	1
	.long	0x436e8402, 0x42569682, 0x43d9e302, 0x43f75f82

	.section	.before_text, "ax", @progbits
	.global	_start
	.type	_start, @function

_start:
	xori	$80, $80, 4
	br	1f
1:
	xori	$80, $80, 4
	br	cellSpursJobMain2

	.size	_start, . - _start
