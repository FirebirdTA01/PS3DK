/* cond.c - the condition variable line: wait, signal, signal all.
 *
 * Condition line (128 bytes):
 *   +0x00 waiting queue header (sync2_internal.h)
 *   +0x14 u16 waiters signalled      +0x16 u16 waiters queued
 *   +0x18 u16 maxWaiters             +0x1a u16 thread types
 *   +0x24 u32 EA of the mutex the condition is used with
 * A waiter queues itself, releases every level of the mutex, blocks, and
 * relocks the mutex to the same depth once signalled.
 */
#include "sync2_internal.h"

typedef struct s2_cond {
	s2_queue_header queue;
	uint32_t pad10;
	uint16_t signalled;
	uint16_t waiters;
	uint16_t max_waiters;
	uint16_t thread_types;
	uint32_t pad1c;
	uint32_t pad20;
	uint32_t mutex;
} s2_cond;

_Static_assert(__builtin_offsetof(s2_cond, signalled) == 0x14, "cond signalled");
_Static_assert(__builtin_offsetof(s2_cond, thread_types) == 0x1a, "cond thread types");
_Static_assert(__builtin_offsetof(s2_cond, mutex) == 0x24, "cond mutex");

/* The checks every cond call makes on the caller and the notifiers. */
static int check_caller(volatile s2_cond *c, const CellSync2CallerThreadType *caller,
                        CellSync2Notifier *const *notifiers, unsigned int numNotifier, int wait)
{
	if (!caller)
		return S2_NULL_POINTER;
	if (caller->threadTypeId == 0)
		return S2_INVAL;
	if ((c->thread_types & caller->threadTypeId) == 0)
		return S2_NOT_SUPPORTED_THREAD;
	if (wait && (!caller->allocateSignalReceiver || !caller->freeSignalReceiver || !caller->waitSignal))
		return S2_PERM;
	if (!notifiers)
		return S2_NULL_POINTER;
	if (numNotifier == 0)
		return S2_INVAL;
	uint32_t uncovered = c->thread_types & 0xff;
	for (unsigned int k = 0; k < numNotifier; k++) {
		const CellSync2Notifier *n = notifiers[k];
		if (n->threadTypeId == 0)
			return S2_INVAL;
		if (!n->sendSignal)
			return S2_NULL_POINTER;
		uncovered &= ~(uint32_t)n->threadTypeId;
	}
	return uncovered ? S2_NO_NOTIFIER : 0;
}

/* Wake the first waiter not yet signalled; AGAIN when there is none. */
static int wake_one(uint32_t ea, CellSync2Notifier *const *notifiers, unsigned int numNotifier, unsigned int dmaTag)
{
	volatile s2_cond *c = (volatile s2_cond *)__sync2_line;
	do {
		s2_getllar(c, ea);
		if (c->signalled == c->waiters)
			return S2_AGAIN;
		c->signalled += 1;
	} while (!s2_putllc(c, ea));
	return __sync2_queue_wakeup(ea, notifiers, numNotifier, dmaTag);
}

int __sync2_cond_wait(uint32_t ea, const CellSync2CallerThreadType *caller, CellSync2Notifier *const *notifiers,
                      unsigned int numNotifier, unsigned int dmaTag)
{
	volatile s2_cond *c = (volatile s2_cond *)__sync2_line;
	volatile const uint8_t *mline = __sync2_line;
	CellSync2SignalReceiverId receiver;
	uint32_t mutex, depth;
	int rc;

	s2_getllar(c, ea);
	if ((rc = check_caller(c, caller, notifiers, numNotifier, 1)))
		return rc;

	/* the caller must own the condition's mutex */
	mutex = c->mutex;
	s2_getllar(__sync2_line, mutex);
	if (!(mline[0x20] & 0x80))
		return S2_STAT;
	if (*(volatile uint16_t *)(mline + 0x10) != caller->threadTypeId
	    || *(volatile uint64_t *)(mline + 0x18) != caller->self(caller->callbackArg))
		return S2_PERM;

	if ((rc = caller->allocateSignalReceiver(&receiver, CELL_SYNC2_OBJECT_TYPE_COND, ea, caller->callbackArg)))
		return rc;
	for (;;) {
		s2_getllar(c, ea);
		if (c->waiters == c->max_waiters) {
			if (caller->freeSignalReceiver(receiver, caller->callbackArg))
				s2_halt();
			return S2_NOMEM;
		}
		c->waiters += 1;
		if (s2_putllc(c, ea))
			break;
	}

	if (__sync2_mutex_unlock(mutex, caller, notifiers, numNotifier, &depth, dmaTag))
		s2_halt();
	if (__sync2_queue_enter(ea, caller->threadTypeId, receiver, caller->waitSignal, CELL_SYNC2_OBJECT_TYPE_COND,
	                        ea, caller->callbackArg, dmaTag))
		s2_halt();
	do {
		s2_getllar(c, ea);
		c->signalled -= 1;
		c->waiters -= 1;
	} while (!s2_putllc(c, ea));
	if (caller->freeSignalReceiver(receiver, caller->callbackArg))
		s2_halt();
	if (__sync2_mutex_lock(mutex, 1, caller, depth, dmaTag))
		s2_halt();
	return 0;
}

int __sync2_cond_signal(uint32_t ea, int all, const CellSync2CallerThreadType *caller,
                        CellSync2Notifier *const *notifiers, unsigned int numNotifier, unsigned int dmaTag)
{
	volatile s2_cond *c = (volatile s2_cond *)__sync2_line;
	int rc;

	s2_getllar(c, ea);
	if ((rc = check_caller(c, caller, notifiers, numNotifier, 0)))
		return rc;
	do
		rc = wake_one(ea, notifiers, numNotifier, dmaTag);
	while (all && rc == 0);
	return rc == S2_AGAIN ? 0 : rc;
}
