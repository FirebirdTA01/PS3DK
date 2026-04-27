/*! \file cell/sync2.h
 \brief cellSync2 thread-pluggable sync primitives.

  Reference-SDK-compatible cellSync2 surface backed by libsync2_stub.a
  (emitted from tools/nidgen/nids/extracted/libsync2_stub.yaml). Link
  with `-lsync2_stub`.

  cellSync2 vs cellSync: sync2 takes a CellSync2ThreadConfig* on every
  blocking call so it can plug in custom self() / waitSignal() /
  sendSignal() callbacks for PPU threads, PPU fibers, SPURS tasks, or
  user-defined blockable threads. Each primitive is a fixed 128-byte
  opaque struct plus a separately allocated waiting-queue buffer the
  caller supplies via cellSync2*EstimateBufferSize().
*/

#ifndef __PSL1GHT_CELL_SYNC2_H__
#define __PSL1GHT_CELL_SYNC2_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SYNC2_NAME_MAX_LENGTH      31
#define _CELL_SYNC2_INTERNAL_VERSION    0x330000

/* ---- error codes (0x80410C01..0x80410C14) ----------------------------- */

#define CELL_SYNC2_ERROR_AGAIN                   0x80410c01
#define CELL_SYNC2_ERROR_INVAL                   0x80410c02
#define CELL_SYNC2_ERROR_NOSYS                   0x80410c03
#define CELL_SYNC2_ERROR_NOMEM                   0x80410c04
#define CELL_SYNC2_ERROR_SRCH                    0x80410c05
#define CELL_SYNC2_ERROR_NOENT                   0x80410c06
#define CELL_SYNC2_ERROR_NOEXEC                  0x80410c07
#define CELL_SYNC2_ERROR_DEADLK                  0x80410c08
#define CELL_SYNC2_ERROR_PERM                    0x80410c09
#define CELL_SYNC2_ERROR_BUSY                    0x80410c0a
#define CELL_SYNC2_ERROR_ABORT                   0x80410c0c
#define CELL_SYNC2_ERROR_FAULT                   0x80410c0d
#define CELL_SYNC2_ERROR_INTR                    0x80410c0e
#define CELL_SYNC2_ERROR_STAT                    0x80410c0f
#define CELL_SYNC2_ERROR_ALIGN                   0x80410c10
#define CELL_SYNC2_ERROR_NULL_POINTER            0x80410c11
#define CELL_SYNC2_ERROR_NOT_SUPPORTED_THREAD    0x80410c12
#define CELL_SYNC2_ERROR_NO_NOTIFIER             0x80410c13
#define CELL_SYNC2_ERROR_NO_SPU_CONTEXT_STORAGE  0x80410c14

/* ---- thread-type ids (bitmask, register threads against these) -------- */

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

/* ---- primitives + attributes (all 128 bytes, 128-aligned) ------------- */

#define CELL_SYNC2_MUTEX_SIZE              128
#define CELL_SYNC2_MUTEX_ALIGN             128
#define CELL_SYNC2_MUTEX_ATTRIBUTE_SIZE    128
#define CELL_SYNC2_MUTEX_ATTRIBUTE_ALIGN   8

typedef struct CellSync2Mutex {
	uint8_t skip[CELL_SYNC2_MUTEX_SIZE];
} __attribute__((aligned(CELL_SYNC2_MUTEX_ALIGN))) CellSync2Mutex;

typedef struct CellSync2MutexAttribute {
	uint32_t sdkVersion;
	uint16_t threadTypes;
	uint16_t maxWaiters;
	bool     recursive;
	uint8_t  padding;
	char     name[CELL_SYNC2_NAME_MAX_LENGTH + 1];
	uint8_t  reserved[CELL_SYNC2_MUTEX_ATTRIBUTE_SIZE
	                  - sizeof(uint32_t) - sizeof(uint16_t) * 2
	                  - sizeof(bool) - sizeof(uint8_t)
	                  - (CELL_SYNC2_NAME_MAX_LENGTH + 1)];
} __attribute__((aligned(CELL_SYNC2_MUTEX_ATTRIBUTE_ALIGN))) CellSync2MutexAttribute;

#define CELL_SYNC2_COND_SIZE              128
#define CELL_SYNC2_COND_ALIGN             128
#define CELL_SYNC2_COND_ATTRIBUTE_SIZE    128
#define CELL_SYNC2_COND_ATTRIBUTE_ALIGN   8

