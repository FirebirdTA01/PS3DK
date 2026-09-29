/* mutex.c - the mutex line and its lock / unlock.
 *
 * Mutex line (128 bytes):
 *   +0x00 waiting queue header (sync2_internal.h)
 *   +0x10 u16 owner thread type id   +0x14 u32 lock count
 *   +0x18 u64 owner thread id
 *   +0x20 u8  bit 7 held             +0x22 u16 waiters signalled
 *   +0x24 u16 waiters queued
 *   +0x28 u32 sdkVersion  +0x2c u32 recursive  +0x30 u16 thread types
 *   +0x32 u16 maxWaiters  +0x34 name
 * An unlock with waiters queued keeps the mutex held and hands it to the
 * first of them, who then writes the owner fields (+0x10..0x1f).
 */
#include "sync2_internal.h"

typedef struct s2_mutex {
	s2_queue_header queue;
	uint16_t owner_type;
	uint16_t pad12;
	uint32_t count;
	uint64_t owner;
	uint8_t  flags;
	uint8_t  pad21;
	uint16_t signalled;
	uint16_t waiters;
	uint16_t pad26;
	uint32_t sdk_version;
	uint32_t recursive;
	uint16_t thread_types;
	uint16_t max_waiters;
} s2_mutex;

_Static_assert(__builtin_offsetof(s2_mutex, owner_type) == 0x10, "mutex owner");
_Static_assert(__builtin_offsetof(s2_mutex, owner) == 0x18, "mutex owner id");
_Static_assert(__builtin_offsetof(s2_mutex, waiters) == 0x24, "mutex waiters");
_Static_assert(__builtin_offsetof(s2_mutex, max_waiters) == 0x32, "mutex maxWaiters");

#define HELD 0x80

static struct {
	uint16_t owner_type;
	uint16_t pad;
	uint32_t count;
	uint64_t owner;
} s_owner __attribute__((aligned(16)));

int __sync2_mutex_lock(uint32_t eaMutex, int wait, const CellSync2CallerThreadType *caller,
                       uint32_t count, unsigned int dmaTag)
{
	volatile s2_mutex *m = (volatile s2_mutex *)__sync2_line;
	const uint16_t type = caller->threadTypeId;
	const uint32_t spin = (uint32_t)(((uint64_t)caller->spinWaitNanoSec * 1307) >> 14);
	const uint32_t start = spu_readch(SPU_RdDec);
	CellSync2SignalReceiverId receiver = 0;
	int first = 1, haveReceiver = 0;

	for (;;) {
		s2_getllar(m, eaMutex);
		if (first) {
			first = 0;
			if (type == 0)
				return S2_INVAL;
			if ((m->thread_types & type) == 0)
				return S2_NOT_SUPPORTED_THREAD;
			if (wait) {
				if (!caller->allocateSignalReceiver || !caller->freeSignalReceiver || !caller->waitSignal)
					return S2_PERM;
				if (m->max_waiters == 0)
					return S2_NOMEM;
			}
		}

		if (!(m->flags & HELD)) {
			m->flags |= HELD;
			if (s2_putllc(m, eaMutex))
				break;
			continue;
		}

		if (m->owner_type == type && m->owner == caller->self(caller->callbackArg)) {
			if (!(m->recursive & 1))
				return S2_DEADLK;
			if (m->count == 0xffffffffu)
				return S2_AGAIN;
			count = m->count + 1;
			break;
		}
		if (!wait)
			return S2_BUSY;
		if (m->waiters == m->max_waiters)
			return S2_NOMEM;

		if (!haveReceiver) {
			if ((uint32_t)(start - spu_readch(SPU_RdDec)) < spin)
				continue;
			int rc = caller->allocateSignalReceiver(&receiver, CELL_SYNC2_OBJECT_TYPE_MUTUEX,
			                                        eaMutex, caller->callbackArg);
			if (rc)
				return rc;
			haveReceiver = 1;
			continue;
		}

		m->waiters += 1;
		if (!s2_putllc(m, eaMutex))
			continue;
		if (__sync2_queue_enter(eaMutex, type, receiver, caller->waitSignal, CELL_SYNC2_OBJECT_TYPE_MUTUEX,
		                        eaMutex, caller->callbackArg, dmaTag))
			s2_halt();
		/* woken: the unlocker kept the mutex held for us */
		do {
			s2_getllar(m, eaMutex);
			m->signalled -= 1;
			m->waiters -= 1;
		} while (!s2_putllc(m, eaMutex));
		break;
	}

	if (haveReceiver && caller->freeSignalReceiver(receiver, caller->callbackArg))
		s2_halt();
	s_owner.owner_type = type;
	s_owner.pad = 0;
	s_owner.count = count;
	s_owner.owner = caller->self(caller->callbackArg);
	s2_put(&s_owner, eaMutex + 0x10, 16, dmaTag);
	return 0;
}

int __sync2_mutex_unlock(uint32_t eaMutex, const CellSync2CallerThreadType *caller,
                         CellSync2Notifier *const *notifiers, unsigned int numNotifier,
                         uint32_t *countOut, unsigned int dmaTag)
{
	volatile s2_mutex *m = (volatile s2_mutex *)__sync2_line;
	int checked = 0, handOver;

	for (;;) {
		s2_getllar(m, eaMutex);
		if (!checked) {
			if (!caller)
				return S2_NULL_POINTER;
			if (caller->threadTypeId == 0)
				return S2_INVAL;
			if ((m->thread_types & caller->threadTypeId) == 0)
				return S2_NOT_SUPPORTED_THREAD;
			if (numNotifier == 0)
				return S2_INVAL;
			/* every blockable thread type the mutex admits needs a notifier */
			uint32_t uncovered = m->thread_types & 0xff;
			for (unsigned int k = 0; k < numNotifier; k++) {
				const CellSync2Notifier *n = notifiers[k];
				if (n->threadTypeId == 0)
					return S2_INVAL;
				if (!n->sendSignal)
					return S2_NULL_POINTER;
				uncovered &= ~(uint32_t)n->threadTypeId;
			}
			if (uncovered)
				return S2_NO_NOTIFIER;
			checked = 1;
		}

		if (!(m->flags & HELD))
			return S2_STAT;
		if (m->owner_type != caller->threadTypeId || m->owner != caller->self(caller->callbackArg))
			return S2_PERM;

		if (countOut) {
			*countOut = m->count;
			m->count = 0;
		} else {
			m->count -= 1;
		}
		handOver = 0;
		if (m->count == 0) {
			if ((int)m->waiters - (int)m->signalled > 0) {
				m->signalled += 1;
				handOver = 1;
			} else {
				m->flags &= ~HELD;
			}
			m->owner_type = 0;
			m->owner = 0;
		}
		if (s2_putllc(m, eaMutex))
			break;
	}

	if (handOver && __sync2_queue_wakeup(eaMutex, notifiers, numNotifier, dmaTag))
		s2_halt();
	return 0;
}
