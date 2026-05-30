#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_SEMAPHORE_H__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_SEMAPHORE_H__

#include <cell/atomic.h>
#include <cell/error.h>
#include <cell/sheap/key_sheap.h>
#include <cell/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapSemaphoreNew(CellKeySheapSemaphore *obj, void *ksheap, CellSheapKey key, int count);
void cellKeySheapSemaphoreDelete(CellKeySheapSemaphore *obj);

#ifdef __cplusplus
}
#endif

static inline int cellKeySheapSemaphoreTryP(CellKeySheapSemaphore *obj)
{
    if (*(int *)(uintptr_t)obj->ea <= 0) return CELL_SYNC_ERROR_BUSY;
    return cellAtomicTestAndDecr32((uint32_t *)(uintptr_t)obj->ea) == 0 ? CELL_SYNC_ERROR_BUSY : CELL_OK;
}

static inline int cellKeySheapSemaphoreP(CellKeySheapSemaphore *obj)
{
    uint32_t old;
    do {
        old = cellAtomicTestAndDecr32((uint32_t *)(uintptr_t)obj->ea);
    } while (old == 0);
    return CELL_OK;
}

static inline int cellKeySheapSemaphoreV(CellKeySheapSemaphore *obj)
{
    cellAtomicIncr32((uint32_t *)(uintptr_t)obj->ea);
    return CELL_OK;
}

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_SEMAPHORE_H__ */
