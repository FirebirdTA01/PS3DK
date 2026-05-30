#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_MUTEX_H__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_MUTEX_H__

#include <cell/sheap/key_sheap.h>
#include <cell/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapMutexNew(CellKeySheapMutex *obj, void *ksheap, CellSheapKey key);
void cellKeySheapMutexDelete(CellKeySheapMutex *obj);

#ifdef __cplusplus
}
#endif

static inline int cellKeySheapMutexTryLock(CellKeySheapMutex *obj) { return cellSyncMutexTryLock((CellSyncMutex *)(uintptr_t)obj->ea); }
static inline int cellKeySheapMutexLock(CellKeySheapMutex *obj) { return cellSyncMutexLock((CellSyncMutex *)(uintptr_t)obj->ea); }
static inline int cellKeySheapMutexUnlock(CellKeySheapMutex *obj) { return cellSyncMutexUnlock((CellSyncMutex *)(uintptr_t)obj->ea); }

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_MUTEX_H__ */
