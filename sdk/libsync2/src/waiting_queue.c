/* waiting_queue.c - the FIFO of blocked threads every sync2 object keeps.
 *
 * Enter and wake-up both run in three steps: announce yourself in the
 * queue header (enters / wake-ups in progress), claim or process one entry
 * of the waiting buffer, then retire from the header, advancing its index.
 * A waker may reach an entry before its waiter has written it; it then
 * leaves a marker (E) and the waiter, finding it, does not block.
 * See sync2_internal.h for the header and entry layout.
 */
#include "sync2_internal.h"

static volatile uint8_t s_entry_line[128] __attribute__((aligned(128)));
static volatile uint8_t s_scan_line[128] __attribute__((aligned(128)));
static volatile uint8_t s_header[16] __attribute__((aligned(16)));

static inline volatile s2_queue_header *queue_in_line(uint32_t eaQueue)
{
	return (volatile s2_queue_header *)(__sync2_line + (eaQueue & 0x7f));
}

/* A fresh copy of the queue's wake and enter words. */
static volatile s2_queue_header *read_header(uint32_t eaQueue, unsigned int dmaTag)
{
	volatile uint8_t *ls = s_header + (eaQueue & 0xf);
	s2_get(ls, eaQueue, 8, dmaTag);
	return (volatile s2_queue_header *)ls;
}

/* Distance from index `from` forward to index `to`. */
static inline uint32_t forward(uint32_t from, uint32_t to, uint32_t entries)
{
	return to >= from ? to - from : to + entries - from;
}

/* Whether the `count` entries from index `idx` (phase `phase`) all show
 * their own lap parity in bit `bit`: written (P) for enter, signalled (S)
 * for wake-up. */
static int entries_done(uint32_t buffer, uint32_t idx, uint32_t phase, uint32_t count,
                        uint32_t entries, unsigned int bit, unsigned int dmaTag)
{
	uint32_t cached = 1;
	for (uint32_t j = 0; j < count; j++) {
		uint32_t slot = idx + j, parity = phase;
		if (slot >= entries) {
			slot -= entries;
			parity ^= 1;
		}
		uint32_t ea = buffer + slot * 8;
		if ((ea & ~0x7fu) != cached) {
			cached = ea & ~0x7fu;
			s2_get(s_scan_line, cached, 128, dmaTag);
		}
		uint32_t w0 = ((volatile s2_entry *)(s_scan_line + (ea & 0x7f)))->word0;
		if (((w0 >> bit) & 1) != parity)
			return 0;
	}
	return 1;
}

int __sync2_queue_enter(uint32_t eaQueue, uint16_t threadTypeId, uint64_t receiver,
                        s2_wait_fn waitSignal, CellSync2ObjectTypeId objectType,
                        uint64_t eaObject, uint64_t callbackArg, unsigned int dmaTag)
{
	const uint32_t eaLine = eaQueue & ~0x7fu;
	volatile s2_queue_header *q = queue_in_line(eaQueue);
	const uint32_t tlog = 31 - __builtin_clz(threadTypeId);
	uint32_t entries, buffer, base, i = 0;
	int signalled;

	do {
		s2_getllar(__sync2_line, eaLine);
		q->enter_ctl = (q->enter_ctl & S2_PHASE) | ((q->enter_ctl + 1) & S2_COUNT);
	} while (!s2_putllc(__sync2_line, eaLine));
	entries = q->entries;
	buffer = q->buffer;
	base = q->enter_idx;

	for (;;) {
		uint32_t slot = (base + i) % entries;
		uint32_t ea = buffer + slot * 8;
		s2_getllar(s_entry_line, ea & ~0x7fu);

		volatile s2_queue_header *h = read_header(eaQueue, dmaTag);
		uint32_t idx = h->enter_idx, phase = h->enter_ctl >> 15;
		uint32_t dist = forward(idx, slot, entries);
		if ((h->enter_ctl & S2_COUNT) <= dist
		    || !entries_done(buffer, idx, phase, dist, entries, 31, dmaTag)) {
			base = idx;
			i = 0;
			continue;
		}

		uint32_t parity = phase ^ (idx > slot);
		volatile s2_entry *e = (volatile s2_entry *)(s_entry_line + (ea & 0x7f));
		uint32_t w0 = e->word0;
		if ((w0 >> 31) == parity) {
			/* written this lap by another waiter: try the next one */
			if (idx != base) {
				base = idx;
				i = 0;
			} else {
				i++;
			}
			continue;
		}
		if (w0 & S2_ENTRY_E) {
			/* the wake-up got here first */
			e->word0 = (w0 & (S2_ENTRY_S | S2_ENTRY_TAG)) | (parity << 31);
			e->word1 = 0;
			signalled = 1;
		} else {
			e->word0 = (w0 & (S2_ENTRY_S | S2_ENTRY_TAG)) | (parity << 31) | (tlog << 16)
			           | ((uint32_t)(receiver >> 32) & S2_ENTRY_RECV);
			e->word1 = (uint32_t)receiver;
			signalled = 0;
		}
		if (s2_putllc(s_entry_line, ea & ~0x7fu))
			break;
	}

	do {
		s2_getllar(__sync2_line, eaLine);
		uint32_t ctl = (q->enter_ctl & S2_PHASE) | ((q->enter_ctl - 1) & S2_COUNT);
		uint32_t idx = q->enter_idx + 1;
		if (idx == entries) {
			idx = 0;
			ctl ^= S2_PHASE;
		}
		q->enter_ctl = ctl;
		q->enter_idx = idx;
	} while (!s2_putllc(__sync2_line, eaLine));

	if (!signalled && waitSignal(receiver, objectType, eaObject, callbackArg) != 0)
		s2_halt();
	return 0;
}

