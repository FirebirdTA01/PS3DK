/* cell/spurs/lfqueue.h - CellSpursLFQueue type.
 *
 * Lock-free queue within a SPURS context.  Opaque 128-byte aligned
 * structure matching the CellSyncLFQueue pattern.
 *
 * On the SPU the queue is named by its effective address and entries
 * move through a push / pop container (LS buffer + DMA tag), layered
 * on the libsync lock-free queue.
 */
#ifndef __PS3DK_CELL_SPURS_LFQUEUE_H__
#define __PS3DK_CELL_SPURS_LFQUEUE_H__

#include <stdint.h>

#define CELL_SPURS_LFQUEUE_ALIGN 128
#define CELL_SPURS_LFQUEUE_SIZE  128

#ifdef __SPU__

typedef struct CellSpursLFQueue {
    unsigned char skip[CELL_SPURS_LFQUEUE_SIZE];
} __attribute__((aligned(CELL_SPURS_LFQUEUE_ALIGN))) CellSpursLFQueue;

#include <cell/sync/lfqueue.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/error.h>

#define CELL_SPURS_LFQUEUE_SPU2SPU  CELL_SYNC_QUEUE_SPU2SPU
#define CELL_SPURS_LFQUEUE_SPU2PPU  CELL_SYNC_QUEUE_SPU2PPU
#define CELL_SPURS_LFQUEUE_PPU2SPU  CELL_SYNC_QUEUE_PPU2SPU
#define CELL_SPURS_LFQUEUE_ANY2ANY  CELL_SYNC_QUEUE_ANY2ANY

typedef CellSyncQueueDirection        CellSpursLFQueueDirection;
typedef CellSyncLFQueuePushContainer  CellSpursLFQueuePushContainer;
typedef CellSyncLFQueuePopContainer   CellSpursLFQueuePopContainer;

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented in libspurs.a / libspurs_task.a over libsync.a (link both);
 * blocking forms are valid only in a SPURS task. */
int cellSpursLFQueueInitialize(uint64_t ea, uint64_t buffer,
                               unsigned int size, unsigned int depth,
                               CellSpursLFQueueDirection direction);
int cellSpursLFQueueInitializeIWL(uint64_t ea, uint64_t buffer,
                                  unsigned int size, unsigned int depth,
                                  CellSpursLFQueueDirection direction);
int cellSpursLFQueueGetTasksetAddress(uint64_t ea, uint64_t *pEaTaskset);
int _cellSpursLFQueuePushBeginBody(uint64_t ea,
                                   CellSpursLFQueuePushContainer *pContainer,
                                   unsigned int isBlocking);
int _cellSpursLFQueuePopBeginBody(uint64_t ea,
                                  CellSpursLFQueuePopContainer *pContainer,
                                  unsigned int isBlocking);
int cellSpursLFQueuePushEnd(uint64_t ea,
                            CellSpursLFQueuePushContainer *pContainer);
int cellSpursLFQueuePopEnd(uint64_t ea,
                           CellSpursLFQueuePopContainer *pContainer);

#ifdef __cplusplus
}   /* extern "C" */
#endif

/* Re-express a libsync failure in the SPURS task error facility; the
 * low byte (the errno-style code) is kept. */
static inline int
__ps3dk_spurs_lfqueue_status(int ret)
{
    if (ret < 0)
        return CELL_ERROR_CAST(0x80410900u | ((unsigned int)ret & 0xffu));
    return ret;
}

static inline void
cellSpursLFQueuePushContainerInitialize(CellSpursLFQueuePushContainer *container,
                                        const void *buffer,
                                        const unsigned int tag)
{
    cellSyncLFQueuePushContainerInitialize(container, buffer, tag);
}

static inline void
cellSpursLFQueuePopContainerInitialize(CellSpursLFQueuePopContainer *container,
                                       void *buffer,
                                       const unsigned int tag)
{
    cellSyncLFQueuePopContainerInitialize(container, buffer, tag);
}

/* Queries answered by the shared libsync queue bodies. */
static inline int
cellSpursLFQueueSize(uint64_t ea, unsigned int *size)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueSize(ea, size)); }

static inline int
cellSpursLFQueueDepth(uint64_t ea, unsigned int *depth)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueDepth(ea, depth)); }

static inline int
cellSpursLFQueueGetDirection(uint64_t ea, CellSpursLFQueueDirection *direction)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueGetDirection(ea, direction)); }

