/*
 * job_start.S - SPU spurs-job entry trampoline.
 *
 * Dispatcher branches here with $0 = return-to-dispatcher, $1 = job
 * stack pointer, $3 = CellSpursJobContext2*, $4 = CellSpursJob256*.
 * 4-instruction xori-dance at LS 0x10..0x1f tail-calls cellSpursJobMain2;
 * the xori pair leaves $80 bitwise unchanged so caller-saved state
 * round-trips intact.  The 16-byte .before_text tail at LS 0x20..0x2f
 * carries the BINARY2 magic word the JM2 dispatcher checks before
 * accepting the image; see docs/abi/spurs-job-entry-point.md § 3.1.
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
	.size	_start, . - _start

	/* "bin2" magic at LS 0x20..0x23 - the JM2 dispatcher loads a
	 * 16-byte block from LS 0x20 and accepts it as a valid BINARY2
	 * job image only if the leading word matches "BIN2" (uppercase)
	 * or "bin2" (lowercase).  Without the magic the dispatcher's
	 * deferred validator sets the JOB_DESCRIPTOR error
	 * (0x80410a0b) and traps via jmHalt at LS 0xee8. */
	.ascii	"bin2"
	.zero	12
