/* cellSync2SemaphoreGetCount - the semaphore's current count. */
#include <cell/sync2/semaphore.h>
#include "sync2_internal.h"

int cellSync2SemaphoreGetCount(uint64_t eaSemaphore, int *pCount)
{
	if (eaSemaphore == 0 || pCount == 0)
		return S2_NULL_POINTER;
	if (eaSemaphore & 0x7f)
		return S2_ALIGN;
	s2_getllar(__sync2_line, (uint32_t)eaSemaphore);
	*pCount = *(volatile int32_t *)(__sync2_line + 0x10);
	return 0;
}
