/*
 * cell/sync/lfqueue.h -- SPU-side lock-free queue declarations.
 *
 * Declares the cellSyncLFQueue* entry points plus inline container
 * initializers and blocking/try wrappers.  Implementations live in
 * libsync.a.
 *
 * Arbitrary depth supported (no cap).  The caller must allocate
 * depth*entry_size + padding + depth*4 bytes for the buffer.
 */
#ifndef _PS3DK_CELL_SYNC_LFQUEUE_H_SPU_
#define _PS3DK_CELL_SYNC_LFQUEUE_H_SPU_

#include <stdint.h>
#include <cell/sync/error.h>
#include <spu_mfcio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction enum. */
typedef enum {
    CELL_SYNC_QUEUE_SPU2SPU = 0,
    CELL_SYNC_QUEUE_SPU2PPU = 1,
    CELL_SYNC_QUEUE_PPU2SPU = 2,
    CELL_SYNC_QUEUE_ANY2ANY = 3
} CellSyncQueueDirection;

/* Push/pop container types. */
typedef struct {
    const void *buffer;
    int pointer;
    unsigned int tag;
} CellSyncLFQueuePushContainer;

typedef struct {
    void *buffer;
    int pointer;
    unsigned int tag;
} CellSyncLFQueuePopContainer;

int cellSyncLFQueueInitialize(uint64_t ea, uint64_t buffer,
                              unsigned int size, unsigned int depth,
                              CellSyncQueueDirection, uint64_t eaSignal);
int cellSyncLFQueueSize(uint64_t ea, unsigned int *size);
int cellSyncLFQueueDepth(uint64_t ea, unsigned int *depth);
int cellSyncLFQueueClear(uint64_t ea);
int cellSyncLFQueueGetDirection(uint64_t ea, CellSyncQueueDirection *direction);
int cellSyncLFQueueGetEntrySize(uint64_t ea, unsigned int *pSize);

int _cellSyncLFQueuePushBeginBody(uint64_t ea,
                                  CellSyncLFQueuePushContainer *pContainer,
                                  unsigned int isBlocking);
int cellSyncLFQueuePushEnd(uint64_t ea,
                           CellSyncLFQueuePushContainer *pContainer);
int _cellSyncLFQueuePopBeginBody(uint64_t ea,
                                 CellSyncLFQueuePopContainer *pContainer,
                                 unsigned int isBlocking);
int cellSyncLFQueuePopEnd(uint64_t ea,
                          CellSyncLFQueuePopContainer *pContainer);

int _cellSyncLFQueueGetSignalAddress(uint64_t ea, uint64_t *pSignal);

#ifdef __cplusplus
}
#endif

/* Inline container initializers. */
static inline void
cellSyncLFQueuePushContainerInitialize(CellSyncLFQueuePushContainer *container,
                                       const void *buffer, const unsigned int tag)
{
    container->buffer = buffer;
    container->pointer = 0;
    container->tag = tag;
}

static inline void
cellSyncLFQueuePopContainerInitialize(CellSyncLFQueuePopContainer *container,
                                      void *buffer, const unsigned int tag)
{
    container->buffer = buffer;
    container->pointer = 0;
    container->tag = tag;
}

static inline int
cellSyncLFQueuePushBegin(uint64_t ea, CellSyncLFQueuePushContainer *pContainer)
{
    return _cellSyncLFQueuePushBeginBody(ea, pContainer, 1);
}

static inline int
cellSyncLFQueueTryPushBegin(uint64_t ea, CellSyncLFQueuePushContainer *pContainer)
{
    return _cellSyncLFQueuePushBeginBody(ea, pContainer, 0);
}

static inline int
cellSyncLFQueuePopBegin(uint64_t ea, CellSyncLFQueuePopContainer *pContainer)
{
    return _cellSyncLFQueuePopBeginBody(ea, pContainer, 1);
}

static inline int
cellSyncLFQueueTryPopBegin(uint64_t ea, CellSyncLFQueuePopContainer *pContainer)
{
    return _cellSyncLFQueuePopBeginBody(ea, pContainer, 0);
}

#endif /* _PS3DK_CELL_SYNC_LFQUEUE_H_SPU_ */
