/* libspurs_task SPU task entry-point crt.
 *
 * The Spurs SPRX branches to the task ELF's e_entry with:
 *   r1 = task stack pointer (SP set up by SPRX)
 *   r3 = argTask   (16-byte qword passed by PPU via createTask*)
 *   r4 = argTaskset (uint64_t in preferred slot - from TasksetAttribute)
 *
 * Our job: hand those through to user code (cellSpursMain), then fall
 * into cellSpursExit which dispatches back to SPRX.  Keep the crt
 * minimal - SPRX owns stack + register setup, user's cellSpursMain
 * does its own prologue.  Second brsl is fine because cellSpursExit
 * is noreturn, so the clobbered LR is never read.
 */
	.text
	.section .text.__spurs_task_crt, "ax", @progbits
	.globl	__spurs_task_start
	.type	__spurs_task_start, @function
	.align	3
__spurs_task_start:
	/* Run on a task-owned stack at the top of LS.  The policy module
	 * enters with SP inside its own area below 0x3000, which a task's
	 * LS pattern cannot cover, so a blocking service (wait-signal,
	 * semaphore P, event-flag wait) would fail its context save.
	 * The initial frame is 32 bytes: cellSpursMain saves its link
	 * register at 16($sp), which must stay inside LS (0x3fff0) rather
	 * than wrap to LS 0 in the kernel's area. */
	ila	$1, 0x3ffe0
	il	$2, 0
	stqd	$2, 0($1)		/* null back chain */
	/* Static constructors, last to first as GCC orders .ctors.  The
	 * task arguments wait in callee-saved registers.  The bounds come
	 * from spurs_task.ld; a script without them (weak, 0) runs none. */
	ori	$82, $3, 0
	ori	$83, $4, 0
	ila	$80, __spurs_task_ctors_end
	ila	$81, __spurs_task_ctors_start
1:	ceq	$2, $80, $81
	brnz	$2, 2f
	ai	$80, $80, -4
	lqd	$5, 0($80)
	rotqby	$5, $5, $80		/* the entry's word to the preferred slot */
	bisl	$0, $5
	br	1b
2:	ori	$3, $82, 0
	ori	$4, $83, 0
	brsl	$0, cellSpursMain
	brsl	$0, cellSpursExit
	.size	__spurs_task_start, . - __spurs_task_start

	.weak	__spurs_task_ctors_start
	.weak	__spurs_task_ctors_end

	/* C++ registers static destructors against this module handle
	 * (__cxa_atexit); a task is one static image, so it is 0, as
	 * crtbegin defines it for an executable. */
	.section .data.__dso_handle, "aw", @progbits
	.align	2
	.globl	__dso_handle
	.hidden	__dso_handle
	.type	__dso_handle, @object
	.size	__dso_handle, 4
__dso_handle:
	.long	0
