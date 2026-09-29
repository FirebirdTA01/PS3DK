/* spurs_task_thread.c - the SPURS task as a sync2 caller thread type.
 *
 * Thread id:   taskset EA << 32 | task id.
 * Receiver id: task id << 32 | taskset EA; a waiting task blocks in
 *              cellSpursWaitSignal and is woken with cellSpursSendSignal.
 */
#include <cell/sync2/thread.h>
#include <cell/spurs/task.h>
#include "sync2_internal.h"

#define SPURS_TASK_NO_CONTEXT 0x8041090Fu

extern int _cellSpursTaskCanCallBlockWait(void);

static CellSync2ThreadId task_self(uint64_t arg)
{
	(void)arg;
	return ((uint64_t)(uint32_t)cellSpursGetTasksetAddress() << 32) | cellSpursGetTaskId();
}

static int task_wait(CellSync2SignalReceiverId receiver, CellSync2ObjectTypeId type, uint64_t ea, uint64_t arg)
{
	(void)receiver; (void)type; (void)ea; (void)arg;
	if (cellSpursWaitSignal() != 0)
		s2_halt();
	return 0;
}

static int task_allocate(CellSync2SignalReceiverId *receiver, CellSync2ObjectTypeId type, uint64_t ea, uint64_t arg)
{
	(void)type; (void)ea; (void)arg;
	int rc = _cellSpursTaskCanCallBlockWait();
	if ((unsigned int)rc == SPURS_TASK_NO_CONTEXT)
		return S2_NO_SPU_CONTEXT_STORAGE;
	if (rc)
		return S2_PERM;
	*receiver = ((uint64_t)cellSpursGetTaskId() << 32) | (uint32_t)cellSpursGetTasksetAddress();
	return 0;
}

static int task_free(CellSync2SignalReceiverId receiver, uint64_t arg)
{
	(void)receiver; (void)arg;
	return 0;
}

CellSync2CallerThreadType gCellSync2CallerThreadTypeSpursTask = {
	CELL_SYNC2_THREAD_TYPE_SPURS_TASK,
	task_self,
	task_wait,
	task_allocate,
	task_free,
	1000,
	0,
};
