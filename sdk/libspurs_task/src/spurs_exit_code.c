/* SPU side of the task exit-code container (CellSpursTaskExitCode).
 *
 * A container is one 128-byte line in main memory:
 *   0x00 16    event record, when a PPU waiter is told through the SPURS
 *              event multiplexer (see spurs_post_event.c)
 *   0x20 u32   state
 *                bit 0   a task is attached
 *                bit 1   ready to wait: the creating call has finished
 *                bit 2   the task exited; the exit code is valid
 *                bit 3   ... but no exit code will be delivered
 *                bit 16  a PPU thread waits (posted through +0x40)
 *                bit 17  an SPU task waits (signalled through +0x30)
 *   0x28 u32   exit code
 *   0x30 u64   waiting task's taskset EA, 0x38 u8 its task id
 *   0x39 u8    attached task id
 *   0x40 u64   SPURS EA the PPU waiter's event is posted to
 *   0x50 u64   attached task's taskset EA
 * Getting the code consumes it: the state returns to 0 and the container
 * can take another task.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>
#include "spurs_line.h"

#define WAIT_KIND_EXIT_CODE 6

#define ST_ATTACHED   0x00000001u
#define ST_READY      0x00000002u
#define ST_EXITED     0x00000004u
#define ST_NO_CODE    0x00000008u
#define ST_PPU_WAITER 0x00010000u
#define ST_SPU_WAITER 0x00020000u

extern unsigned int cellSpursGetTaskId(void);
extern uint64_t cellSpursGetTasksetAddress(void);
extern int cellSpursSendSignal(uint64_t eaTaskset, unsigned int idTask);
extern int cellSpursWaitSignal(void);
extern int _cellSpursTaskCanCallBlockWait(void);
extern int cellSpursPostEvent(uint64_t eaSpurs, uint64_t eaEvent, uint64_t data);

static uint32_t line[32] __attribute__((aligned(128)));

static int check_ea(uint64_t ea)
{
	if (!ea)
		return (int)TASK_NULL;
	if (ea & 0x7f)
		return (int)TASK_ALIGN;
	return 0;
}

int cellSpursTaskExitCodeInitialize(uint64_t ea)
{
	unsigned i;
	int rc = check_ea(ea);
	if (rc)
		return rc;
	for (i = 0; i < 32; ++i)
		line[i] = 0;
	spu_dsync();
	mfc_putlluc(line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	return 0;
}

int _cellSpursTaskExitCodeAttachTask(uint64_t ea, uint64_t eaTaskset, unsigned int idTask)
{
	if (!ea || !eaTaskset)
		return (int)TASK_NULL;
	if ((ea & 0x7f) || (eaTaskset & 0x7f))
		return (int)TASK_ALIGN;
	do {
		line_get_at(line, ea);
		if (LINE_U32(line, 0x20))
			return (int)TASK_BUSY;
		LINE_U32(line, 0x20) = ST_ATTACHED;
		LINE_U64(line, 0x50) = eaTaskset;
		LINE_U8(line, 0x39) = (uint8_t)idTask;
	} while (!line_put_at(line, ea));
	return 0;
}

/* The creating call has finished.  `noCode`: the task's taskset delivers
 * no exit codes, so unless the task already exited the code is marked
 * exited-without-a-code. */
int _cellSpursTaskExitCodeMakeReadyToWait(uint64_t ea, int noCode)
{
	uint32_t st;
	int rc = check_ea(ea);
	if (rc)
		return rc;
	do {
		line_get_at(line, ea);
		st = LINE_U32(line, 0x20);
		if (st & ~(ST_ATTACHED | ST_EXITED | ST_NO_CODE))
			return (int)TASK_STAT;
		if (noCode && !(st & ST_EXITED))
			st |= ST_EXITED | ST_NO_CODE;
		LINE_U32(line, 0x20) = st | ST_READY;
	} while (!line_put_at(line, ea));
	return 0;
}

/* The attached task exited with `code` (`noCode`: without one); wake
 * whoever waits. */
int _cellSpursTaskExitCodeSet(uint64_t ea, int code, int noCode)
{
	uint32_t st;
	int rc = check_ea(ea);
	if (rc)
		return rc;
	do {
		line_get_at(line, ea);
		st = LINE_U32(line, 0x20);
		if (!(st & ST_ATTACHED))
			return (int)TASK_STAT;
		LINE_U32(line, 0x20) = st | ST_EXITED | (noCode ? ST_NO_CODE : 0);
		LINE_U32(line, 0x28) = (uint32_t)code;
	} while (!line_put_at(line, ea));
	if ((st & ST_SPU_WAITER) && (rc = cellSpursSendSignal(LINE_U64(line, 0x30), LINE_U8(line, 0x38))) != 0)
		return rc;
	if ((st & ST_PPU_WAITER) && (rc = cellSpursPostEvent(LINE_U64(line, 0x40), ea, ea)) != 0)
		return rc;
	return 0;
}

static int deliver(uint32_t st, int *code)
{
	if (st & ST_NO_CODE)
		return (int)TASK_SHUTDOWN;
	*code = (int)LINE_U32(line, 0x28);
	return 0;
}

static int exit_code_get(uint64_t ea, int *code, int isBlocking)
{
	uint64_t taskset;
	unsigned id;
	uint32_t st;
	int rc;
	if (!ea || !code)
		return (int)TASK_NULL;
	if (ea & 0x7f)
		return (int)TASK_ALIGN;
	if (isBlocking && (rc = _cellSpursTaskCanCallBlockWait()) != 0)
		return rc;
	taskset = cellSpursGetTasksetAddress();
	id = cellSpursGetTaskId();
	for (;;) {
		line_get_at(line, ea);
		st = LINE_U32(line, 0x20);
		if (!(st & ST_READY))
			return (int)TASK_STAT;
		if (st & (ST_PPU_WAITER | ST_SPU_WAITER))
			return (int)TASK_BUSY;
		if (st & ST_EXITED) {
			LINE_U32(line, 0x20) = 0;           /* consume */
			if (line_put_at(line, ea))
				return deliver(st, code);
			continue;
		}
		if (!isBlocking)
			return (int)TASK_AGAIN;
		LINE_U32(line, 0x20) = st | ST_SPU_WAITER;
		LINE_U64(line, 0x30) = taskset;
		LINE_U8(line, 0x38) = (uint8_t)id;
		if (line_put_at(line, ea))
			break;
	}

	if ((rc = _cellSpursTaskCanCallBlockWait()) != 0)
		return rc;
	*(volatile uint64_t *)TASK_WAIT_OBJECT = ea | WAIT_KIND_EXIT_CODE;
	rc = cellSpursWaitSignal();
	*(volatile uint64_t *)TASK_WAIT_OBJECT = 0;
	if (rc)
		return rc;
	do {
		line_get_at(line, ea);
		st = LINE_U32(line, 0x20);
		LINE_U64(line, 0x30) = 0;
		LINE_U8(line, 0x38) = 0;
		LINE_U32(line, 0x20) = 0;
	} while (!line_put_at(line, ea));
	return deliver(st, code);
}

int cellSpursTaskExitCodeGet(uint64_t ea, int *code)    { return exit_code_get(ea, code, 1); }
int cellSpursTaskExitCodeTryGet(uint64_t ea, int *code) { return exit_code_get(ea, code, 0); }
