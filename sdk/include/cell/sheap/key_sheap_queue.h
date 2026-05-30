#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_QUEUE_H__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_QUEUE_H__

#include <cell/sheap/key_sheap.h>
#include <cell/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapQueueNew(CellKeySheapQueue *obj, void *ksheap, CellSheapKey key, uint32_t buffer_size, uint32_t depth);
void cellKeySheapQueueDelete(CellKeySheapQueue *obj);

#ifdef __cplusplus
}
#endif

static inline int cellKeySheapQueuePush(CellKeySheapQueue *obj, const void *buf) { return cellSyncQueuePush((CellSyncQueue *)(uintptr_t)obj->ea, buf); }
static inline int cellKeySheapQueueTryPush(CellKeySheapQueue *obj, const void *buf) { return cellSyncQueueTryPush((CellSyncQueue *)(uintptr_t)obj->ea, buf); }
static inline int cellKeySheapQueuePop(CellKeySheapQueue *obj, void *buf) { return cellSyncQueuePop((CellSyncQueue *)(uintptr_t)obj->ea, buf); }
static inline int cellKeySheapQueueTryPop(CellKeySheapQueue *obj, void *buf) { return cellSyncQueueTryPop((CellSyncQueue *)(uintptr_t)obj->ea, buf); }
static inline int cellKeySheapQueuePeek(CellKeySheapQueue *obj, void *buf) { return cellSyncQueuePeek((CellSyncQueue *)(uintptr_t)obj->ea, buf); }
static inline int cellKeySheapQueueTryPeek(CellKeySheapQueue *obj, void *buf) { return cellSyncQueueTryPeek((CellSyncQueue *)(uintptr_t)obj->ea, buf); }
static inline int cellKeySheapQueueClear(CellKeySheapQueue *obj) { return cellSyncQueueClear((CellSyncQueue *)(uintptr_t)obj->ea); }
static inline unsigned int cellKeySheapQueueSize(CellKeySheapQueue *obj) { return cellSyncQueueSize((CellSyncQueue *)(uintptr_t)obj->ea); }

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_QUEUE_H__ */
