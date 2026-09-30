/* cellSync2MutexLock - lock, blocking the caller while another thread owns the mutex. */
#include <cell/sync2/mutex.h>
#include "sync2_internal.h"

int cellSync2MutexLock(uint64_t eaMutex, const CellSync2ThreadConfig *config, unsigned int dmaTag)
{
	int rc = s2_check_args(eaMutex, config, dmaTag);
	if (rc)
		return rc;
	return __sync2_mutex_lock((uint32_t)eaMutex, 1, config->callerThreadType, 1, dmaTag);
}
