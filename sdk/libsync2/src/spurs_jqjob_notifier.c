/* gCellSync2NotifierSpursJobQueueJob - wakes a job suspended in a job queue:
 * receiver id = the suspended job's EA (libspurs_jq). */
#include <cell/sync2/thread.h>
#include "sync2_internal.h"

extern int cellSpursJobQueueSendSignal(uint64_t eaJob);

static int jqjob_send(CellSync2SignalReceiverId receiver, uint64_t arg)
{
	(void)arg;
	if (cellSpursJobQueueSendSignal(receiver) != 0)
		s2_halt();
	return 0;
}

CellSync2Notifier gCellSync2NotifierSpursJobQueueJob = {
	CELL_SYNC2_THREAD_TYPE_SPURS_JOBQUEUE_JOB,
	jqjob_send,
	0,
};
