/* _cellSpursTaskCanCallBlockWait - permission/precondition gate that
 * sync primitives consult before invoking a BLOCK-shaped service.
 *
 * Computes the bitfield of "blocking is OK" reasons by combining:
 *   - SP-relative alignment / frame-size checks (rotmi/rotqmbybi etc.
 *     stand in for SP misalignment guards)
 *   - module signature compare against constant 0x544b ("TK"=task)
 *   - per-task ELF bytes loaded from dispatch_base+0x98/0xa0
 *
 * Returns one of three error words (ilhu/iohl pairs 0x80410909 / 0x9 0f /
 * 0x90b ...) selected by the chained selb chain, or 0 on success.
 *
 * Hand-translated to byte-match libspurs.a/canCallBlockWait.o.
 * No external relocations.  Size: 152 bytes.
 */
	.text
	.globl	_cellSpursTaskCanCallBlockWait
	.type	_cellSpursTaskCanCallBlockWait, @function
	.align	3
_cellSpursTaskCanCallBlockWait:
	rotmi	$29, $1, -11
	lqa	$33, 0x2fb0
	il	$30, -1
	lqa	$28, 0x1e0
	il	$17, 0
	hbr	.L_ccbw_bi, $0
	il	$26, 0x544b
	fsmbi	$4, 0
	sfi	$31, $29, 7
	sfi	$25, $29, 0
	ilhu	$7, 0x8041
	rotqbyi	$32, $33, 8
	ilhu	$5, 0x8041
	rotqmbybi $24, $30, $31
	iohl	$7, 0x90f
	rotqbyi	$27, $28, 6
	iohl	$5, 0x909
	ai	$19, $32, 152
	lqd	$21, 160($32)
	lqd	$18, 0($19)
	ceqh	$23, $27, $26
	rotqmbi	$20, $24, $25
	ceqhi	$22, $23, 0
	andc	$13, $20, $21
	rotqby	$16, $18, $19
	clgti	$11, $13, 0
	fsm	$6, $22
	gb	$9, $11
	ceq	$15, $16, $17
	gb	$14, $15
	cgti	$12, $14, 11
	sfi	$10, $12, 0
	or	$8, $9, $10
	ceqi	$2, $8, 0
	selb	$3, $7, $4, $2
	selb	$3, $3, $5, $6
.L_ccbw_bi:
	bi	$0
	lnop
	.size	_cellSpursTaskCanCallBlockWait, . - _cellSpursTaskCanCallBlockWait
