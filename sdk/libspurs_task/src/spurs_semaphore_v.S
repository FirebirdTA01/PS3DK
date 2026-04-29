/* cellSpursSemaphoreV - "V" / signal a CellSpursSemaphore.
 *
 * Args:
 *   r3 = ptr to CellSpursSemaphore (16-byte aligned)
 *
 * Behavior:
 *   - Validate ptr non-null + 16-byte aligned (0x80410911/0x80410910).
 *   - GETLLAR/PUTLLC retry loop on the semaphore cache line at LS 0x80
 *     (128 bytes from EA in r3):
 *       * if count < 0 (a waiter is queued): pop oldest waiter taskId
 *         from waiter list at LS 0x90, mark "waiter to wake" for
 *         post-commit; commit waiter list back at LS 0xa0.
 *       * else: count++.
 *   - After PUTLLC succeeds:
 *       * if a waiter was popped: compute its workload flag EA via
 *         _cellSpursGetWorkloadFlag, then call cellSpursSendSignal to
 *         wake it.
 *       * else: return success.
 *
 * Hand-translated to byte-match libspurs.a/semaphoreV.o.
 * Two REL16 relocs (_cellSpursGetWorkloadFlag, cellSpursSendSignal).
 * Size: 432 bytes.
 */
	.text
	.globl	cellSpursSemaphoreV
	.type	cellSpursSemaphoreV, @function
	.align	3
cellSpursSemaphoreV:
	il	$7, 0
	hbrr	.L_sV_h0_brnz, .L_sV_epilog
	ilhu	$4, 0x8041
	stqd	$0, 16($1)
	ceq	$6, $3, $7
	stqd	$80, -16($1)
	iohl	$4, 0x911
	stqd	$126, -32($1)
	gb	$5, $6
	stqd	$1, -64($1)
	ai	$1, $1, -64
	cgti	$2, $5, 11
.L_sV_h0_brnz:
	brnz	$2, .L_sV_epilog
	shlqbyi	$5, $3, 4
	ilhu	$4, 0x8041
	iohl	$4, 0x910
	andi	$8, $5, 0x7f
	brnz	$8, .L_sV_epilog
	ori	$10, $5, 0
	cbd	$13, 0($1)
	il	$80, 0x80
	fsmbi	$12, 0
	il	$8, 0x20
	shlqbyi	$9, $3, 0
	il	$7, 0x80
	il	$15, 0xd0
	il	$14, 0xb4
.L_sV_retry:
	wrch	$ch16, $7
	wrch	$ch17, $9
	wrch	$ch18, $10
	wrch	$ch19, $7
	wrch	$ch20, $12
	wrch	$ch21, $15
	rdch	$2, $ch27
	dsync
	lqa	$4, 0x80
	il	$11, 0
	cwd	$6, 0($1)
	cgti	$3, $4, -1
	brz	$3, .L_sV_after_waiter
	lqa	$23, 0x90
	rotqbyi	$21, $23, 13
	shlqbyi	$17, $23, 1
	rotqbyi	$5, $23, 14
	andi	$20, $21, 0xff
	ceqbi	$19, $21, 0
	ai	$18, $20, -1
	xsbh	$16, $19
	shufb	$4, $18, $17, $13
	brhnz	$16, .L_sV_after_waiter
	il	$27, 0
	lqa	$36, 0xa0
	andi	$80, $5, 0xff
	hbrr	.L_sV_h1_br, .L_sV_after_increment
	il	$11, 1
	stqa	$4, 0x90
	rotqbyi	$35, $36, 13
	shlqbyi	$26, $36, 1
	rotqbyi	$31, $36, 14
	ceqbi	$34, $35, 0
	andi	$33, $35, 0xff
	xsbh	$32, $34
	ai	$29, $33, -1
	ceqhi	$30, $32, 0
	andi	$8, $31, 0xff
	fsm	$28, $30
	selb	$25, $27, $29, $28
	shufb	$24, $25, $26, $13
	stqa	$24, 0xa0
.L_sV_h1_br:
	br	.L_sV_after_increment
.L_sV_after_waiter:
	lqa	$39, 0x80
	ai	$38, $39, 1
	shufb	$37, $38, $39, $6
	stqa	$37, 0x80
.L_sV_after_increment:
	dsync
	wrch	$ch16, $7
	wrch	$ch17, $9
	wrch	$ch18, $10
	wrch	$ch19, $7
	wrch	$ch20, $12
	wrch	$ch21, $14
	rdch	$41, $ch27
	brnz	$41, .L_sV_retry
	brz	$11, .L_sV_success_no_wake
	il	$13, 0
	lqa	$14, 0xb0
	rotqbyi	$3, $14, 8
	ceq	$12, $3, $13
	gb	$11, $12
	cgti	$10, $11, 11
	brz	$10, .L_sV_send_signal
	ilhu	$4, 0x8041
	clgti	$15, $8, 0x1f
	iohl	$4, 0x902
	brnz	$15, .L_sV_epilog
	ori	$3, $8, 0
	brsl	$0, _cellSpursGetWorkloadFlag
.L_sV_send_signal:
	ori	$4, $80, 0
	brsl	$0, cellSpursSendSignal
	ori	$4, $3, 0
	brnz	$3, .L_sV_epilog
.L_sV_success_no_wake:
	il	$4, 0
.L_sV_epilog:
	ori	$3, $4, 0
	lqd	$0, 80($1)
	ai	$1, $1, 64
	lqd	$80, -16($1)
	lqd	$126, -32($1)
	bi	$0
	.size	cellSpursSemaphoreV, . - cellSpursSemaphoreV
