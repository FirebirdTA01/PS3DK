/* cell/sync/mutex.h -- SPU-side sync mutex declarations.
 *
 * Declares the cellSyncMutex* entry points.  Implementations live in
 * the SPU runtime libraries (not yet shipped -- link will produce
 * honest undefined-reference errors until the runtime is filled in).
 */
#ifndef __CELL_SYNC_MUTEX_H_SPU__
#define __CELL_SYNC_MUTEX_H_SPU__

#include <stdint.h>
#include <cell/sync/error.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellSyncMutexInitialize(uint64_t ea_obj, unsigned int tag);
int cellSyncMutexTryLock(uint64_t ea);
int cellSyncMutexLock(uint64_t ea);
int cellSyncMutexUnlock(uint64_t ea);

#ifdef __cplusplus
}
#endif

#endif /* __CELL_SYNC_MUTEX_H_SPU__ */
