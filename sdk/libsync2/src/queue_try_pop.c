/* cellSync2QueueTryPop - take the oldest element, or return CELL_SYNC2_ERROR_AGAIN when empty. */
#include <cell/sync2/queue.h>
#include "sync2_internal.h"

int cellSync2QueueTryPop(uint64_t eaQueue, void *buffer, const CellSync2ThreadConfig *config, unsigned int dmaTag)
{
	if (eaQueue == 0 || buffer == 0 || config == 0)
		return S2_NULL_POINTER;
	if ((eaQueue & 0x7f) || ((uintptr_t)buffer & 15))
		return S2_ALIGN;
	if (dmaTag >= 32)
		return S2_INVAL;
	return __sync2_queue_pop((uint32_t)eaQueue, buffer, config->callerThreadType, config->notifierTable,
	                          config->numNotifier, 0, dmaTag);
}
