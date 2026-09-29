/* cell/sync2/mutex.h - SPU mutex API (libsync2.a).
 *
 * The object is named by its effective address; dmaTag is the MFC tag the
 * call may use (0..31).
 */
#ifndef __PS3DK_CELL_SYNC2_MUTEX_H_SPU__
#define __PS3DK_CELL_SYNC2_MUTEX_H_SPU__

#include <stdint.h>
#include <cell/sync2/mutex_types.h>
#include <cell/sync2/thread_types.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellSync2MutexLock(uint64_t eaMutex, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2MutexTryLock(uint64_t eaMutex, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2MutexUnlock(uint64_t eaMutex, const CellSync2ThreadConfig *config, unsigned int dmaTag);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYNC2_MUTEX_H_SPU__ */
