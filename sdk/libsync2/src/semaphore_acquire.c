/* cellSync2SemaphoreAcquire - take count, blocking the caller until it is available. */
#include <cell/sync2/semaphore.h>
#include "sync2_internal.h"

int cellSync2SemaphoreAcquire(uint64_t eaSemaphore, unsigned int count, const CellSync2ThreadConfig *config, unsigned int dmaTag)
{
	int rc = s2_check_args(eaSemaphore, config, dmaTag);
	if (rc)
		return rc;
	return __sync2_semaphore_acquire((uint32_t)eaSemaphore, 1, count, config->callerThreadType, dmaTag);
}
