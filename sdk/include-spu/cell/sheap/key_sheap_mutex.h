/* cell/sheap/key_sheap_mutex.h -- SPU keyed shared mutex.
 *
 * A CellSyncMutex at the start of a 128-byte block of a keyed heap; the
 * inline operations forward to libsync on the object's address.
 */
#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_MUTEX_H_SPU__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_MUTEX_H_SPU__

#include <cell/sheap/key_sheap.h>
#include <cell/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapMutexNew(CellKeySheapMutex *obj, uint64_t ea_ksheap,
                         CellSheapKey key);
void cellKeySheapMutexDelete(CellKeySheapMutex *obj);

static inline int cellKeySheapMutexLock(CellKeySheapMutex *obj)
{
    return cellSyncMutexLock(obj->ea);
}

static inline int cellKeySheapMutexTryLock(CellKeySheapMutex *obj)
{
    return cellSyncMutexTryLock(obj->ea);
}

static inline int cellKeySheapMutexUnlock(CellKeySheapMutex *obj)
{
    return cellSyncMutexUnlock(obj->ea);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_MUTEX_H_SPU__ */
