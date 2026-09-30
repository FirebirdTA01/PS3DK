/* gCellSync2ThreadConfigSpursTask - a SPURS task calling sync2, able to wake
 * PPU threads, PPU fibers, SPURS tasks and job queue jobs. */
#include <cell/sync2/thread.h>

static CellSync2Notifier *s_notifiers[] = {
	&gCellSync2NotifierPpuThread,
	&gCellSync2NotifierPpuFiber,
	&gCellSync2NotifierSpursTask,
	&gCellSync2NotifierSpursJobQueueJob,
};

CellSync2ThreadConfig gCellSync2ThreadConfigSpursTask = {
	&gCellSync2CallerThreadTypeSpursTask,
	s_notifiers,
	sizeof s_notifiers / sizeof s_notifiers[0],
};
