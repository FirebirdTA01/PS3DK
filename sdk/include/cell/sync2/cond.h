/* cell/sync2/cond.h - PPU cond API (the libsync2 module, link -lsync2_stub).
 */
#ifndef __PS3DK_CELL_SYNC2_COND_H__
#define __PS3DK_CELL_SYNC2_COND_H__

#include <cell/sync2/cond_types.h>
#include <cell/sync2/thread_types.h>
#include <cell/sync2/version.h>
#include <cell/sync2/mutex_types.h>

#ifdef __cplusplus
extern "C" {
#endif

int _cellSync2CondAttributeInitialize(CellSync2CondAttribute *attr,
                                      uint32_t sdkVersion);
static inline int cellSync2CondAttributeInitialize(CellSync2CondAttribute *attr) {
	return _cellSync2CondAttributeInitialize(attr, _CELL_SYNC2_INTERNAL_VERSION);
}
int cellSync2CondEstimateBufferSize(const CellSync2CondAttribute *attr,
                                    size_t *bufferSize);
int cellSync2CondInitialize        (CellSync2Cond *cond, CellSync2Mutex *mutex,
                                    void *waitingQueueBuffer,
                                    const CellSync2CondAttribute *attr);
int cellSync2CondFinalize          (CellSync2Cond *cond);
int cellSync2CondWait              (CellSync2Cond *cond,
                                    const CellSync2ThreadConfig *config);
int cellSync2CondSignal            (CellSync2Cond *cond,
                                    const CellSync2ThreadConfig *config);
int cellSync2CondSignalAll         (CellSync2Cond *cond,
                                    const CellSync2ThreadConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYNC2_COND_H__ */
