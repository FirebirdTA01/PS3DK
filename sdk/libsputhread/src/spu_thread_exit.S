/* libsputhread SPU thread / group exit + yield traps.
 *
 * Exit routines drain outstanding MFC tag groups via MFC tag-status
 * channels:
 *   ch22 = MFC_WrTagMask   (select tag groups to monitor)
 *   ch23 = MFC_WrTagUpdate (request tag status update: 0=immediate, 2=all)
 *   ch24 = MFC_RdTagStat   (read tag status mask)
 * (SPU event channels are ch0/ch1/ch2, distinct from MFC tag channels).
 *
 * The tag-status sequence requests immediate tag status (WrTagUpdate 0),
 * waits for and reads that status from ch24, selects all 32 tag groups
 * (WrTagMask -1), and requests completion of all selected tags
 * (WrTagUpdate 2). Reading ch24 then blocks until all tag groups have
 * completed.
 *
 * After the MFC tag groups complete, the routine writes the exit code to
 * the outbound mailbox (ch28 = SPU_WrOutMbox) and issues a `stop` trap.
 * The `stop` instruction operand specifies the operation:
 *
 *   stop 0x100  -> sys_spu_thread_group_yield (cooperative yield)
 *   stop 0x101  -> sys_spu_thread_group_exit  (whole thread group)
 *   stop 0x102  -> sys_spu_thread_exit        (this thread only)
 *
 * The two exit helpers and group_yield each have a bi $0 after their stop;
 * switch_system_module instead has rdch after stop. For exit routines,
 * non-return behavior depends on LV2 kernel trap handling. For group_yield,
 * execution resumes at the instruction following `stop`.
 */
	.text

/* void sys_spu_thread_exit(int exit_code) __attribute__((noreturn))
 *   r3 = exit_code */
	.globl	sys_spu_thread_exit
	.type	sys_spu_thread_exit, @function
	.align	3
sys_spu_thread_exit:
	il	$2, 0
	wrch	$ch23, $2            /* ch23 = MFC_WrTagUpdate: request immediate status (0) */
1:	rchcnt	$5, $ch24            /* wait until ch24 (MFC_RdTagStat) count is readable */
	ceqi	$4, $5, 1
	nop	$127
	brz	$4, 1b
	ori	$6, $3, 0            /* save exit_code in $6 */
	hbr	2f, $0
	il	$7, -1
	rdch	$3, $ch24            /* read initial tag status from ch24 */
	il	$3, 2                /* tag update condition = MFC_TAG_UPDATE_ALL (2) */
	wrch	$ch22, $7            /* ch22 = MFC_WrTagMask: select all 32 tag groups (-1) */
	wrch	$ch23, $3            /* ch23 = MFC_WrTagUpdate: request completion of all tags */
	rdch	$2, $ch24            /* wait for and read tag status on ch24 */
	wrch	$ch28, $6            /* deliver exit_code via ch28 (SPU_WrOutMbox) */
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
	wrch	$ch23, $2            /* ch23 = MFC_WrTagUpdate: request immediate status (0) */
1:	rchcnt	$5, $ch24            /* wait until ch24 (MFC_RdTagStat) count is readable */
	ceqi	$4, $5, 1
	nop	$127
	brz	$4, 1b
	ori	$6, $3, 0            /* save exit_code in $6 */
	hbr	2f, $0
	il	$7, -1
	rdch	$3, $ch24            /* read initial tag status from ch24 */
	il	$3, 2                /* tag update condition = MFC_TAG_UPDATE_ALL (2) */
	wrch	$ch22, $7            /* ch22 = MFC_WrTagMask: select all 32 tag groups (-1) */
	wrch	$ch23, $3            /* ch23 = MFC_WrTagUpdate: request completion of all tags */
	rdch	$2, $ch24            /* wait for and read tag status on ch24 */
	wrch	$ch28, $6            /* deliver exit_code via ch28 (SPU_WrOutMbox) */
	stop	0x101                /* trap: thread-group exit */
2:	bi	$0
	.size	sys_spu_thread_group_exit, . - sys_spu_thread_group_exit


/* void sys_spu_thread_group_yield(void)
 * Cooperative yield: issues stop 0x100 trap. Execution resumes at the
 * instruction following `stop`. */
	.globl	sys_spu_thread_group_yield
	.type	sys_spu_thread_group_yield, @function
	.align	3
sys_spu_thread_group_yield:
	stop	0x100                /* trap: yield */
	bi	$0
	.size	sys_spu_thread_group_yield, . - sys_spu_thread_group_yield


/* int sys_spu_thread_switch_system_module(uint32_t newAddress)
 *   r3 = address of the new system module image to swap into LS
 * Requests the kernel to replace the running SPU image with a new one.
 * Executes the MFC tag-status sequence to wait for outstanding MFC tag
 * groups to complete before issuing `stop 0x120` trap. After each trap,
 * reads the result from ch29; retries on EAGAIN (0x8001000a), otherwise
 * returns that result. The status is read on every attempt. */
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
	wrch	$ch23, $5           /* ch23 = MFC_WrTagUpdate: request immediate status (0) */
2:	rchcnt	$7, $ch24
	ceqi	$6, $7, 1
	.long	0x4020007f          /* nop $127 */
	brz	$6, 2b              /* wait until ch24 (MFC_RdTagStat) count is readable */
	il	$9, -1
	hbrr	3f, .Lssm_retry     /* hint: brnz at 3f goes back to retry */
	ori	$6, $3, 0           /* $6 = new module address */
	rdch	$2, $ch24           /* read initial tag status from ch24 */
	il	$3, 2               /* tag update condition = MFC_TAG_UPDATE_ALL (2) */
	wrch	$ch22, $9           /* ch22 = MFC_WrTagMask: select all 32 tag groups (-1) */
	ilhu	$8, 0x8001
	wrch	$ch23, $3           /* ch23 = MFC_WrTagUpdate: request completion of all tags */
	ori	$5, $8, 10          /* $5 = 0x8001000a (EAGAIN compare value) */
	rdch	$2, $ch24           /* wait for and read tag status on ch24 */
.Lssm_retry:
	wrch	$ch28, $6           /* arg = new module address */
	stop	0x120               /* trap: switch_system_module */
	rdch	$4, $ch29           /* result code */
	ceq	$10, $4, $5         /* kernel asks retry? */
3:	brnz	$10, .Lssm_retry    /* loop on EAGAIN */
	br	.Lssm_ret           /* otherwise return $4 */
	.size	sys_spu_thread_switch_system_module, . - sys_spu_thread_switch_system_module


/* --- PSL1GHT-compat legacy symbol aliases (deprecated) --------------
 * Upstream PSL1GHT exports these SPU routines without the sys_ prefix.
 * Provide weak aliases of the modern symbols so unmodified PSL1GHT SPU
 * code links; the deprecated extern decls in <sys/spu_thread.h> emit
 * the per-call warning pointing at the sys_ name.  Weak so they never
 * clash with any other provider.  Define __PS3DK_NO_PSL1GHT_COMPAT__
 * to drop them. */
#ifndef __PS3DK_NO_PSL1GHT_COMPAT__
	.weak	spu_thread_exit
	.set	spu_thread_exit, sys_spu_thread_exit
	.type	spu_thread_exit, @function
	.weak	spu_thread_group_exit
	.set	spu_thread_group_exit, sys_spu_thread_group_exit
	.type	spu_thread_group_exit, @function
	.weak	spu_thread_group_yield
	.set	spu_thread_group_yield, sys_spu_thread_group_yield
	.type	spu_thread_group_yield, @function
#endif
