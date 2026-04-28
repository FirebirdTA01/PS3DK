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
 */

	.section	.before_text, "ax", @progbits
	.global	_start
	.type	_start, @function

_start:
	xori	$80, $80, 4
	br	1f
1:
	xori	$80, $80, 4
	br	cellSpursJobMain2
	/* Pad .before_text to 32 bytes so .text always lands at LS 0x30
	 * (= .SpuGUID 0x10 + .before_text 0x20).  The JOB CHAIN
	 * dispatcher rejects images whose code section starts elsewhere
	 * with CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR. */
	nop
	nop
	nop
	nop

	.size	_start, . - _start
