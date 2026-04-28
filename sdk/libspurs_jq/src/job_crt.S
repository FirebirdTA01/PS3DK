/*
 * job_crt.S - SPU __job_start wrapper for spurs-job-initialize jobs.
 *
 * Called from _start (in job_start_w_crt.S) with $3 =
 * CellSpursJobContext2*, $4 = CellSpursJob256*.  Runs the
 * Initialize -> _init -> user main -> _fini -> Finalize chain that
 * the JQ kernel expects.  Byte-shape mirrors the reference job_crt.o
 * __job_start (~84 bytes); register-save sequence is fixed by ABI.
 */

	.text
	.global	__job_start
	.type	__job_start, @function

__job_start:
	stqd	$0, 16($1)              /* save LR to caller frame */
	stqd	$80, -16($1)            /* save $80 */
	ori	$80, $3, 0              /* $80 = ctx */
	stqd	$81, -32($1)            /* save $81 */
	ori	$81, $4, 0              /* $81 = job */
	stqd	$1, -64($1)             /* save SP backchain */
	ai	$1, $1, -64             /* allocate 64-byte frame */
	brsl	$0, _cellSpursJobCrtAuxInitialize
	brsl	$0, _init
	ori	$4, $81, 0              /* restore $4 = job */
	ori	$3, $80, 0              /* restore $3 = ctx */
	brsl	$0, cellSpursJobMain2
	brsl	$0, __do_atexit
	brsl	$0, _fini
	ori	$3, $80, 0              /* $3 = ctx for finalize */
	lqd	$0, 80($1)              /* restore LR */
	ai	$1, $1, 64              /* deallocate frame */
	shlqbyi	$4, $81, 0              /* $4 = job for finalize */
	lqd	$80, -16($1)
	lqd	$81, -32($1)
	br	_cellSpursJobCrtAuxFinalize  /* tail-call */

	.size	__job_start, . - __job_start
