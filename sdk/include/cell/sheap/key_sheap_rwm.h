#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_RWM_H__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_RWM_H__

#include <cell/sheap/key_sheap.h>
#include <cell/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapRwmNew(CellKeySheapRwm *obj, void *ksheap, CellSheapKey key, uint32_t buffer_size);
void cellKeySheapRwmDelete(CellKeySheapRwm *obj);

#ifdef __cplusplus
}
#endif

static inline int cellKeySheapRwmRead(CellKeySheapRwm *obj, void *buffer) { return cellSyncRwmRead((CellSyncRwm *)(uintptr_t)obj->ea, buffer); }
static inline int cellKeySheapRwmTryRead(CellKeySheapRwm *obj, void *buffer) { return cellSyncRwmTryRead((CellSyncRwm *)(uintptr_t)obj->ea, buffer); }
static inline int cellKeySheapRwmWrite(CellKeySheapRwm *obj, void *buffer) { return cellSyncRwmWrite((CellSyncRwm *)(uintptr_t)obj->ea, buffer); }
static inline int cellKeySheapRwmTryWrite(CellKeySheapRwm *obj, void *buffer) { return cellSyncRwmTryWrite((CellSyncRwm *)(uintptr_t)obj->ea, buffer); }

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_RWM_H__ */
