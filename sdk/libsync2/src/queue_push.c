/* cellSync2QueuePush - append an element, blocking while the queue is full. */
#include <cell/sync2/queue.h>
#include "sync2_internal.h"

int cellSync2QueuePush(uint64_t eaQueue, const void *data, const CellSync2ThreadConfig *config, unsigned int dmaTag)
{
	if (eaQueue == 0 || data == 0 || config == 0)
		return S2_NULL_POINTER;
	if ((eaQueue & 0x7f) || ((uintptr_t)data & 15))
		return S2_ALIGN;
	if (dmaTag >= 32)
		return S2_INVAL;
	return __sync2_queue_push((uint32_t)eaQueue, data, config->callerThreadType, config->notifierTable,
	                          config->numNotifier, 1, dmaTag);
}
