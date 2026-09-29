/* cell/sync2/thread.h - the PPU thread types and notifiers the libsync2
 * module provides.
 *
 * gCellSync2CallerThreadType{PpuThread,PpuFiber} describe a calling PPU
 * thread or fiber; gCellSync2Notifier* wake a waiter of each kind.  They are
 * variables of the module: the stub resolves each one at load time and
 * copies it here before main (see sdk/libsync2_vars), so they can be used in
 * static initialisers like any other object.
 */
#ifndef __PS3DK_CELL_SYNC2_THREAD_H__
#define __PS3DK_CELL_SYNC2_THREAD_H__

#include <cell/sync2/thread_types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern CellSync2CallerThreadType gCellSync2CallerThreadTypePpuThread;
extern CellSync2Notifier gCellSync2NotifierPpuThread;

extern CellSync2CallerThreadType gCellSync2CallerThreadTypePpuFiber;
extern CellSync2Notifier gCellSync2NotifierPpuFiber;

extern CellSync2Notifier gCellSync2NotifierSpursTask;

extern CellSync2Notifier gCellSync2NotifierSpursJobQueueJob;

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYNC2_THREAD_H__ */
