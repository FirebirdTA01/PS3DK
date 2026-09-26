/*
 * cell/sync/queue.h -- SPU-side sync queue declarations.
 *
 * Declares the cellSyncQueue* entry points, as in the SPU sync umbrella.
 * Push, Pop and Peek (and their Try forms) move the element with the
 * caller's DMA tag.  Implementations live in libsync.a (SPU runtime).
 */
#ifndef _PS3DK_CELL_SYNC_QUEUE_H_SPU_
#define _PS3DK_CELL_SYNC_QUEUE_H_SPU_

#include <stdint.h>
#include <cell/sync/error.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellSyncQueueInitialize(uint64_t ea, uint64_t ptr_buffer,
                            uint32_t buffer_size, unsigned int depth,
                            unsigned int tag);
int          cellSyncQueuePush    (uint64_t ea, const void *buf,
                                   unsigned int tag);
int          cellSyncQueueTryPush (uint64_t ea, const void *buf,
                                   unsigned int tag);
int          cellSyncQueuePop     (uint64_t ea, void *buf,
                                   unsigned int tag);
int          cellSyncQueueTryPop  (uint64_t ea, void *buf,
                                   unsigned int tag);
int          cellSyncQueuePeek    (uint64_t ea, void *buf,
                                   unsigned int tag);
int          cellSyncQueueTryPeek (uint64_t ea, void *buf,
                                   unsigned int tag);
int          cellSyncQueueClear   (uint64_t ea);
unsigned int cellSyncQueueSize    (uint64_t ea);

#ifdef __cplusplus
}
#endif

#endif /* _PS3DK_CELL_SYNC_QUEUE_H_SPU_ */
