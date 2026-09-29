/* gCellSync2NotifierSpursTask - wakes a waiting SPURS task. */
#include <cell/sync2/thread.h>
#include <cell/spurs/task.h>
#include "sync2_internal.h"

static int task_send(CellSync2SignalReceiverId receiver, uint64_t arg)
{
	(void)arg;
	if (cellSpursSendSignal((uint32_t)receiver, (receiver >> 32) & 0xff) != 0)
		s2_halt();
	return 0;
}

CellSync2Notifier gCellSync2NotifierSpursTask = {
	CELL_SYNC2_THREAD_TYPE_SPURS_TASK,
	task_send,
	0,
};