typedef struct CellSync2Cond {
	uint8_t skip[CELL_SYNC2_COND_SIZE];
} __attribute__((aligned(CELL_SYNC2_COND_ALIGN))) CellSync2Cond;

typedef struct CellSync2CondAttribute {
	uint32_t sdkVersion;
	uint16_t maxWaiters;
	char     name[CELL_SYNC2_NAME_MAX_LENGTH + 1];
	uint8_t  reserved[CELL_SYNC2_COND_ATTRIBUTE_SIZE
	                  - sizeof(uint32_t) - sizeof(uint16_t)
	                  - (CELL_SYNC2_NAME_MAX_LENGTH + 1)];
} __attribute__((aligned(CELL_SYNC2_COND_ATTRIBUTE_ALIGN))) CellSync2CondAttribute;

#define CELL_SYNC2_QUEUE_SIZE              128
#define CELL_SYNC2_QUEUE_ALIGN             128
#define CELL_SYNC2_QUEUE_ATTRIBUTE_SIZE    128
#define CELL_SYNC2_QUEUE_ATTRIBUTE_ALIGN   8
#define CELL_SYNC2_QUEUE_BUFFER_ALIGN      128

typedef struct CellSync2Queue {
	uint8_t skip[CELL_SYNC2_QUEUE_SIZE];
} __attribute__((aligned(CELL_SYNC2_QUEUE_ALIGN))) CellSync2Queue;

typedef struct CellSync2QueueAttribute {
	uint32_t sdkVersion;
	uint32_t threadTypes;
	size_t   elementSize;
	uint32_t depth;
	uint16_t maxPushWaiters;
	uint16_t maxPopWaiters;
	char     name[CELL_SYNC2_NAME_MAX_LENGTH + 1];
	uint8_t  reserved[CELL_SYNC2_QUEUE_ATTRIBUTE_SIZE
	                  - sizeof(uint32_t) * 2 - sizeof(size_t)
	                  - sizeof(uint32_t) - sizeof(uint16_t) * 2
	                  - (CELL_SYNC2_NAME_MAX_LENGTH + 1)];
} __attribute__((aligned(CELL_SYNC2_QUEUE_ATTRIBUTE_ALIGN))) CellSync2QueueAttribute;

#define CELL_SYNC2_SEMAPHORE_SIZE              128
#define CELL_SYNC2_SEMAPHORE_ALIGN             128
#define CELL_SYNC2_SEMAPHORE_ATTRIBUTE_SIZE    128
#define CELL_SYNC2_SEMAPHORE_ATTRIBUTE_ALIGN   8

typedef struct CellSync2Semaphore {
	uint8_t skip[CELL_SYNC2_SEMAPHORE_SIZE];
} __attribute__((aligned(CELL_SYNC2_SEMAPHORE_ALIGN))) CellSync2Semaphore;

typedef struct CellSync2SemaphoreAttribute {
	uint32_t sdkVersion;
	uint16_t threadTypes;
	uint16_t maxWaiters;
	char     name[CELL_SYNC2_NAME_MAX_LENGTH + 1];
	uint8_t  reserved[CELL_SYNC2_SEMAPHORE_ATTRIBUTE_SIZE
	                  - sizeof(uint32_t) - sizeof(uint16_t) * 2
	                  - (CELL_SYNC2_NAME_MAX_LENGTH + 1)];
} __attribute__((aligned(CELL_SYNC2_SEMAPHORE_ATTRIBUTE_ALIGN))) CellSync2SemaphoreAttribute;

/* ---- mutex ----------------------------------------------------------- */

int _cellSync2MutexAttributeInitialize(CellSync2MutexAttribute *attr,
                                       uint32_t sdkVersion);
static inline int cellSync2MutexAttributeInitialize(CellSync2MutexAttribute *attr) {
	return _cellSync2MutexAttributeInitialize(attr, _CELL_SYNC2_INTERNAL_VERSION);
}
int cellSync2MutexEstimateBufferSize(const CellSync2MutexAttribute *attr,
                                     size_t *bufferSize);
int cellSync2MutexInitialize        (CellSync2Mutex *mutex, void *buffer,
                                     const CellSync2MutexAttribute *attr);
int cellSync2MutexFinalize          (CellSync2Mutex *mutex);
int cellSync2MutexLock              (CellSync2Mutex *mutex,
                                     const CellSync2ThreadConfig *config);
