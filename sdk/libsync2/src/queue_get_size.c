/* cellSync2QueueGetSize - the number of elements in the queue. */
#include <cell/sync2/queue.h>
#include "sync2_internal.h"

int cellSync2QueueGetSize(uint64_t eaQueue, unsigned int *size)
{
	if (eaQueue == 0 || size == 0)
		return S2_NULL_POINTER;
	if (eaQueue & 0x7f)
		return S2_ALIGN;
	s2_getllar(__sync2_line, (uint32_t)eaQueue);
	*size = *(volatile uint32_t *)(__sync2_line + 0x20);
	return 0;
}
