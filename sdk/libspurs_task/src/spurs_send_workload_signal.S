/* _cellSpursSendWorkloadSignal - signal the workload-signal mailbox
 * via an atomic GETLLAR/PUTLLC sequence on the workload signal cache
 * line at LS 0x170 and the EA derived from SpursAddress at LS 0x1c0.
 *
 * Argument:
 *   r3 = signal-bit index (0..31)
 *
 * Pattern:
 *   - GETLLAR 128 bytes at LSA 0x100 from EA = SpursAddress
 *   - rotate 1<<31 right by r3 to form a single-bit mask
 *   - merge the mask into the signal word in the cached line
 *   - PUTLLC; retry on collision
 *
 * Hand-translated to byte-match libspurs.a/sendWorkloadSignal.o.
 * No external relocations.  Size: 168 bytes.
 */
	.text
	.globl	_cellSpursSendWorkloadSignal
	.type	_cellSpursSendWorkloadSignal, @function
	.align	3
_cellSpursSendWorkloadSignal:
	ila	$8, 0x8000
	lqa	$4, 0x1c0
	sfi	$6, $3, 0
	fsmbi	$10, 0
	ilhu	$2, 0x8000
	sfi	$5, $3, 0
	rotma	$9, $8, $6
	il	$12, 0x100
	rotm	$8, $2, $5
	shlqbyi	$7, $4, 4
	il	$11, 0x80
	shlqbyi	$6, $4, 0
	il	$14, 0xd0
	il	$13, 0xb4
.L_sws_retry:
	wrch	$ch16, $12
	wrch	$ch17, $6
	wrch	$ch18, $7
	wrch	$ch19, $11
	wrch	$ch20, $10
	wrch	$ch21, $14
	rdch	$2, $ch27
	dsync
	lqa	$24, 0x170
	chd	$22, 0($1)
	chd	$18, 8($1)
	rotqbyi	$23, $24, 14
	or	$21, $23, $9
	shufb	$17, $21, $24, $22
	rotqbyi	$19, $17, 6
	or	$16, $19, $8
	shufb	$15, $16, $17, $18
	stqa	$15, 0x170
	dsync
	wrch	$ch16, $12
	wrch	$ch17, $6
	wrch	$ch18, $7
	wrch	$ch19, $11
	wrch	$ch20, $10
	wrch	$ch21, $13
	rdch	$3, $ch27
	brnz	$3, .L_sws_retry
	bi	$0
	.size	_cellSpursSendWorkloadSignal, . - _cellSpursSendWorkloadSignal
