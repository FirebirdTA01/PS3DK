/* cell/sync/queue.h -- SPU-side sync queue declarations.
 *
 * Declares the cellSyncQueue* entry points.  Implementations live in
 * the SPU runtime libraries (not yet shipped -- link will produce
 * honest undefined-reference errors until the runtime is filled in).
 */
#ifndef __CELL_SYNC_QUEUE_H_SPU__
#define __CELL_SYNC_QUEUE_H_SPU__

#include <stdint.h>
#include <cell/sync/error.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellSyncQueueInitialize(uint64_t ea, uint64_t ptr_buffer,
                            uint32_t buffer_size, unsigned int depth,
                            unsigned int tag);
int cellSyncQueuePush(uint64_t ptr_obj, const void *buf, unsigned int tag);
int cellSyncQueueTryPush(uint64_t ptr_obj, const void *buf, unsigned int tag);
int cellSyncQueuePop(uint64_t ptr_obj, void *buf, unsigned int tag);
int cellSyncQueueTryPop(uint64_t ptr_obj, void *buf, unsigned int tag);
int cellSyncQueueSize(uint64_t ea);
int cellSyncQueueClear(uint64_t ea);
int cellSyncQueuePeek(uint64_t ptr_obj, void *buf, unsigned int tag);
int cellSyncQueueTryPeek(uint64_t ptr_obj, void *buf, unsigned int tag);

#ifdef __cplusplus
}
#endif

#endif /* __CELL_SYNC_QUEUE_H_SPU__ */
