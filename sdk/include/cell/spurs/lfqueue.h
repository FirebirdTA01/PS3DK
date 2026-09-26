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

typedef struct CellSpursLFQueue {
    unsigned char skip[CELL_SPURS_LFQUEUE_SIZE];
} __attribute__((aligned(CELL_SPURS_LFQUEUE_ALIGN))) CellSpursLFQueue;

#ifdef __SPU__

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

/* Declared only: the SPURS-flavoured bodies (which block through the
 * taskset instead of spinning) are not in the SPU runtime yet. */
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

#endif /* __SPU__ */

#endif /* __PS3DK_CELL_SPURS_LFQUEUE_H__ */