static inline int
cellSpursLFQueueGetEntrySize(uint64_t ea, unsigned int *size)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueGetEntrySize(ea, size)); }

static inline int
cellSpursLFQueueClear(uint64_t ea)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueClear(ea)); }

static inline int
cellSpursLFQueuePushBegin(uint64_t ea, CellSpursLFQueuePushContainer *pContainer)
{ return _cellSpursLFQueuePushBeginBody(ea, pContainer, 1); }

static inline int
cellSpursLFQueueTryPushBegin(uint64_t ea, CellSpursLFQueuePushContainer *pContainer)
{ return _cellSpursLFQueuePushBeginBody(ea, pContainer, 0); }

static inline int
cellSpursLFQueuePopBegin(uint64_t ea, CellSpursLFQueuePopContainer *pContainer)
{ return _cellSpursLFQueuePopBeginBody(ea, pContainer, 1); }

static inline int
cellSpursLFQueueTryPopBegin(uint64_t ea, CellSpursLFQueuePopContainer *pContainer)
{ return _cellSpursLFQueuePopBeginBody(ea, pContainer, 0); }

#ifdef __cplusplus
namespace cell {
namespace Spurs {

/* SPU handle on a lock-free queue in main memory: holds its EA and
 * forwards to the EA-based C API. */
class LFQueueStub {
protected:
    uint64_t object_ea;

public:
    static const uint32_t kAlign = CELL_SPURS_LFQUEUE_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_LFQUEUE_SIZE;

    void setObject(uint64_t ea) { object_ea = ea; }
    uint64_t getObject(void) const { return object_ea; }

    int initialize(uint64_t buffer, unsigned int size, unsigned int depth,
                   CellSpursLFQueueDirection direction) const
    { return cellSpursLFQueueInitialize(object_ea, buffer, size, depth, direction); }
    int initializeIWL(uint64_t buffer, unsigned int size, unsigned int depth,
                      CellSpursLFQueueDirection direction) const
    { return cellSpursLFQueueInitializeIWL(object_ea, buffer, size, depth, direction); }
    int getTasksetAddress(uint64_t *pEaTaskset) const
    { return cellSpursLFQueueGetTasksetAddress(object_ea, pEaTaskset); }

    int pushBegin(CellSpursLFQueuePushContainer *c) const
    { return cellSpursLFQueuePushBegin(object_ea, c); }
    int tryPushBegin(CellSpursLFQueuePushContainer *c) const
    { return cellSpursLFQueueTryPushBegin(object_ea, c); }
    int pushEnd(CellSpursLFQueuePushContainer *c) const
    { return cellSpursLFQueuePushEnd(object_ea, c); }
    int popBegin(CellSpursLFQueuePopContainer *c) const
    { return cellSpursLFQueuePopBegin(object_ea, c); }
    int tryPopBegin(CellSpursLFQueuePopContainer *c) const
    { return cellSpursLFQueueTryPopBegin(object_ea, c); }
    int popEnd(CellSpursLFQueuePopContainer *c) const
    { return cellSpursLFQueuePopEnd(object_ea, c); }

    int size(unsigned int *size) const { return cellSpursLFQueueSize(object_ea, size); }
    int depth(unsigned int *depth) const { return cellSpursLFQueueDepth(object_ea, depth); }
    int clear(void) const { return cellSpursLFQueueClear(object_ea); }
    int getDirection(CellSpursLFQueueDirection *direction) const
    { return cellSpursLFQueueGetDirection(object_ea, direction); }
    int getEntrySize(unsigned int *size) const
    { return cellSpursLFQueueGetEntrySize(object_ea, size); }
};

}   /* namespace Spurs */
}   /* namespace cell */
#endif /* __cplusplus */

#else /* PPU */

#include <stdbool.h>
#include <cell/sync.h>
#include <cell/spurs/types.h>
#include <cell/spurs/error.h>

/* The SPURS queue is a libsync lock-free queue whose waiters block
 * through SPURS: a taskset's tasks, or a whole instance's (IWL). */
typedef CellSyncLFQueue CellSpursLFQueue;
typedef CellSyncQueueDirection CellSpursLFQueueDirection;

#define CELL_SPURS_LFQUEUE_SPU2SPU  CELL_SYNC_QUEUE_SPU2SPU
#define CELL_SPURS_LFQUEUE_SPU2PPU  CELL_SYNC_QUEUE_SPU2PPU
#define CELL_SPURS_LFQUEUE_PPU2SPU  CELL_SYNC_QUEUE_PPU2SPU
#define CELL_SPURS_LFQUEUE_ANY2ANY  CELL_SYNC_QUEUE_ANY2ANY

