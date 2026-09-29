/* gCellSync2ThreadConfigSpursJob - a SPURS job calling sync2, able to wake
 * PPU threads, PPU fibers, SPURS tasks and job queue jobs. */
#include <cell/sync2/thread.h>

static CellSync2Notifier *s_notifiers[] = {
	&gCellSync2NotifierPpuThread,
	&gCellSync2NotifierPpuFiber,
	&gCellSync2NotifierSpursTask,
	&gCellSync2NotifierSpursJobQueueJob,
};

CellSync2ThreadConfig gCellSync2ThreadConfigSpursJob = {
	&gCellSync2CallerThreadTypeSpursJob,
	s_notifiers,
	sizeof s_notifiers / sizeof s_notifiers[0],
};
