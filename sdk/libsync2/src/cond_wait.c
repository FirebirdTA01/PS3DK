/* cellSync2CondWait - release the condition's mutex, block until signalled,
 * then take the mutex back. */
#include <cell/sync2/cond.h>
#include "sync2_internal.h"

int cellSync2CondWait(uint64_t eaCondition, const CellSync2ThreadConfig *config, unsigned int dmaTag)
{
	int rc = s2_check_args(eaCondition, config, dmaTag);
	if (rc)
		return rc;
	return __sync2_cond_wait((uint32_t)eaCondition, config->callerThreadType, config->notifierTable,
	                         config->numNotifier, dmaTag);
}