#ifdef __cplusplus
extern "C" {
#endif

/* pTasksetOrSpurs: a CellSpursTaskset, or a CellSpurs with bit 0 set */
int _cellSpursLFQueueInitialize(void *pTasksetOrSpurs, CellSpursLFQueue *pQueue,
                                const void *buffer, unsigned int size,
                                unsigned int depth, CellSpursLFQueueDirection direction);
int cellSpursLFQueueAttachLv2EventQueue(CellSpursLFQueue *pQueue);
int cellSpursLFQueueDetachLv2EventQueue(CellSpursLFQueue *pQueue);
int cellSpursLFQueueGetTasksetAddress(const CellSpursLFQueue *pQueue,
                                      struct CellSpursTaskset **ppTaskset);
int _cellSpursLFQueuePushBody(CellSpursLFQueue *pQueue, const void *buffer,
                              unsigned int isBlocking);
int _cellSpursLFQueuePopBody(CellSpursLFQueue *pQueue, void *buffer,
                             unsigned int isBlocking);

#ifdef __cplusplus
}   /* extern "C" */
#endif

/* Re-express a libsync failure in the SPURS task error facility; the
 * low byte (the errno-style code) is kept. */
static inline int
__ps3dk_spurs_lfqueue_status(int ret)
{
    if (ret < 0)
        return CELL_ERROR_CAST(0x80410900u | ((unsigned int)ret & 0xffu));
    return ret;
}

static inline int
cellSpursLFQueueInitialize(struct CellSpursTaskset *pTaskset, CellSpursLFQueue *pQueue,
                           const void *buffer, unsigned int size, unsigned int depth,
                           CellSpursLFQueueDirection direction)
{
    if (!pTaskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if ((uintptr_t)pTaskset & 0x7f)
        return CELL_SPURS_TASK_ERROR_ALIGN;
    return _cellSpursLFQueueInitialize((void *)pTaskset, pQueue, buffer, size, depth, direction);
}

static inline int
cellSpursLFQueueInitializeIWL(CellSpurs *pSpurs, CellSpursLFQueue *pQueue,
                              const void *buffer, unsigned int size, unsigned int depth,
                              CellSpursLFQueueDirection direction)
{
    if (!pSpurs)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if ((uintptr_t)pSpurs & 0x7f)
        return CELL_SPURS_TASK_ERROR_ALIGN;
    return _cellSpursLFQueueInitialize((void *)((uintptr_t)pSpurs | 1), pQueue, buffer,
                                       size, depth, direction);
}

static inline int
cellSpursLFQueueSize(CellSpursLFQueue *pQueue, unsigned int *size)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueSize(pQueue, size)); }

static inline int
cellSpursLFQueueDepth(CellSpursLFQueue *pQueue, unsigned int *depth)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueDepth(pQueue, depth)); }

static inline int
cellSpursLFQueueGetDirection(const CellSpursLFQueue *pQueue, CellSpursLFQueueDirection *direction)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueGetDirection(pQueue, direction)); }

static inline int
cellSpursLFQueueGetEntrySize(const CellSpursLFQueue *pQueue, unsigned int *size)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueGetEntrySize(pQueue, size)); }

static inline int
cellSpursLFQueueClear(CellSpursLFQueue *pQueue)
{ return __ps3dk_spurs_lfqueue_status(cellSyncLFQueueClear(pQueue)); }

static inline int
cellSpursLFQueuePush(CellSpursLFQueue *pQueue, const void *buffer)
{ return _cellSpursLFQueuePushBody(pQueue, buffer, 1); }

static inline int
cellSpursLFQueueTryPush(CellSpursLFQueue *pQueue, const void *buffer)
{ return _cellSpursLFQueuePushBody(pQueue, buffer, 0); }

static inline int
cellSpursLFQueuePop(CellSpursLFQueue *pQueue, void *buffer)
{ return _cellSpursLFQueuePopBody(pQueue, buffer, 1); }

static inline int
cellSpursLFQueueTryPop(CellSpursLFQueue *pQueue, void *buffer)
{ return _cellSpursLFQueuePopBody(pQueue, buffer, 0); }

#endif /* __SPU__ */

#endif /* __PS3DK_CELL_SPURS_LFQUEUE_H__ */
