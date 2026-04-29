/* cellSpursSemaphoreP - "P" / wait on a CellSpursSemaphore.
 *
 * Args:
 *   r3 = ptr to CellSpursSemaphore (16-byte aligned)
 *
 * Behavior:
 *   - Validate ptr non-null and 16-byte aligned (0x80410911/0x80410910).
 *   - Call _cellSpursTaskCanCallBlockWait; bail if it returns non-zero.
 *   - GETLLAR/PUTLLC retry loop on the semaphore cache line at LS 0x80
 *     (128 bytes from EA in r3):
 *       * if count > 0: count--, commit, return success
 *       * else: append our task-id to the waiter slot at LS 0x90 and
 *         the dispatch-base info at LS 0xa0, commit, then call the
 *         BLOCK service at dispatch_base+0xc4 with service-id 2.
 *   - The BLOCK service yields to SPRX; resumes here once V() runs.
 *
 * Hand-translated to byte-match libspurs.a/semaphoreP.o.
 * Two REL16 relocs against _cellSpursTaskCanCallBlockWait at 0x60+0x164.
 * Size: 480 bytes.
 */
	.text
	.globl	cellSpursSemaphoreP
	.type	cellSpursSemaphoreP, @function
	.align	3
cellSpursSemaphoreP:
	il	$5, 0
	hbrr	.L_sP_h0_brnz, .L_sP_epilog
	ilhu	$8, 0x8041
	stqd	$81, -32($1)
	ori	$81, $3, 0
	stqd	$0, 16($1)
	iohl	$8, 0x911
	stqd	$80, -16($1)
	ceq	$4, $81, $5
	stqd	$126, -48($1)
	stqd	$1, -80($1)
	gb	$3, $4
	ai	$1, $1, -80
	cgti	$2, $3, 11
.L_sP_h0_brnz:
	brnz	$2, .L_sP_epilog
	shlqbyi	$80, $81, 4
	ilhu	$8, 0x8041
	iohl	$8, 0x910
	andi	$6, $80, 0x7f
	brnz	$6, .L_sP_epilog
	br	.L_sP_call_check
.L_sP_err_901:
	ilhu	$8, 0x8041
	iohl	$8, 0x901
	br	.L_sP_epilog
.L_sP_call_check:
	brsl	$0, _cellSpursTaskCanCallBlockWait
	ori	$8, $3, 0
	brnz	$3, .L_sP_epilog
	lqa	$19, 0x2fb0
	ori	$12, $80, 0
	cbd	$16, 0($1)
	il	$10, 0x80
	fsmbi	$15, 0
	il	$18, 0xd0
	shlqbyi	$11, $81, 0
	il	$17, 0xb4
	rotqbyi	$14, $19, 8
	ai	$9, $14, 0xd4
	lqd	$7, 0($9)
	rotqby	$13, $7, $9
.L_sP_retry:
	hbrr	.L_sP_h1_brz, .L_sP_after_waiter
	wrch	$ch16, $10
	wrch	$ch17, $11
	wrch	$ch18, $12
	wrch	$ch19, $10
	wrch	$ch20, $15
	wrch	$ch21, $18
	rdch	$2, $ch27
	dsync
	lqa	$22, 0x80
	cwd	$14, 0($1)
	cgti	$23, $22, 0
	nor	$20, $23, $23
	a	$6, $22, $23
	sfi	$9, $20, 0
.L_sP_h1_brz:
	brz	$9, .L_sP_after_waiter
	lqa	$28, 0x90
	rotqbyi	$29, $28, 13
	ahi	$27, $29, 1
	clgtbi	$26, $29, 14
	andi	$25, $27, 0xff
	xsbh	$24, $26
	shufb	$7, $27, $28, $16
	cbx	$3, $1, $25
	brhnz	$24, .L_sP_err_901
	lqa	$36, 0xa0
	lqa	$38, 0x1d0
	shufb	$39, $13, $7, $3
	rotqbyi	$37, $36, 13
	rotqbyi	$31, $38, 12
	stqa	$39, 0x90
	ahi	$35, $37, 1
	andi	$34, $35, 0xff
	shufb	$32, $35, $36, $16
	cbx	$33, $1, $34
	shufb	$30, $31, $32, $33
	stqa	$30, 0xa0
.L_sP_after_waiter:
	lqa	$42, 0x80
	shufb	$41, $6, $42, $14
	stqa	$41, 0x80
	dsync
	wrch	$ch16, $10
	wrch	$ch17, $11
	wrch	$ch18, $12
	wrch	$ch19, $10
	wrch	$ch20, $15
	wrch	$ch21, $17
	rdch	$40, $ch27
	brnz	$40, .L_sP_retry
	brz	$9, .L_sP_epilog
	brsl	$0, _cellSpursTaskCanCallBlockWait
	ori	$4, $3, 0
	brnz	$3, .L_sP_after_block
	il	$3, 2
	fsmbi	$48, 0x101
	lqa	$47, 0x2fb0
	lqa	$43, 0x2fd0
	cdd	$80, 8($1)
	andhi	$46, $48, 0x101
	or	$44, $81, $46
	rotqbyi	$45, $47, 8
	shufb	$18, $44, $43, $80
	ai	$17, $45, 0xc4
	stqa	$18, 0x2fd0
	lqd	$16, 0($17)
	rotqby	$15, $16, $17
	bisl	$0, $15
	il	$12, 0
	lqa	$10, 0x2fd0
	ori	$4, $3, 0
	shufb	$8, $12, $10, $80
	stqa	$8, 0x2fd0
.L_sP_after_block:
	ori	$8, $4, 0
.L_sP_epilog:
	ori	$3, $8, 0
	lqd	$0, 96($1)
	ai	$1, $1, 80
	lqd	$80, -16($1)
	lqd	$81, -32($1)
	lqd	$126, -48($1)
	bi	$0
	lnop
	.size	cellSpursSemaphoreP, . - cellSpursSemaphoreP
