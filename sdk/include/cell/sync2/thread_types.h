/* cell/sync2/thread_types.h - thread types, notifiers and the per-call thread configuration.
 *
 * A blocking call names its caller through CellSync2ThreadConfig: the
 * caller thread type supplies self(), waitSignal() and the signal receiver
 * allocation; the notifier table, indexed by thread type, supplies
 * sendSignal() for every kind of waiter the call may wake.
 */
#ifndef __PS3DK_CELL_SYNC2_THREAD_TYPES_H__
#define __PS3DK_CELL_SYNC2_THREAD_TYPES_H__

#include <stdint.h>
#include <cell/sync2/error.h>

#define CELL_SYNC2_THREAD_TYPE_INVALID                     0
#define CELL_SYNC2_THREAD_TYPE_PPU_THREAD                  (1u << 0)
#define CELL_SYNC2_THREAD_TYPE_PPU_FIBER                   (1u << 1)
#define CELL_SYNC2_THREAD_TYPE_SPURS_TASK                  (1u << 2)
#define CELL_SYNC2_THREAD_TYPE_SPURS_JOBQUEUE_JOB          (1u << 3)
#define CELL_SYNC2_THREAD_TYPE_USER_DEFINED_BLOCKABLE0     (1u << 6)
#define CELL_SYNC2_THREAD_TYPE_USER_DEFINED_BLOCKABLE1     (1u << 7)
#define CELL_SYNC2_THREAD_TYPE_SPURS_JOB                   (1u << 8)
#define CELL_SYNC2_THREAD_TYPE_USER_DEFINED_UNBLOCKABLE0   (1u << 14)
#define CELL_SYNC2_THREAD_TYPE_USER_DEFINED_UNBLOCKABLE1   (1u << 15)

#define CELL_SYNC2_WAITING_QUEUE_ALIGN  8

typedef enum CellSync2ObjectTypeId {
	CELL_SYNC2_OBJECT_TYPE_MUTUEX    = 1,  /* sic — reference spelling */
	CELL_SYNC2_OBJECT_TYPE_COND      = 2,
	CELL_SYNC2_OBJECT_TYPE_QUEUE     = 3,
	CELL_SYNC2_OBJECT_TYPE_SEMAPHORE = 4,
} CellSync2ObjectTypeId;

typedef uint16_t CellSync2ThreadTypeId;
typedef uint64_t CellSync2ThreadId;
typedef uint64_t CellSync2SignalReceiverId;

typedef struct CellSync2CallerThreadType {
	CellSync2ThreadTypeId threadTypeId;
	CellSync2ThreadId (*self)(uint64_t);
	int (*waitSignal)(CellSync2SignalReceiverId, CellSync2ObjectTypeId, uint64_t, uint64_t);
	int (*allocateSignalReceiver)(CellSync2SignalReceiverId *, CellSync2ObjectTypeId, uint64_t, uint64_t);
	int (*freeSignalReceiver)(CellSync2SignalReceiverId, uint64_t);
	unsigned int spinWaitNanoSec;
	uint64_t callbackArg;
} CellSync2CallerThreadType;

typedef struct CellSync2Notifier {
	CellSync2ThreadTypeId threadTypeId;
	int (*sendSignal)(CellSync2SignalReceiverId, uint64_t);
	uint64_t callbackArg;
} CellSync2Notifier;

typedef struct CellSync2ThreadConfig {
	CellSync2CallerThreadType *callerThreadType;
	CellSync2Notifier        **notifierTable;
	unsigned int               numNotifier;
} CellSync2ThreadConfig;

#endif /* __PS3DK_CELL_SYNC2_THREAD_TYPES_H__ */
