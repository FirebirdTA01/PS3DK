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
	brsl	$0, cellSpursMain
	brsl	$0, cellSpursExit
	.size	__spurs_task_start, . - __spurs_task_start
