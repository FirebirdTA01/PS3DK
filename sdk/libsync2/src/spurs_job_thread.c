/* gCellSync2CallerThreadTypeSpursJob - a SPURS job calling sync2.  A job
 * cannot block, so it has no signal receiver and may only use the
 * non-blocking calls (TryLock, TryAcquire, Release, Signal, TryPush ...). */
#include <cell/sync2/thread.h>
#include "sync2_internal.h"
#include "spurs_job_id.h"

static uint64_t s_id;

static CellSync2ThreadId job_self(uint64_t arg)
{
	(void)arg;
	return s2_job_id(&s_id);
}

CellSync2CallerThreadType gCellSync2CallerThreadTypeSpursJob = {
	CELL_SYNC2_THREAD_TYPE_SPURS_JOB,
	job_self,
	0,
	0,
	0,
	0,
	0,
};
