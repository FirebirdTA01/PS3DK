/*! \file cell/sync.h
 \brief cellSync user-space synchronisation primitives.

  Reference-SDK-compatible cellSync surface backed by libsync_stub.a
  (emitted from tools/nidgen/nids/extracted/libsync_stub.yaml). Link
  with `-lsync_stub`.

  Five families of primitives, all PPU-resident. SPU-side variants
  exist for the same types but live in a separate SPU-only header
  outside this SDK.

    - CellSyncMutex     — ticket-style fair mutex (32-bit)
    - CellSyncBarrier   — N-thread barrier (32-bit)
    - CellSyncRwm       — read/write lock with attached buffer
    - CellSyncQueue     — FIFO queue, fixed-size entries, blocking +
                          try-variants
    - CellSyncLFQueue   — lock-free queue, 128-byte aligned, with
                          PPU/SPU direction tag

  Distinct from sys/synchronization.h: the `sys_*` family wraps LV2
  kernel sync (sys_event_flag, sys_lwmutex) reached via syscall. The
  cellSync* family is user-space, atomics-backed, with a thinner SPRX
  surface that calls into the kernel only when blocking.
*/

#ifndef __PSL1GHT_CELL_SYNC_H__
#define __PSL1GHT_CELL_SYNC_H__

#include <stdint.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes (0x80410101..0x80410113) ----------------------------- */

#define CELL_SYNC_ERROR_AGAIN         0x80410101
#define CELL_SYNC_ERROR_INVAL         0x80410102
#define CELL_SYNC_ERROR_NOSYS         0x80410103
#define CELL_SYNC_ERROR_NOMEM         0x80410104
#define CELL_SYNC_ERROR_SRCH          0x80410105
#define CELL_SYNC_ERROR_NOENT         0x80410106
#define CELL_SYNC_ERROR_NOEXEC        0x80410107
#define CELL_SYNC_ERROR_DEADLK        0x80410108
#define CELL_SYNC_ERROR_PERM          0x80410109
#define CELL_SYNC_ERROR_BUSY          0x8041010a
#define CELL_SYNC_ERROR_ABORT         0x8041010c
#define CELL_SYNC_ERROR_FAULT         0x8041010d
#define CELL_SYNC_ERROR_CHILD         0x8041010e
#define CELL_SYNC_ERROR_STAT          0x8041010f
#define CELL_SYNC_ERROR_ALIGN         0x80410110
#define CELL_SYNC_ERROR_NULL_POINTER  0x80410111
#define CELL_SYNC_ERROR_SHOTAGE       0x80410112  /* sic — reference spelling */
#define CELL_SYNC_ERROR_UNKNOWNKEY    0x80410113

#define CELL_SYNC_LFQUEUE_TERMINATED  2

/* ---- CellSyncMutex (ticket-fair mutex) ------------------------------- */

typedef union CellSyncMutex {
	struct CellSyncMutex_ticket {
		uint16_t current_ticket;
		uint16_t next_ticket;
	} ticket;
	uint32_t uint_val;
} CellSyncMutex;

int cellSyncMutexInitialize(CellSyncMutex *m);
int cellSyncMutexTryLock   (CellSyncMutex *m);
int cellSyncMutexLock      (CellSyncMutex *m);
int cellSyncMutexUnlock    (CellSyncMutex *m);

/* ---- CellSyncBarrier (N-thread rendezvous) --------------------------- */

typedef union _CellSyncBarrier {
	struct _CellSyncBarrier_count {
		uint16_t count;
		uint16_t total_count;
	} count;
	uint32_t uint_val;
} CellSyncBarrier;

int cellSyncBarrierInitialize(CellSyncBarrier *barrier, uint16_t total_count);
int cellSyncBarrierNotify    (CellSyncBarrier *barrier);
int cellSyncBarrierTryNotify (CellSyncBarrier *barrier);
int cellSyncBarrierWait      (CellSyncBarrier *barrier);
int cellSyncBarrierTryWait   (CellSyncBarrier *barrier);

/* ---- CellSyncRwm (read/write lock + attached buffer) ----------------- */

typedef struct CellSyncRwm {
	union CellSyncRwm_lock {
		struct CellSyncRwm_rwlock {
			uint16_t rlock;
			uint16_t wlock;
		} rwlock;
		uint32_t uint_val;
	} lock;
	uint32_t buffer_size;
	uint64_t buffer;
} CellSyncRwm;

int cellSyncRwmInitialize(CellSyncRwm *obj, void *buffer, uint32_t buffer_size);
int cellSyncRwmRead      (CellSyncRwm *obj, void *buffer);
int cellSyncRwmTryRead   (CellSyncRwm *obj, void *buffer);
int cellSyncRwmWrite     (CellSyncRwm *obj, void *buffer);
int cellSyncRwmTryWrite  (CellSyncRwm *obj, void *buffer);

/* ---- CellSyncQueue (fixed-size FIFO) --------------------------------- */

