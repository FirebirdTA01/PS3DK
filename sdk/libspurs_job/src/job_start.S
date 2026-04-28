/*
 * job_start.S - SPU spurs-job entry trampoline.
 *
 * The SPRX job dispatcher branches to the binary's _start symbol with
 * $0 = return-to-dispatcher, $1 = job-allocated stack pointer,
 * $3 = CellSpursJobContext2*, $4 = CellSpursJob256*.  Reference
 * job_start.o ships this exact 4-instruction sequence in .before_text,
 * padded out to 32 bytes:
 *
 *   xori $80, $80, 4          ; toggle bit 2 of $80
 *   br   _start+0x8           ; (no-op forward branch)
 *   xori $80, $80, 4          ; toggle back -> net no-op on $80
 *   br   cellSpursJobMain2    ; tail-jump to user entry
 *
 * The xori pair leaves $80 bitwise unchanged so callee-saved state
 * the dispatcher placed there round-trips intact; the dispatcher
 * appears to recognise this entry shape as a job-start signature.
 * The trailing nops bring .before_text to the canonical 32-byte size.
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
	nop
	nop
	nop
	nop

	.size	_start, . - _start
