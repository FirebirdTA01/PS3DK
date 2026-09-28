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
	brsl	$0, cellSpursMain
	brsl	$0, cellSpursExit
	.size	__spurs_task_start, . - __spurs_task_start
