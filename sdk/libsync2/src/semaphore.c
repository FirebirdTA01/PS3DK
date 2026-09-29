/* semaphore.c - the semaphore line and its acquire / release.
 *
 * Semaphore line (128 bytes):
 *   +0x00 waiting queue header (sync2_internal.h)
 *   +0x10 s32 count
 *   +0x14 u16 waiters signalled      +0x16 u16 waiters queued
 *   +0x20 u16 thread types           +0x22 u16 maxWaiters
 * With maxWaiters 1 the word at +0x14 instead holds the one waiter:
 *   bit 31 signalled, bit 30 waiting, bits 29..0 the count it still lacks;
 *   that waiter may ask for any count, and releases fill its deficit first.
 * With more waiters each asks for 1, and a release hands its count
 * straight to queued waiters.
 */
#include "sync2_internal.h"

typedef struct s2_semaphore {
	s2_queue_header queue;
	int32_t  count;
	union {
		struct {
			uint16_t signalled;
			uint16_t waiters;
		};
		uint32_t single;
	};
	uint32_t pad18[2];
	uint16_t thread_types;
	uint16_t max_waiters;
} s2_semaphore;

_Static_assert(__builtin_offsetof(s2_semaphore, count) == 0x10, "semaphore count");
_Static_assert(__builtin_offsetof(s2_semaphore, thread_types) == 0x20, "semaphore thread types");

#define SINGLE_SIGNALLED 0x80000000u
#define SINGLE_WAITING   0x40000000u
#define SINGLE_LACKING   0x3fffffffu
#define COUNT_MAX        0x7fffff

int __sync2_semaphore_acquire(uint32_t ea, int wait, unsigned int n, const CellSync2CallerThreadType *caller,
                              unsigned int dmaTag)
{
	volatile s2_semaphore *s = (volatile s2_semaphore *)__sync2_line;
	CellSync2SignalReceiverId receiver = 0;
	int haveReceiver = 0, queued;

	s2_getllar(s, ea);
	if (!caller)
		return S2_NULL_POINTER;
	if (caller->threadTypeId == 0)
		return S2_INVAL;
	if ((s->thread_types & caller->threadTypeId) == 0)
		return S2_NOT_SUPPORTED_THREAD;
	if (wait && (!caller->allocateSignalReceiver || !caller->freeSignalReceiver || !caller->waitSignal))
		return S2_PERM;
	/* only a lone waiter may block for more than 1 */
	if (wait && n > 1 && s->max_waiters != 1)
		return S2_INVAL;
	if (n > COUNT_MAX)
		return S2_INVAL;
	if (n == 0)
		return 0;

	for (;;) {
		s2_getllar(s, ea);
		if ((int32_t)n <= s->count) {
			s->count -= n;
			queued = 0;
		} else {
			if (!wait)
				return S2_AGAIN;
			if (!haveReceiver) {
				int rc = caller->allocateSignalReceiver(&receiver, CELL_SYNC2_OBJECT_TYPE_SEMAPHORE, ea,
				                                        caller->callbackArg);
				if (rc)
					return rc;
				haveReceiver = 1;
				continue;
			}
			if (s->max_waiters == 1) {
				if (s->single & SINGLE_WAITING)
					return S2_BUSY;
				s->single = (s->single & SINGLE_SIGNALLED) | SINGLE_WAITING | ((n - s->count) & SINGLE_LACKING);
				s->count = 0;
			} else {
				if (s->waiters == s->max_waiters)
					return S2_NOMEM;
				s->waiters += 1;
			}
			queued = 1;
		}
		if (s2_putllc(s, ea))
			break;
	}

	if (queued) {
		if (!haveReceiver)
			s2_halt();
		if (__sync2_queue_enter(ea, caller->threadTypeId, receiver, caller->waitSignal,
		                        CELL_SYNC2_OBJECT_TYPE_SEMAPHORE, ea, caller->callbackArg, dmaTag))
			s2_halt();
		do {
			s2_getllar(s, ea);
			if (s->max_waiters == 1) {
				s->single &= ~(SINGLE_SIGNALLED | SINGLE_WAITING);
			} else {
				s->signalled -= 1;
				s->waiters -= 1;
			}
		} while (!s2_putllc(s, ea));
	}
	if (haveReceiver && caller->freeSignalReceiver(receiver, caller->callbackArg))
		s2_halt();
	return 0;
}

int __sync2_semaphore_release(uint32_t ea, unsigned int n, const CellSync2CallerThreadType *caller,
                              CellSync2Notifier *const *notifiers, unsigned int numNotifier, unsigned int dmaTag)
{
	volatile s2_semaphore *s = (volatile s2_semaphore *)__sync2_line;
	int32_t wake;

	s2_getllar(s, ea);
	if (!caller)
		return S2_NULL_POINTER;
	if (caller->threadTypeId == 0)
		return S2_INVAL;
	if ((s->thread_types & caller->threadTypeId) == 0)
		return S2_NOT_SUPPORTED_THREAD;
	if (!notifiers)
		return S2_NULL_POINTER;
	if (numNotifier == 0)
		return S2_INVAL;
	uint32_t uncovered = s->thread_types & 0xff;
	for (unsigned int k = 0; k < numNotifier; k++) {
		const CellSync2Notifier *nt = notifiers[k];
		if (nt->threadTypeId == 0)
			return S2_INVAL;
		if (!nt->sendSignal)
			return S2_NULL_POINTER;
		uncovered &= ~(uint32_t)nt->threadTypeId;
	}
	if (uncovered)
		return S2_NO_NOTIFIER;
	if (n > COUNT_MAX)
		return S2_INVAL;
	if (n == 0)
		return 0;

	for (;;) {
		s2_getllar(s, ea);
		int32_t count = s->count + (int32_t)n;
		if (count > COUNT_MAX)
			return S2_STAT;
		s->count = count;
		wake = 0;
		if (count > 0) {
			if (s->max_waiters == 1) {
				uint32_t single = s->single;
				if ((single & (SINGLE_SIGNALLED | SINGLE_WAITING)) == SINGLE_WAITING) {
					int32_t lacking = single & SINGLE_LACKING;
					if (lacking == 0)
						s2_halt();
					if (lacking > count) {
						s->single = (single & ~SINGLE_LACKING) | (lacking - count);
						s->count = 0;
					} else {
						s->count = count - lacking;
						s->single = (single & ~SINGLE_LACKING) | SINGLE_SIGNALLED;
						wake = 1;
					}
				}
			} else {
				int32_t pending = (int32_t)s->waiters - (int32_t)s->signalled;
				if (pending > 0) {
					wake = count > pending ? pending : count;
					s->signalled += wake;
					if (count - wake < 0)
						s2_halt();
					s->count = count - wake;
				}
			}
		}
		if (s2_putllc(s, ea))
			break;
	}

	for (int32_t k = 0; k < wake; k++)
		if (__sync2_queue_wakeup(ea, notifiers, numNotifier, dmaTag))
			s2_halt();
	return 0;
}