typedef union _CellSyncQueueHead {
#ifdef __GNUC__
	__extension__
#endif
	struct _CellSyncQueueHeadStr {
		uint64_t rlock :  8;
		uint64_t index : 24;
		uint64_t wlock :  8;
		uint64_t size  : 24;
	} asStr;
	uint64_t asUint;
} _CellSyncQueueHead;

typedef struct _CellSyncQueue {
	_CellSyncQueueHead head;
	uint32_t           buffer_size;
	uint32_t           depth;
	uint64_t           addr_buffer;
	uint64_t           _padding;
} CellSyncQueue;

int          cellSyncQueueInitialize(CellSyncQueue *obj, void *ptr_buffer,
                                     uint32_t buffer_size, uint32_t depth);
int          cellSyncQueuePush      (CellSyncQueue *obj, const void *buf);
int          cellSyncQueueTryPush   (CellSyncQueue *obj, const void *buf);
int          cellSyncQueuePop       (CellSyncQueue *obj, void *buf);
int          cellSyncQueueTryPop    (CellSyncQueue *obj, void *buf);
int          cellSyncQueuePeek      (CellSyncQueue *obj, void *buf);
int          cellSyncQueueTryPeek   (CellSyncQueue *obj, void *buf);
int          cellSyncQueueClear     (CellSyncQueue *obj);
unsigned int cellSyncQueueSize      (CellSyncQueue *obj);

/* ---- CellSyncLFQueue (lock-free queue, PPU<->SPU) -------------------- */

typedef enum CellSyncQueueDirection {
	CELL_SYNC_QUEUE_SPU2SPU = 0,
	CELL_SYNC_QUEUE_SPU2PPU = 1,
	CELL_SYNC_QUEUE_PPU2SPU = 2,
	CELL_SYNC_QUEUE_ANY2ANY = 3
} CellSyncQueueDirection;

#define CELL_SYNC_LFQUEUE_ALIGN 128
#define CELL_SYNC_LFQUEUE_SIZE  128

typedef struct CellSyncLFQueue {
	unsigned char skip[CELL_SYNC_LFQUEUE_SIZE];
} __attribute__((aligned(CELL_SYNC_LFQUEUE_ALIGN))) CellSyncLFQueue;

int cellSyncLFQueueInitialize    (CellSyncLFQueue *queue, const void *buffer,
                                  unsigned int size, unsigned int depth,
                                  CellSyncQueueDirection direction, void *pSignal);
int cellSyncLFQueueClear         (CellSyncLFQueue *queue);
int cellSyncLFQueueSize          (CellSyncLFQueue *queue, unsigned int *size);
int cellSyncLFQueueDepth         (CellSyncLFQueue *queue, unsigned int *depth);
int cellSyncLFQueueGetDirection  (const CellSyncLFQueue *queue,
                                  CellSyncQueueDirection *pDirection);
int cellSyncLFQueueGetEntrySize  (const CellSyncLFQueue *queue,
                                  unsigned int *pSize);

/* Internal blocking/non-blocking bodies. The push/pop wrappers below
 * compile to direct branches; downstream callers should prefer the
 * wrappers, not the bodies. */
int _cellSyncLFQueuePushBody       (CellSyncLFQueue *queue, const void *buffer,
                                    unsigned int isBlocking);
int _cellSyncLFQueuePopBody        (CellSyncLFQueue *queue, void *buffer,
                                    unsigned int isBlocking);
int _cellSyncLFQueueGetSignalAddress(const CellSyncLFQueue *queue, void **ppSignal);

static inline int cellSyncLFQueuePush(CellSyncLFQueue *queue, const void *buffer) {
	return _cellSyncLFQueuePushBody(queue, buffer, 1);
}
static inline int cellSyncLFQueueTryPush(CellSyncLFQueue *queue, const void *buffer) {
	return _cellSyncLFQueuePushBody(queue, buffer, 0);
}
static inline int cellSyncLFQueuePop(CellSyncLFQueue *queue, void *buffer) {
	return _cellSyncLFQueuePopBody(queue, buffer, 1);
}
static inline int cellSyncLFQueueTryPop(CellSyncLFQueue *queue, void *buffer) {
	return _cellSyncLFQueuePopBody(queue, buffer, 0);
}

/* SPU thread group / event-queue attach for LFQueue.  Forward-declared
 * with the canonical PPC64 signature; sys_spu_thread_t resolves to a
 * 32-bit thread id. Pulling <sys/spu_thread.h> here would drag in the
 * whole SPU-thread surface for callers that just want sync, so we take
 * uint32_t * (the underlying type) and let the caller cast. */
int _cellSyncLFQueueAttachLv2EventQueue(uint32_t *ids, unsigned int numSpus,
                                        CellSyncLFQueue *pQueue);
int _cellSyncLFQueueDetachLv2EventQueue(uint32_t *ids, unsigned int numSpus,
                                        CellSyncLFQueue *pQueue);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYNC_H__ */
