/* cellSpursExit / cellSpursTaskExit - hand-scheduled to match the
 * reference libspurs.a shape so the SPU dual-issue pipeline overlaps
 * the two independent LS loads (control vector at 0x2fb0 and state
 * vector at 0x2fd0) instead of paying each load's latency twice.
 *
 * Calling convention (CellOS SPU ABI):
 *   cellSpursExit:     void (*)(void)       - r3 preserved irrelevant
 *   cellSpursTaskExit: void (*)(int code)   - r3 = exit code on entry
 *
 * Both are noreturn - the dispatch at dispatch_base+0xc4 with r3=0
 * (SPURS_SVC_EXIT) hands control back to the SPRX scheduler and the
 * task's code never resumes from that call.
 */
	.text

	.globl	cellSpursExit
	.type	cellSpursExit, @function
	.align	3
cellSpursExit:
	il	$3, 0                  /* service ID 0 = EXIT */
	lqa	$7, 0x2fb0             /* load 16B control vector */
	stqd	$0, 16($1)             /* save LR (convention; we don't return) */
	stqd	$1, -32($1)            /* link new frame */
	ai	$1, $1, -32
	rotqbyi	$6, $7, 8              /* dispatch_base into slot 0 of $6 */
	ai	$2, $6, 196            /* $2 = dispatch_base + 0xc4 */
	lqd	$5, 0($2)              /* load 16B containing dispatch fn ptr */
	rotqby	$4, $5, $2             /* align to the 4B slot at $2 */
	bisl	$0, $4                 /* branch and link - SPRX takes over */
	.size	cellSpursExit, . - cellSpursExit

	.globl	cellSpursTaskExit
	.type	cellSpursTaskExit, @function
	.align	3
cellSpursTaskExit:
	stqd	$0, 16($1)
	stqd	$1, -32($1)
	ai	$1, $1, -32
	lqa	$8, 0x2fb0             /* control vec */
	lqa	$9, 0x2fd0             /* state vec (contains exit code slot) */
	cwd	$10, 0($1)             /* shuffle mask: insert word at slot 0 */
	rotqbyi	$7, $8, 8              /* dispatch_base */
	shufb	$6, $3, $9, $10        /* merge r3 (code) into state vec slot 0 */
	il	$3, 0                  /* service ID = EXIT */
	ai	$4, $7, 196            /* dispatch_base + 0xc4 */
	stqa	$6, 0x2fd0             /* commit state vec */
	lqd	$5, 0($4)
	rotqby	$2, $5, $4
	bisl	$0, $2
	.size	cellSpursTaskExit, . - cellSpursTaskExit
