/* cell/sync2/thread.h - the PPU thread types and notifiers the libsync2
 * module provides.
 *
 * gCellSync2CallerThreadType{PpuThread,PpuFiber} describe a calling PPU
 * thread or fiber; gCellSync2Notifier* wake a waiter of each kind.  They are
 * variables of the libsync2 system module.  The stub (libsync2_stub.a)
 * imports each one: the loader stores the variable's address in a 32-bit
 * slot, <name>_vslot, and stores it again when cellSysmoduleLoadModule
 * loads the module.  Each name below reads that slot, so it is valid once
 * CELL_SYSMODULE_SYNC2 is loaded.  Because the address is only known at run
 * time, the names cannot appear in a static initialiser: build such tables
 * at run time, after loading the module.
 */
#ifndef __PS3DK_CELL_SYNC2_THREAD_H__
#define __PS3DK_CELL_SYNC2_THREAD_H__

#include <stdint.h>
#include <cell/sync2/thread_types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile const uint32_t gCellSync2CallerThreadTypePpuThread_vslot;
extern volatile const uint32_t gCellSync2NotifierPpuThread_vslot;
extern volatile const uint32_t gCellSync2CallerThreadTypePpuFiber_vslot;
extern volatile const uint32_t gCellSync2NotifierPpuFiber_vslot;
extern volatile const uint32_t gCellSync2NotifierSpursTask_vslot;
extern volatile const uint32_t gCellSync2NotifierSpursJobQueueJob_vslot;

#ifdef __cplusplus
}
#endif

#define __CELL_SYNC2_MODULE_VARIABLE(type, name) (*(type *)(uintptr_t)name##_vslot)

#define gCellSync2CallerThreadTypePpuThread \
	__CELL_SYNC2_MODULE_VARIABLE(CellSync2CallerThreadType, gCellSync2CallerThreadTypePpuThread)
#define gCellSync2NotifierPpuThread \
	__CELL_SYNC2_MODULE_VARIABLE(CellSync2Notifier, gCellSync2NotifierPpuThread)
#define gCellSync2CallerThreadTypePpuFiber \
	__CELL_SYNC2_MODULE_VARIABLE(CellSync2CallerThreadType, gCellSync2CallerThreadTypePpuFiber)
#define gCellSync2NotifierPpuFiber \
	__CELL_SYNC2_MODULE_VARIABLE(CellSync2Notifier, gCellSync2NotifierPpuFiber)
#define gCellSync2NotifierSpursTask \
	__CELL_SYNC2_MODULE_VARIABLE(CellSync2Notifier, gCellSync2NotifierSpursTask)
#define gCellSync2NotifierSpursJobQueueJob \
	__CELL_SYNC2_MODULE_VARIABLE(CellSync2Notifier, gCellSync2NotifierSpursJobQueueJob)

#endif /* __PS3DK_CELL_SYNC2_THREAD_H__ */
