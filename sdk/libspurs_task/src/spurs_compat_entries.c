/* Function forms of SPU SPURS calls the headers spell as macros over a
 * blocking-flag entry point.  Objects built against older headers call
 * these names directly.  This file must not see the macro definitions,
 * so it declares what it forwards to itself.
 * Independently written from the published interfaces. */
#include <stdint.h>

extern int _cellSpursQueuePushBegin(uint64_t ea, const void *buffer, unsigned int tag, unsigned isBlocking);
extern int _cellSpursQueuePopBegin(uint64_t ea, void *buffer, unsigned int tag, unsigned isBlocking);
extern int _cellSpursQueuePopEnd(uint64_t ea, unsigned int tag, unsigned isPeek);
extern int _cellSpursEventFlagWait(uint64_t ea, uint16_t *bits, unsigned mode, unsigned isBlocking);

int cellSpursQueuePushBegin(uint64_t ea, const void *buffer, unsigned int tag)
{
	return _cellSpursQueuePushBegin(ea, buffer, tag, 1);
}

int cellSpursQueueTryPushBegin(uint64_t ea, const void *buffer, unsigned int tag)
{
	return _cellSpursQueuePushBegin(ea, buffer, tag, 0);
}

int cellSpursQueuePopBegin(uint64_t ea, void *buffer, unsigned int tag)
{
	return _cellSpursQueuePopBegin(ea, buffer, tag, 1);
}

int cellSpursQueueTryPopBegin(uint64_t ea, void *buffer, unsigned int tag)
{
	return _cellSpursQueuePopBegin(ea, buffer, tag, 0);
}

int cellSpursQueuePopEndBody(uint64_t ea, unsigned int tag, unsigned isPeek)
{
	return _cellSpursQueuePopEnd(ea, tag, isPeek);
}

int cellSpursEventFlagWait(uint64_t ea, uint16_t *bits, unsigned mode)
{
	return _cellSpursEventFlagWait(ea, bits, mode, 1);
}

int cellSpursEventFlagTryWait(uint64_t ea, uint16_t *bits, unsigned mode)
{
	return _cellSpursEventFlagWait(ea, bits, mode, 0);
}
