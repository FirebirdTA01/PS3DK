/* _cellSpursGetWorkloadFlag - returns the per-workload "flag" EA
 * computed from the SpursAddress + a workload-id derived offset.
 *
 * Reads SpursAddress (uint64_t) from LS 0x1c0 and adds a small
 * fixed offset (0x60) to obtain the EA of the workload-flag word in
 * main memory.  Used by SemaphoreV / SendSignal as the EA argument
 * for the kernel-side wakeup queue.
 *
 * Hand-translated to byte-match libspurs.a/getWorkloadFlag.o.
 * Single REL16 reloc against .rodata.cst16 shuffle mask.  Size: 48 B.
 */
	.text
	.globl	_cellSpursGetWorkloadFlag
	.type	_cellSpursGetWorkloadFlag, @function
	.align	3
_cellSpursGetWorkloadFlag:
	lqa	$5, 0x1c0
	fsmbi	$6, 0x101
	lqr	$4, .L_gwf_shuf
	andbi	$2, $6, 0x60
	cg	$3, $5, $2
	shufb	$3, $3, $3, $4
	addx	$3, $5, $2
	bi	$0
	.size	_cellSpursGetWorkloadFlag, . - _cellSpursGetWorkloadFlag

	.section	.rodata.cst16,"aM",@progbits,16
	.align	4
.L_gwf_shuf:
	.byte	0x04, 0x05, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80
	.byte	0x0c, 0x0d, 0x0e, 0x0f, 0x80, 0x80, 0x80, 0x80
