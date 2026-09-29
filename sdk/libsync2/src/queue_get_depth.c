/* cellSync2QueueGetDepth - how many elements the queue can hold. */
#include <cell/sync2/queue.h>
#include "sync2_internal.h"

int cellSync2QueueGetDepth(uint64_t eaQueue, unsigned int *depth)
{
	if (eaQueue == 0 || depth == 0)
		return S2_NULL_POINTER;
	if (eaQueue & 0x7f)
		return S2_ALIGN;
	s2_getllar(__sync2_line, (uint32_t)eaQueue);
	*depth = *(volatile uint32_t *)(__sync2_line + 0x4c) - 1;
	return 0;
}
