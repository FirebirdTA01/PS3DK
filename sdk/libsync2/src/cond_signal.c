/* cellSync2CondSignal - wake one waiter, if any. */
#include <cell/sync2/cond.h>
#include "sync2_internal.h"

int cellSync2CondSignal(uint64_t eaCondition, const CellSync2ThreadConfig *config, unsigned int dmaTag)
{
	int rc = s2_check_args(eaCondition, config, dmaTag);
	if (rc)
		return rc;
	return __sync2_cond_signal((uint32_t)eaCondition, 0, config->callerThreadType, config->notifierTable,
	                           config->numNotifier, dmaTag);
}