int __sync2_queue_wakeup(uint32_t eaQueue, CellSync2Notifier *const *notifiers,
                         unsigned int numNotifier, unsigned int dmaTag)
{
	const uint32_t eaLine = eaQueue & ~0x7fu;
	volatile s2_queue_header *q = queue_in_line(eaQueue);
	uint32_t entries, buffer, base, i = 0;
	uint32_t typeId = 0;
	uint64_t receiver = 0;
	int signalled, rc = 0;

	do {
		s2_getllar(__sync2_line, eaLine);
		uint32_t busy = ((q->wake_ctl & S2_COUNT) + 1) & S2_COUNT;
		uint32_t sum = q->wake_idx + busy;
		entries = q->entries;
		/* refuse to run a whole lap ahead of the waiters */
		if (sum % entries == q->enter_idx
		    && ((q->wake_ctl >> 15) ^ (sum >= entries)) != (uint32_t)(q->enter_ctl >> 15))
			return S2_NOMEM;
		q->wake_ctl = (q->wake_ctl & S2_PHASE) | busy;
	} while (!s2_putllc(__sync2_line, eaLine));
	buffer = q->buffer;
	base = q->wake_idx;

	for (;;) {
		uint32_t slot = (base + i) % entries;
		uint32_t ea = buffer + slot * 8;
		s2_getllar(s_entry_line, ea & ~0x7fu);

		volatile s2_queue_header *h = read_header(eaQueue, dmaTag);
		uint32_t idx = h->wake_idx, phase = h->wake_ctl >> 15;
		uint32_t dist = forward(idx, slot, entries);
		if ((h->wake_ctl & S2_COUNT) <= dist
		    || !entries_done(buffer, idx, phase, dist, entries, 30, dmaTag)) {
			base = idx;
			i = 0;
			continue;
		}

		uint32_t parity = phase ^ (idx > slot);
		volatile s2_entry *e = (volatile s2_entry *)(s_entry_line + (ea & 0x7f));
		uint32_t w0 = e->word0;
		if (((w0 >> 30) & 1) == parity) {
			/* already signalled by another waker */
			if (idx != base) {
				base = idx;
				i = 0;
			} else {
				i++;
			}
			continue;
		}
		uint32_t tag = (w0 + (1u << 20)) & S2_ENTRY_TAG;
		if ((w0 >> 31) == parity) {
			typeId = 1u << ((w0 & S2_ENTRY_TLOG) >> 16);
			receiver = ((uint64_t)(w0 & S2_ENTRY_RECV) << 32) | e->word1;
			e->word0 = (w0 & ~(S2_ENTRY_S | S2_ENTRY_E | S2_ENTRY_TAG)) | (parity << 30) | tag;
			signalled = 1;
		} else {
			/* the waiter has not written its entry yet: leave the marker */
			e->word0 = (w0 & S2_ENTRY_P) | (parity << 30) | S2_ENTRY_E | tag;
			e->word1 = 0;
			signalled = 0;
		}
		if (s2_putllc(s_entry_line, ea & ~0x7fu))
			break;
	}

	int (*send)(CellSync2SignalReceiverId, uint64_t) = 0;
	uint64_t sendArg = 0;
	if (signalled) {
		for (unsigned int k = 0; k < numNotifier; k++) {
			const CellSync2Notifier *n = notifiers[k];
			if (n->threadTypeId == typeId) {
				send = n->sendSignal;
				sendArg = n->callbackArg;
			}
		}
		if (!send)
			rc = S2_INVAL;
	}

	do {
		s2_getllar(__sync2_line, eaLine);
		uint32_t ctl = (q->wake_ctl & S2_PHASE) | ((q->wake_ctl - 1) & S2_COUNT);
		if (rc == 0) {
			uint32_t idx = q->wake_idx + 1;
			if (idx == entries) {
				idx = 0;
				ctl ^= S2_PHASE;
			}
			q->wake_idx = idx;
		}
		q->wake_ctl = ctl;
	} while (!s2_putllc(__sync2_line, eaLine));

	if (signalled && send && send(receiver, sendArg) != 0)
		s2_halt();
	return rc;
}
