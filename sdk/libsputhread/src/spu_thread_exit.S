/* libsputhread SPU thread / group exit + yield traps.
 *
 * Exit routines synchronise with the kernel via SPU event channels
 * (ch22 = WrEventMask, ch23 = WrEventAck, ch24 = RdEventStat) and
 * then deliver the exit code through the regular outbound mailbox
 * (ch28) before issuing a `stop` instruction.  The `stop` trap code
 * tells the kernel which exit operation it is:
 *
 *   stop 0x100  -> sys_spu_thread_group_yield (cooperative yield)
 *   stop 0x101  -> sys_spu_thread_group_exit  (whole thread group)
 *   stop 0x102  -> sys_spu_thread_exit        (this thread only)
 *
 * The two exit functions are byte-identical apart from the `stop`
 * code; group_yield is a one-instruction barrier with no event-
 * channel handshake (the kernel handles re-entry from the yield
 * trap site directly).  `stop` is a no-return instruction; the
 * trailing `bi $0` exists for symmetry / unwind metadata only.
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


/* void sys_spu_thread_group_yield(void)
 * Cooperative yield: trap to the kernel, which may re-schedule the
 * group, then resume execution at the instruction after `stop`. */
	.globl	sys_spu_thread_group_yield
	.type	sys_spu_thread_group_yield, @function
	.align	3
sys_spu_thread_group_yield:
	stop	0x100                /* trap: yield */
	bi	$0
	.size	sys_spu_thread_group_yield, . - sys_spu_thread_group_yield


/* int sys_spu_thread_switch_system_module(uint32_t newAddress)
 *   r3 = address of the new system module image to swap into LS
 * Driver-level: requests the kernel to replace the running SPU
 * image with a new one.  Same channel handshake as exit (ch22/23/24)
 * plus a `stop 0x120` trap; on EAGAIN the kernel asks us to retry,
 * so the body loops back into the handshake until either CELL_OK
 * or a hard error returns.  Returns the kernel's status code. */
	.globl	sys_spu_thread_switch_system_module
	.type	sys_spu_thread_switch_system_module, @function
	.align	3
sys_spu_thread_switch_system_module:
	ilhu	$4, 0x8001
	rchcnt	$2, $ch29           /* mailbox already pending? */
	ori	$4, $4, 10          /* $4 = 0x8001000a (EAGAIN) */
	brz	$2, 1f
.Lssm_ret:
	ori	$3, $4, 0
	bi	$0
1:	il	$5, 0
	wrch	$ch23, $5           /* ack=0: reset event-state */
2:	rchcnt	$7, $ch24
	ceqi	$6, $7, 1
	.long	0x4020007f          /* nop $127 */
	brz	$6, 2b              /* spin until ch24 has one event */
	il	$9, -1
	hbrr	3f, .Lssm_retry     /* hint: brnz at 3f goes back to retry */
	ori	$6, $3, 0           /* $6 = new module address */
	rdch	$2, $ch24           /* drain ch24 */
	il	$3, 2
	wrch	$ch22, $9           /* mask all events */
	ilhu	$8, 0x8001
	wrch	$ch23, $3           /* ack=2: arm */
	ori	$5, $8, 10          /* $5 = 0x8001000a (EAGAIN compare value) */
	rdch	$2, $ch24           /* wait for kernel ack */
.Lssm_retry:
	wrch	$ch28, $6           /* arg = new module address */
	stop	0x120               /* trap: switch_system_module */
	rdch	$4, $ch29           /* result code */
	ceq	$10, $4, $5         /* kernel asks retry? */
3:	brnz	$10, .Lssm_retry    /* loop on EAGAIN */
	br	.Lssm_ret           /* otherwise return $4 */
	.size	sys_spu_thread_switch_system_module, . - sys_spu_thread_switch_system_module