int cellSync2MutexTryLock           (CellSync2Mutex *mutex,
                                     const CellSync2ThreadConfig *config);
int cellSync2MutexUnlock            (CellSync2Mutex *mutex,
                                     const CellSync2ThreadConfig *config);

/* ---- condition variable --------------------------------------------- */

int _cellSync2CondAttributeInitialize(CellSync2CondAttribute *attr,
                                      uint32_t sdkVersion);
static inline int cellSync2CondAttributeInitialize(CellSync2CondAttribute *attr) {
	return _cellSync2CondAttributeInitialize(attr, _CELL_SYNC2_INTERNAL_VERSION);
}
int cellSync2CondEstimateBufferSize(const CellSync2CondAttribute *attr,
                                    size_t *bufferSize);
int cellSync2CondInitialize        (CellSync2Cond *cond, CellSync2Mutex *mutex,
                                    void *waitingQueueBuffer,
                                    const CellSync2CondAttribute *attr);
int cellSync2CondFinalize          (CellSync2Cond *cond);
int cellSync2CondWait              (CellSync2Cond *cond,
                                    const CellSync2ThreadConfig *config);
int cellSync2CondSignal            (CellSync2Cond *cond,
                                    const CellSync2ThreadConfig *config);
int cellSync2CondSignalAll         (CellSync2Cond *cond,
                                    const CellSync2ThreadConfig *config);

/* ---- queue ----------------------------------------------------------- */

int _cellSync2QueueAttributeInitialize(CellSync2QueueAttribute *attr,
                                       uint32_t sdkVersion);
static inline int cellSync2QueueAttributeInitialize(CellSync2QueueAttribute *attr) {
	return _cellSync2QueueAttributeInitialize(attr, _CELL_SYNC2_INTERNAL_VERSION);
}
int cellSync2QueueEstimateBufferSize(const CellSync2QueueAttribute *attr,
                                     size_t *bufferSize);
int cellSync2QueueInitialize        (CellSync2Queue *queue, void *buffer,
                                     const CellSync2QueueAttribute *attr);
int cellSync2QueueFinalize          (CellSync2Queue *queue);
int cellSync2QueuePush              (CellSync2Queue *queue, const void *data,
                                     const CellSync2ThreadConfig *config);
int cellSync2QueueTryPush           (CellSync2Queue *queue,
                                     const void *const data[], unsigned int numData,
                                     const CellSync2ThreadConfig *config);
int cellSync2QueuePop               (CellSync2Queue *queue, void *buffer,
                                     const CellSync2ThreadConfig *config);
int cellSync2QueueTryPop            (CellSync2Queue *queue, void *buffer,
                                     const CellSync2ThreadConfig *config);
int cellSync2QueueGetSize           (CellSync2Queue *queue, unsigned int *size);
int cellSync2QueueGetDepth          (CellSync2Queue *queue, unsigned int *depth);

/* ---- semaphore ------------------------------------------------------- */

int _cellSync2SemaphoreAttributeInitialize(CellSync2SemaphoreAttribute *attr,
                                           uint32_t sdkVersion);
static inline int cellSync2SemaphoreAttributeInitialize(CellSync2SemaphoreAttribute *attr) {
	return _cellSync2SemaphoreAttributeInitialize(attr, _CELL_SYNC2_INTERNAL_VERSION);
}
int cellSync2SemaphoreEstimateBufferSize(const CellSync2SemaphoreAttribute *attr,
                                         size_t *bufferSize);
int cellSync2SemaphoreInitialize        (CellSync2Semaphore *semaphore, void *buffer,
                                         int initialValue,
                                         const CellSync2SemaphoreAttribute *attr);
int cellSync2SemaphoreFinalize          (CellSync2Semaphore *semaphore);
int cellSync2SemaphoreAcquire           (CellSync2Semaphore *semaphore, unsigned int count,
                                         const CellSync2ThreadConfig *config);
int cellSync2SemaphoreTryAcquire        (CellSync2Semaphore *semaphore, unsigned int count,
                                         const CellSync2ThreadConfig *config);
int cellSync2SemaphoreRelease           (CellSync2Semaphore *semaphore, unsigned int count,
                                         const CellSync2ThreadConfig *config);
int cellSync2SemaphoreGetCount          (CellSync2Semaphore *semaphore, int *pCount);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYNC2_H__ */
