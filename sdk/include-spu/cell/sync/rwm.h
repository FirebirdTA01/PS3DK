/* cell/sync/rwm.h -- SPU-side read-write mutex declarations.
 *
 * Declares the cellSyncRwm* entry points.  Implementations live in
 * the SPU runtime libraries (not yet shipped -- link will produce
 * honest undefined-reference errors until the runtime is filled in).
 */
#ifndef __CELL_SYNC_RWM_H_SPU__
#define __CELL_SYNC_RWM_H_SPU__

#include <stdint.h>
#include <cell/sync/error.h>
#include <cell/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellSyncRwmInitialize(uint64_t ea, uint64_t ptr_buffer,
                          uint32_t buffer_size, unsigned int tag);
int cellSyncRwmReadBegin(uint64_t ea, void *buffer, unsigned int tag);
int cellSyncRwmTryReadBegin(uint64_t ea, void *buffer, unsigned int tag);
int cellSyncRwmReadEnd(uint64_t ea, unsigned int tag);
int cellSyncRwmWrite(uint64_t ea, void *buffer, unsigned int tag);
int cellSyncRwmTryWrite(uint64_t ea, void *buffer, unsigned int tag);

#ifdef __cplusplus
}
#endif

#endif /* __CELL_SYNC_RWM_H_SPU__ */
