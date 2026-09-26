/* cell/sheap/key_sheap_queue.h -- SPU keyed shared queue.
 *
 * A CellSyncQueue of `depth` elements of `buffer_size` bytes: the queue
 * header at the start of the block and its ring 128 bytes in.  The inline
 * operations forward to libsync; element transfers use the caller's tag.
 */
#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_QUEUE_H_SPU__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_QUEUE_H_SPU__

#include <cell/sheap/key_sheap.h>
#include <cell/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapQueueNew(CellKeySheapQueue *obj, uint64_t ea_ksheap,
                         CellSheapKey key, uint32_t buffer_size,
                         uint32_t depth);
void cellKeySheapQueueDelete(CellKeySheapQueue *obj);

static inline int cellKeySheapQueuePush(CellKeySheapQueue *obj,
                                        const void *buf, unsigned int tag)
{
    return cellSyncQueuePush(obj->ea, buf, tag);
}

static inline int cellKeySheapQueueTryPush(CellKeySheapQueue *obj,
                                           const void *buf, unsigned int tag)
{
    return cellSyncQueueTryPush(obj->ea, buf, tag);
}

static inline int cellKeySheapQueuePop(CellKeySheapQueue *obj, void *buf,
                                       unsigned int tag)
{
    return cellSyncQueuePop(obj->ea, buf, tag);
}

static inline int cellKeySheapQueueTryPop(CellKeySheapQueue *obj, void *buf,
                                          unsigned int tag)
{
    return cellSyncQueueTryPop(obj->ea, buf, tag);
}

static inline int cellKeySheapQueuePeek(CellKeySheapQueue *obj, void *buf,
                                        unsigned int tag)
{
    return cellSyncQueuePeek(obj->ea, buf, tag);
}

static inline int cellKeySheapQueueTryPeek(CellKeySheapQueue *obj, void *buf,
                                           unsigned int tag)
{
    return cellSyncQueueTryPeek(obj->ea, buf, tag);
}

static inline unsigned int cellKeySheapQueueSize(CellKeySheapQueue *obj)
{
    return cellSyncQueueSize(obj->ea);
}

static inline int cellKeySheapQueueClear(CellKeySheapQueue *obj)
{
    return cellSyncQueueClear(obj->ea);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_QUEUE_H_SPU__ */
