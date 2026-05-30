#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_BARRIER_H__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_BARRIER_H__

#include <cell/sheap/key_sheap.h>
#include <cell/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapBarrierNew(CellKeySheapBarrier *obj, void *ksheap, CellSheapKey key, uint16_t total_count);
void cellKeySheapBarrierDelete(CellKeySheapBarrier *obj);

#ifdef __cplusplus
}
#endif

static inline int cellKeySheapBarrierNotify(CellKeySheapBarrier *obj) { return cellSyncBarrierNotify((CellSyncBarrier *)(uintptr_t)obj->ea); }
static inline int cellKeySheapBarrierTryNotify(CellKeySheapBarrier *obj) { return cellSyncBarrierTryNotify((CellSyncBarrier *)(uintptr_t)obj->ea); }
static inline int cellKeySheapBarrierWait(CellKeySheapBarrier *obj) { return cellSyncBarrierWait((CellSyncBarrier *)(uintptr_t)obj->ea); }
static inline int cellKeySheapBarrierTryWait(CellKeySheapBarrier *obj) { return cellSyncBarrierTryWait((CellSyncBarrier *)(uintptr_t)obj->ea); }

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_BARRIER_H__ */
