/* libsputhread SPU thread / group exit traps.
 *
 * Both routines synchronise with the kernel via SPU event channels
 * (ch22 = WrEventMask, ch23 = WrEventAck, ch24 = RdEventStat) and
 * then deliver the exit code through the regular outbound mailbox
 * (ch28) before issuing a `stop` instruction.  The `stop` trap code
 * tells the kernel which exit operation it is:
 *
 *   stop 0x102  -> sys_spu_thread_exit       (this thread only)
 *   stop 0x101  -> sys_spu_thread_group_exit (whole thread group)
 *
 * Both functions are byte-identical apart from that one constant.
 * `stop` is a no-return instruction; the trailing `bi $0` exists for
 * symmetry / unwind metadata only.
 */
	.text

/* void sys_spu_thread_exit(int exit_code) __attribute__((noreturn))
 *   r3 = exit_code */
	.globl	sys_spu_thread_exit
	.type	sys_spu_thread_exit, @function
	.align	3
sys_spu_thread_exit:
	il	$2, 0
	wrch	$ch23, $2            /* ack-state = idle */
1:	rchcnt	$5, $ch24            /* wait for kernel to populate ch24 */
	ceqi	$4, $5, 1
	nop	$127
	brz	$4, 1b
	ori	$6, $3, 0            /* save exit_code in $6 */
	hbr	2f, $0
	il	$7, -1
	rdch	$3, $ch24            /* drain ch24 */
	il	$3, 2                /* exit mode = 2 */
	wrch	$ch22, $7            /* mask all events */
	wrch	$ch23, $3            /* arm exit */
	rdch	$2, $ch24            /* wait for kernel ack */
	wrch	$ch28, $6            /* deliver exit_code */
	stop	0x102                /* trap: thread exit */
2:	bi	$0
	.size	sys_spu_thread_exit, . - sys_spu_thread_exit


/* void sys_spu_thread_group_exit(int exit_code) __attribute__((noreturn))
 *   r3 = exit_code (group-wide) */
	.globl	sys_spu_thread_group_exit
	.type	sys_spu_thread_group_exit, @function
	.align	3
sys_spu_thread_group_exit:
	il	$2, 0
	wrch	$ch23, $2
1:	rchcnt	$5, $ch24
	ceqi	$4, $5, 1
	nop	$127
	brz	$4, 1b
	ori	$6, $3, 0
	hbr	2f, $0
	il	$7, -1
	rdch	$3, $ch24
	il	$3, 2
	wrch	$ch22, $7
	wrch	$ch23, $3
	rdch	$2, $ch24
	wrch	$ch28, $6
	stop	0x101                /* trap: thread-group exit */
2:	bi	$0
	.size	sys_spu_thread_group_exit, . - sys_spu_thread_group_exit
