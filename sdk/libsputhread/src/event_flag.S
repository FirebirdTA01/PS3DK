/* libsputhread SPU lv2-syscall wrappers — sys_event_flag_set_bit + impatient.
 *
 * SPU↔kernel lv2 syscall protocol via SPU mailbox channels:
 *
 *   ch28 (SPU_WrOutMbox)      - syscall arg 1 (here: sys_event_flag_t ef)
 *   ch29 (SPU_RdInMbox)       - syscall return slot
 *   ch30 (SPU_WrOutIntrMbox)  - tagged opcode; writing this wakes the
 *                                kernel to service the syscall
 *
 * For sys_event_flag_set_bit the protocol is:
 *
 *   $5 = 0x80010002          ("EBUSY-ish" return code on early-out paths)
 *   bitn must be in 0..63 or we early-return $5
 *   if there is already a pending value queued in $ch29 we early-return $5
 *   otherwise:
 *     wrch $ch28, ef
 *     wrch $ch30, bitn | 0x80000000   (set the BLOCKING flag)
 *     rdch $5, $ch29                  (kernel writes result here)
 *     return $5
 *
 * Note: the reference shape branches to the early-return path (which
 * just sets $3 = $5 and returns) from BOTH "pending in queue" and
 * "after-rdch" paths.  The first uses $5 = 0x80010002, the second
 * uses $5 = whatever the kernel wrote.  Same exit code in both cases.
 */
	.text

/* int sys_event_flag_set_bit(sys_event_flag_t ef, uint8_t bitn)
 *   r3 = ef
 *   r4 = bitn
 * Returns CELL_OK in r3 on success, error code on early-out / kernel
 * rejection. */
	.globl	sys_event_flag_set_bit
	.type	sys_event_flag_set_bit, @function
	.align	3
sys_event_flag_set_bit:
	clgtbi	$6, $4, 63           /* bit > 63 ? */
	ilhu	$5, 0x8001
	xsbh	$2, $6
	ori	$5, $5, 2            /* $5 = 0x80010002 (early-exit return code) */
	brhz	$2, .Lefsb_loop      /* valid bit: enter the syscall body */
.Lefsb_ret:
	ori	$3, $5, 0            /* $3 = $5; return */
	nop	$0
	bi	$0
.Lefsb_loop:
	ori	$6, $3, 0            /* save ef in $6 */
	rchcnt	$7, $ch29            /* depth of result mailbox */
	andi	$3, $4, 0xff         /* bitn (low byte) */
	hbrr	.Lefsb_post, .Lefsb_ret  /* hint: post-syscall branch goes to ret */
	ilhu	$8, 0x8001
	ilhu	$4, 0x8000           /* BLOCKING flag */
	ori	$5, $8, 10           /* unused-but-matches-reference scratch */
	lnop
	or	$3, $3, $4           /* arg = bitn | 0x80000000 */
	brnz	$7, .Lefsb_ret       /* result already pending: bail with $5 */
	wrch	$ch28, $6            /* arg1: ef */
	wrch	$ch30, $3            /* arg2: bitn|BLOCKING; this fires the syscall */
.Lefsb_post:
	rdch	$5, $ch29            /* result -> $5 */
	br	.Lefsb_ret           /* return $5 */
	.size	sys_event_flag_set_bit, . - sys_event_flag_set_bit


/* int sys_event_flag_set_bit_impatient(sys_event_flag_t ef, uint8_t bitn)
 *   r3 = ef
 *   r4 = bitn
 * Non-blocking: returns immediately with CELL_OK on success, or an
 * error code if the receiver FIFO is full / parameters are invalid. */
	.globl	sys_event_flag_set_bit_impatient
	.type	sys_event_flag_set_bit_impatient, @function
	.align	3
sys_event_flag_set_bit_impatient:
	clgtbi	$7, $4, 63           /* bit > 63 ? */
	hbr	1f, $0
	ilhu	$2, 0xc000           /* IMPATIENT marker */
	andi	$4, $4, 0xff
	ilhu	$6, 0x8001
	xsbh	$5, $7
	or	$4, $4, $2           /* arg = bitn | 0xc0000000 */
	shlqbyi	$2, $3, 0            /* $2 = ef */
	ori	$6, $6, 2            /* $6 = 0x80010002 (return code on invalid) */
	brhnz	$5, 2f               /* invalid: skip syscall, return $6 */
	il	$6, 0                /* CELL_OK */
	wrch	$ch28, $2            /* arg1: ef */
	wrch	$ch30, $4            /* arg2: bitn|IMPATIENT */
2:	ori	$3, $6, 0
1:	bi	$0
	.size	sys_event_flag_set_bit_impatient, . - sys_event_flag_set_bit_impatient
