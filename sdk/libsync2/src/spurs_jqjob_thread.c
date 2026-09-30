/* gCellSync2CallerThreadTypeSpursJobQueueJob - a job queue job calling sync2.
 *
 * callbackArg = attribute << 32 | EA of the job's CellSpursJobQueueSuspendedJob
 * storage.  The receiver id is that EA; a waiting job suspends in
 * cellSpursJobQueueWaitSignal2 and is resumed by cellSpursJobQueueSendSignal.
 */
#include <cell/sync2/thread.h>
#include "sync2_internal.h"
#include "spurs_job_id.h"

extern int cellSpursJobQueueWaitSignal2(uint64_t eaSuspendedJob2, unsigned int attr);

static uint64_t s_id;

static CellSync2ThreadId jqjob_self(uint64_t arg)
{
	(void)arg;
	return s2_job_id(&s_id);
}

static int jqjob_wait(CellSync2SignalReceiverId receiver, CellSync2ObjectTypeId type, uint64_t ea, uint64_t arg)
{
	(void)receiver; (void)type; (void)ea;
	if (cellSpursJobQueueWaitSignal2((uint32_t)arg, (unsigned int)(arg >> 32)) != 0)
		s2_halt();
	return 0;
}

static int jqjob_allocate(CellSync2SignalReceiverId *receiver, CellSync2ObjectTypeId type, uint64_t ea,
                          uint64_t arg)
{
	(void)type; (void)ea;
	if ((uint32_t)arg == 0)
		return S2_NO_SPU_CONTEXT_STORAGE;
	*receiver = (uint32_t)arg;
	return 0;
}

static int jqjob_free(CellSync2SignalReceiverId receiver, uint64_t arg)
{
	(void)receiver; (void)arg;
	return 0;
}

CellSync2CallerThreadType gCellSync2CallerThreadTypeSpursJobQueueJob = {
	CELL_SYNC2_THREAD_TYPE_SPURS_JOBQUEUE_JOB,
	jqjob_self,
	jqjob_wait,
	jqjob_allocate,
	jqjob_free,
	1000,
	0,
};
