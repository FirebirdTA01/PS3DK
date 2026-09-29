/* gCellSync2ThreadConfigSpursJobQueueJob - a job queue job calling sync2, able to wake
 * PPU threads, PPU fibers, SPURS tasks and job queue jobs. */
#include <cell/sync2/thread.h>

static CellSync2Notifier *s_notifiers[] = {
	&gCellSync2NotifierPpuThread,
	&gCellSync2NotifierPpuFiber,
	&gCellSync2NotifierSpursTask,
	&gCellSync2NotifierSpursJobQueueJob,
};

CellSync2ThreadConfig gCellSync2ThreadConfigSpursJobQueueJob = {
	&gCellSync2CallerThreadTypeSpursJobQueueJob,
	s_notifiers,
	sizeof s_notifiers / sizeof s_notifiers[0],
};
