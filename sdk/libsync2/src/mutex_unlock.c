/* cellSync2MutexUnlock - release one level of ownership; the last one hands
 * the mutex to the first waiter, if any. */
#include <cell/sync2/mutex.h>
#include "sync2_internal.h"

int cellSync2MutexUnlock(uint64_t eaMutex, const CellSync2ThreadConfig *config, unsigned int dmaTag)
{
	int rc = s2_check_args(eaMutex, config, dmaTag);
	if (rc)
		return rc;
	return __sync2_mutex_unlock((uint32_t)eaMutex, config->callerThreadType, config->notifierTable,
	                            config->numNotifier, 0, dmaTag);
}
