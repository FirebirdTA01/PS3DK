/* cell/sync2/queue.h - SPU queue API (libsync2.a).
 *
 * The object is named by its effective address; dmaTag is the MFC tag the
 * call may use (0..31).
 */
#ifndef __PS3DK_CELL_SYNC2_QUEUE_H_SPU__
#define __PS3DK_CELL_SYNC2_QUEUE_H_SPU__

#include <stdint.h>
#include <cell/sync2/queue_types.h>
#include <cell/sync2/thread_types.h>
#include <stddef.h>
#include <cell/sync2/thread.h>
#include <cell/sync2/error.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellSync2QueuePush(uint64_t eaQueue, const void *data, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2QueueTryPush(uint64_t eaQueue, const void *data, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2QueuePop(uint64_t eaQueue, void *buffer, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2QueueTryPop(uint64_t eaQueue, void *buffer, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2QueueGetSize(uint64_t eaQueue, unsigned int *size);
int cellSync2QueueGetDepth(uint64_t eaQueue, unsigned int *depth);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYNC2_QUEUE_H_SPU__ */
