/* cellSpursSendSignal - signal a single bit on the SPURS-side
 * task-signal mailbox associated with a CellSpursTaskset.
 *
 * Args:
 *   r3 = ptr to CellSpursTaskset (16-byte aligned)
 *   r4 = task index (0..127)
 *
 * Behavior:
 *   - Validate ptr non-null + 16-byte aligned + task index < 128.
 *     On failure return 0x80410911 / 0x80410910 / 0x80410902.
 *   - Atomically (GETLLAR/PUTLLC at LS 0x80, 128 bytes from EA in r3)
 *     set the task's signal bit in the cached line.  Retry on PUTLLC
 *     collision.
 *   - If a waiter exists (signal flag transitions 0->1 with no prior
 *     pending bit), call _cellSpursSendWorkloadSignal to wake it.
 *
 * Hand-translated to byte-match libspurs.a/sendSignal.o.
 * One REL16 reloc against _cellSpursSendWorkloadSignal at 0x1ac.
 * Size: 464 bytes.
 */
	.text
	.globl	cellSpursSendSignal
	.type	cellSpursSendSignal, @function
	.align	3
cellSpursSendSignal:
	ori	$6, $3, 0
	hbrr	.L_ss_h0_br, .L_ss_epilog
	il	$8, 0
	stqd	$0, 16($1)
	ilhu	$3, 0x8041
	stqd	$126, -16($1)
	ceq	$7, $6, $8
	stqd	$1, -48($1)
	iohl	$3, 0x911
	gb	$5, $7
	ai	$1, $1, -48
	cgti	$2, $5, 11
	ori	$5, $4, 0
.L_ss_h0_br:
	brnz	$2, .L_ss_epilog
	ilhu	$3, 0x8041
	shlqbyi	$4, $6, 4
	iohl	$3, 0x910
	andi	$9, $4, 0x7f
	brnz	$9, .L_ss_epilog
	ilhu	$3, 0x8041
	clgti	$10, $5, 0x7f
	iohl	$3, 0x902
	brnz	$10, .L_ss_epilog
	fsmbi	$11, 0x8000
	sfi	$3, $5, 7
	lqa	$17, 0x1c0
	ori	$15, $4, 0
	fsmbi	$18, 0
	sfi	$4, $5, 0
	shlqbyi	$14, $6, 0
	andbi	$19, $11, -128
	il	$13, 0x80
	il	$20, 0xd0
	rotqmbybi $12, $19, $3
	ai	$11, $5, -31
	il	$19, 0xb4
	rotqmbi	$16, $12, $4
.L_ss_retry:
	wrch	$ch16, $13
	wrch	$ch17, $14
	wrch	$ch18, $15
	wrch	$ch19, $13
	wrch	$ch20, $18
	wrch	$ch21, $20
	rdch	$2, $ch27
	dsync
	lqa	$23, 0xe0
	ilhu	$10, 0x8041
	iohl	$10, 0x905
	ceq	$22, $23, $17
	gb	$21, $22
	cgti	$6, $21, 11
	brz	$6, .L_ss_err_905
	lqa	$26, 0xf0
	rotqbyi	$25, $26, 4
	clgti	$24, $25, 0x1f
	brnz	$24, .L_ss_err_905
	lqa	$43, 0xc0
	lqa	$42, 0xd0
	lqa	$53, 0x80
	lqa	$50, 0x90
	lqa	$51, 0xa0
	lqa	$45, 0xb0
	or	$12, $43, $16
	hbrr	.L_ss_h1_brnz, .L_ss_putllc
	or	$52, $43, $42
	and	$46, $53, $42
	or	$48, $53, $50
	or	$49, $51, $52
	and	$47, $50, $51
	rotqbybi $41, $45, $11
	or	$44, $48, $49
	or	$40, $46, $47
	andc	$39, $44, $45
	rotqbi	$36, $41, $11
	or	$37, $39, $40
	andc	$38, $42, $43
	orx	$33, $37
	andi	$34, $36, 1
	ceqi	$31, $33, 0
	rotqbybi $35, $38, $11
	xori	$32, $34, 1
	nor	$30, $31, $31
	andi	$29, $32, 0xff
	sfi	$28, $30, 0
	rotqbi	$4, $35, $11
	or	$27, $28, $29
.L_ss_h1_brnz:
	brnz	$27, .L_ss_putllc
	hbrr	.L_ss_h2_brz, .L_ss_send_wsig
	andi	$10, $4, 1
	stqa	$12, 0xc0
	dsync
.L_ss_putllc:
	wrch	$ch16, $13
	wrch	$ch17, $14
	wrch	$ch18, $15
	wrch	$ch19, $13
	wrch	$ch20, $18
	wrch	$ch21, $19
	rdch	$54, $ch27
.L_ss_h2_brz:
	brz	$54, .L_ss_send_wsig
	br	.L_ss_retry
.L_ss_err_905:
	ilhu	$10, 0x8041
	iohl	$10, 0x902
	br	.L_ss_set_r3_from_r10
.L_ss_send_wsig:
	cgti	$13, $10, 0
	brz	$13, .L_ss_set_r3_from_r10
	lqa	$15, 0xf0
	rotqbyi	$3, $15, 4
	brsl	$0, _cellSpursSendWorkloadSignal
	il	$3, 0
	br	.L_ss_epilog
.L_ss_set_r3_from_r10:
	ori	$3, $10, 0
.L_ss_epilog:
	lqd	$0, 64($1)
	ai	$1, $1, 48
	lqd	$126, -16($1)
	bi	$0
	lnop
	.size	cellSpursSendSignal, . - cellSpursSendSignal
