/* cell/sync2/semaphore.h - SPU semaphore API (libsync2.a).
 *
 * The object is named by its effective address; dmaTag is the MFC tag the
 * call may use (0..31).
 */
#ifndef __PS3DK_CELL_SYNC2_SEMAPHORE_H_SPU__
#define __PS3DK_CELL_SYNC2_SEMAPHORE_H_SPU__

#include <stdint.h>
#include <cell/sync2/semaphore_types.h>
#include <cell/sync2/thread_types.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellSync2SemaphoreAcquire(uint64_t eaSemaphore, unsigned int count, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2SemaphoreTryAcquire(uint64_t eaSemaphore, unsigned int count, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2SemaphoreRelease(uint64_t eaSemaphore, unsigned int count, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2SemaphoreGetCount(uint64_t eaSemaphore, int *pCount);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYNC2_SEMAPHORE_H_SPU__ */
