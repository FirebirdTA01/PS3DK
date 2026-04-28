/*
 * job_start_w_crt.S - SPU spurs-job-initialize entry trampoline.
 *
 * The "with CRT" form of _start that JQ workloads require.  Layout
 * mirrors the reference job_start_w_crt.o byte-for-byte: 4-instruction
 * xori-dance trampoline that tail-calls __job_start, followed by 16
 * bytes of 0xa5 fill, the "JOBCRT Ver13" ASCII magic at offset 0x20,
 * ADDR32 relocations to _end and __bss_start at 0x2c / 0x30, and a
 * trailing stop word.  Total .before_text = 80 bytes.
 *
 * The reference packager + the JQ kernel both check for the "JOBCRT"
 * magic at offset 0x20 of the loaded SPU image (= LS 0x30 once the
 * .SpuGUID prefix lands at LS 0x10..0x1f and .before_text follows).
 * Without it the kernel rejects the binary as a non-CRT job.
 *
 * .SpuGUID is also emitted here so JQ jobs that link libspurs_jq pick
 * up a valid GUID without separately linking libspurs_job.
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
	br	__job_start

	/* 16 bytes 0xa5 fill */
	.long	0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5

	/* "JOBCRT Ver13" magic - 12 bytes ASCII (BE on SPU) */
	.long	0x4a4f4243         /* "JOBC" */
	.long	0x52542056         /* "RT V" */
	.long	0x65723133         /* "er13" */

	/* ADDR32 relocations the linker fills in with the runtime _end /
	 * __bss_start LS addresses; the JQ kernel reads them to compute
	 * the binary's BSS clear range. */
	.long	_end
	.long	__bss_start

	/* 16 bytes alignment padding */
	.long	0, 0, 0, 0

	/* Filler + trailing stop sentinel */
	.long	0xffffffff
	.long	0x00000000

	.size	_start, . - _start
