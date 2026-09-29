/* cell/sync2/thread.h - the SPU-side thread types, notifiers and ready-made
 * thread configurations (libsync2.a).
 *
 * gCellSync2Notifier{PpuThread,PpuFiber} wake PPU waiters from the SPU; the
 * SPURS task, job and job-queue job sets describe an SPU caller and wake a
 * waiter of the same kind.
 */
#ifndef __PS3DK_CELL_SYNC2_THREAD_H_SPU__
#define __PS3DK_CELL_SYNC2_THREAD_H_SPU__

#include <cell/sync2/thread_types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern CellSync2Notifier gCellSync2NotifierPpuThread;

extern CellSync2Notifier gCellSync2NotifierPpuFiber;

extern CellSync2CallerThreadType gCellSync2CallerThreadTypeSpursTask;
extern CellSync2Notifier gCellSync2NotifierSpursTask;
extern CellSync2ThreadConfig gCellSync2ThreadConfigSpursTask;

extern CellSync2CallerThreadType gCellSync2CallerThreadTypeSpursJob;
extern CellSync2Notifier gCellSync2NotifierSpursJob;
extern CellSync2ThreadConfig gCellSync2ThreadConfigSpursJob;

extern CellSync2CallerThreadType gCellSync2CallerThreadTypeSpursJobQueueJob;
extern CellSync2Notifier gCellSync2NotifierSpursJobQueueJob;
extern CellSync2ThreadConfig gCellSync2ThreadConfigSpursJobQueueJob;

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYNC2_THREAD_H_SPU__ */
