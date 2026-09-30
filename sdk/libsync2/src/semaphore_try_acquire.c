/* cellSync2SemaphoreTryAcquire - take count, or return CELL_SYNC2_ERROR_AGAIN at once. */
#include <cell/sync2/semaphore.h>
#include "sync2_internal.h"

int cellSync2SemaphoreTryAcquire(uint64_t eaSemaphore, unsigned int count, const CellSync2ThreadConfig *config, unsigned int dmaTag)
{
	int rc = s2_check_args(eaSemaphore, config, dmaTag);
	if (rc)
		return rc;
	return __sync2_semaphore_acquire((uint32_t)eaSemaphore, 0, count, config->callerThreadType, dmaTag);
}
