/* cell/sheap/key_sheap_barrier.h -- SPU keyed shared barrier.
 *
 * A CellSyncBarrier for `count` participants at the start of a 128-byte
 * block of a keyed heap; the inline operations forward to libsync.
 */
#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_BARRIER_H_SPU__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_BARRIER_H_SPU__

#include <cell/sheap/key_sheap.h>
#include <cell/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapBarrierNew(CellKeySheapBarrier *obj, uint64_t ea_ksheap,
                           CellSheapKey key, uint16_t count);
void cellKeySheapBarrierDelete(CellKeySheapBarrier *obj);

static inline int cellKeySheapBarrierNotify(CellKeySheapBarrier *obj)
{
    return cellSyncBarrierNotify(obj->ea);
}

static inline int cellKeySheapBarrierTryNotify(CellKeySheapBarrier *obj)
{
    return cellSyncBarrierTryNotify(obj->ea);
}

static inline int cellKeySheapBarrierWait(CellKeySheapBarrier *obj)
{
    return cellSyncBarrierWait(obj->ea);
}

static inline int cellKeySheapBarrierTryWait(CellKeySheapBarrier *obj)
{
    return cellSyncBarrierTryWait(obj->ea);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_BARRIER_H_SPU__ */
