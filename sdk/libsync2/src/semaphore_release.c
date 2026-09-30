/* cellSync2SemaphoreRelease - return count, waking waiters it satisfies. */
#include <cell/sync2/semaphore.h>
#include "sync2_internal.h"

int cellSync2SemaphoreRelease(uint64_t eaSemaphore, unsigned int count, const CellSync2ThreadConfig *config,
                              unsigned int dmaTag)
{
	int rc = s2_check_args(eaSemaphore, config, dmaTag);
	if (rc)
		return rc;
	return __sync2_semaphore_release((uint32_t)eaSemaphore, count, config->callerThreadType,
	                                 config->notifierTable, config->numNotifier, dmaTag);
}
