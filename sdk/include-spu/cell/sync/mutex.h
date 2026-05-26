/*
 * cell/sync/mutex.h -- SPU-side sync mutex declarations.
 *
 * Declares the cellSyncMutex* entry points.  Implementations live
 * in libsync.a.
 */
#ifndef _PS3DK_CELL_SYNC_MUTEX_H_SPU_
#define _PS3DK_CELL_SYNC_MUTEX_H_SPU_

#include <stdint.h>
#include <cell/sync/error.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellSyncMutexInitialize(uint64_t ea_obj, unsigned int tag);
int cellSyncMutexLock(uint64_t ea);
int cellSyncMutexTryLock(uint64_t ea);
int cellSyncMutexUnlock(uint64_t ea);

#ifdef __cplusplus
}
#endif

#endif /* _PS3DK_CELL_SYNC_MUTEX_H_SPU_ */
