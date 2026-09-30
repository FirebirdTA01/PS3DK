/* queue.c - the queue line and its push / pop.
 *
 * Queue line (128 bytes):
 *   +0x00 waiting queue header, pushers waiting for a free entry
 *   +0x10 waiting queue header, poppers waiting for an element
 *   +0x20 u32 elements present
 *   +0x24 u16 poppers queued         +0x26 u16 poppers signalled
 *   +0x28 u32 head entry             +0x2c u32 tail entry
 *   +0x30 u32 free entries
 *   +0x34 u16 pushers queued         +0x36 u16 pushers signalled
 *   +0x38 u32 where the next entry search starts
 *   +0x3c u32 EA of the entry-in-use bitmap (1 bit per entry, MSB first)
 *   +0x40 u32 EA of the link array (u32 per entry: next entry or -1)
 *   +0x44 u32 EA of the element array
 *   +0x48 u32 element size           +0x4c u32 entries (depth + 1)
 *   +0x54 u16 thread types  +0x56 u16 maxPushWaiters  +0x58 u16 maxPopWaiters
 * The elements form a linked list with a dummy head: a push writes its
 * element into a free entry and links it after the tail; a pop reads the
 * entry after the head, which then becomes the new head, and frees the old
 * one.  The tail may lag; whoever finds its link set moves it on.
 */
#include "sync2_internal.h"

typedef struct s2_queue {
	s2_queue_header push_queue;
	s2_queue_header pop_queue;
	uint32_t size;
	uint16_t pop_queued;
	uint16_t pop_signalled;
	uint32_t head;
	uint32_t tail;
	uint32_t free_entries;
	uint16_t push_queued;
	uint16_t push_signalled;
	uint32_t search;
	uint32_t bitmap;
	uint32_t links;
	uint32_t elements;
	uint32_t element_size;
	uint32_t entries;
	uint16_t pad50[2];
	uint16_t thread_types;
	uint16_t max_push_waiters;
	uint16_t max_pop_waiters;
} s2_queue;

_Static_assert(__builtin_offsetof(s2_queue, size) == 0x20, "queue size");
_Static_assert(__builtin_offsetof(s2_queue, free_entries) == 0x30, "queue free entries");
_Static_assert(__builtin_offsetof(s2_queue, entries) == 0x4c, "queue entries");
_Static_assert(__builtin_offsetof(s2_queue, max_pop_waiters) == 0x58, "queue maxPopWaiters");

#define NO_ENTRY 0xffffffffu

static volatile uint8_t s_line[128] __attribute__((aligned(128)));
static volatile uint32_t s_word[4] __attribute__((aligned(16)));

static inline volatile s2_queue *queue_line(void)
{
	return (volatile s2_queue *)__sync2_line;
}

/* One u32 of main memory, read with a plain DMA. */
static uint32_t read_word(uint32_t ea, unsigned int dmaTag)
{
	volatile uint32_t *ls = &s_word[(ea & 12) >> 2];
	s2_get(ls, ea, 4, dmaTag);
	return *ls;
}

static void repair_tail(uint32_t ea, unsigned int dmaTag)
{
	volatile s2_queue *q = queue_line();
	for (;;) {
		s2_getllar(q, ea);
		uint32_t next = read_word(q->links + q->tail * 4, dmaTag);
		if (next == NO_ENTRY)
			return;
		q->tail = next;
		s2_putllc(q, ea);
	}
}

static int check_caller(volatile s2_queue *q, const CellSync2CallerThreadType *caller,
                        CellSync2Notifier *const *notifiers, unsigned int numNotifier, int wait)
{
	if (!caller)
		return S2_NULL_POINTER;
	if (caller->threadTypeId == 0)
		return S2_INVAL;
	if ((q->thread_types & caller->threadTypeId) == 0)
		return S2_NOT_SUPPORTED_THREAD;
	if (wait && (!caller->allocateSignalReceiver || !caller->freeSignalReceiver || !caller->waitSignal))
		return S2_PERM;
	if (!notifiers)
		return S2_NULL_POINTER;
	if (numNotifier == 0)
		return S2_INVAL;
	uint32_t uncovered = q->thread_types & 0xff;
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

/* Reserve a free entry (waiting for one if asked) and mark it in use. */
static int allocate_entry(uint32_t ea, uint32_t *entry, const CellSync2CallerThreadType *caller, int wait,
                          unsigned int dmaTag)
{
	volatile s2_queue *q = queue_line();
	CellSync2SignalReceiverId receiver = 0;
	int haveReceiver = 0, queued;
	uint32_t start;

	for (;;) {
		s2_getllar(q, ea);
		if (q->free_entries > q->push_queued) {
			q->free_entries -= 1;
			start = q->search;
			q->search = start + 1 == q->entries ? 0 : start + 1;
			queued = 0;
		} else {
			if (!wait)
				return S2_AGAIN;
			if (q->push_queued == q->max_push_waiters)
				return S2_NOMEM;
			if (!haveReceiver) {
				int rc = caller->allocateSignalReceiver(&receiver, CELL_SYNC2_OBJECT_TYPE_QUEUE, ea,
				                                        caller->callbackArg);
				if (rc)
					return rc;
				haveReceiver = 1;
				continue;
			}
			q->push_queued += 1;
			queued = 1;
		}
		if (s2_putllc(q, ea))
			break;
	}

	if (queued) {
		if (__sync2_queue_enter(ea, caller->threadTypeId, receiver, caller->waitSignal,
		                        CELL_SYNC2_OBJECT_TYPE_QUEUE, ea, caller->callbackArg, dmaTag))
			s2_halt();
		do {
			s2_getllar(q, ea);
			if (q->free_entries == 0 || q->push_queued == 0 || q->push_signalled == 0)
				s2_halt();
			q->push_signalled -= 1;
			q->push_queued -= 1;
			q->free_entries -= 1;
			start = q->search;
			q->search = start + 1 == q->entries ? 0 : start + 1;
		} while (!s2_putllc(q, ea));
	}
	if (haveReceiver && caller->freeSignalReceiver(receiver, caller->callbackArg))
		s2_halt();

	/* claim the first entry clear in the bitmap, from the search start */
	const uint32_t entries = q->entries, bitmap = q->bitmap;
	for (uint32_t i = 0;;) {
		uint32_t e = (start + i) % entries;
		uint32_t wea = bitmap + (e >> 5) * 4, bit = 0x80000000u >> (e & 31);
		s2_getllar(s_line, wea & ~0x7fu);
		volatile uint32_t *w = (volatile uint32_t *)(s_line + (wea & 0x7c));
		if (*w & bit) {
			i++;
			continue;
		}
		*w |= bit;
		if (s2_putllc(s_line, wea & ~0x7fu)) {
			*entry = e;
			return 0;
		}
	}
}

/* Release an entry and hand it to a waiting pusher, if any. */
static void free_entry(uint32_t ea, uint32_t entry, CellSync2Notifier *const *notifiers, unsigned int numNotifier,
                       unsigned int dmaTag)
{
	volatile s2_queue *q = queue_line();
	int wake;

	s2_getllar(q, ea);
	if (entry == NO_ENTRY || entry >= q->entries)
		s2_halt();
	uint32_t wea = q->bitmap + (entry >> 5) * 4, bit = 0x80000000u >> (entry & 31);
	for (;;) {
		s2_getllar(s_line, wea & ~0x7fu);
		volatile uint32_t *w = (volatile uint32_t *)(s_line + (wea & 0x7c));
		if (!(*w & bit))
			s2_halt();
		*w &= ~bit;
		if (s2_putllc(s_line, wea & ~0x7fu))
			break;
	}
	do {
		s2_getllar(q, ea);
		q->free_entries += 1;
		wake = (int)q->push_queued - (int)q->push_signalled > 0;
		if (wake)
			q->push_signalled += 1;
	} while (!s2_putllc(q, ea));
	if (wake && __sync2_queue_wakeup(ea, notifiers, numNotifier, dmaTag))
		s2_halt();
}

int __sync2_queue_push(uint32_t ea, const void *data, const CellSync2CallerThreadType *caller,
                       CellSync2Notifier *const *notifiers, unsigned int numNotifier, int wait, unsigned int dmaTag)
{
	volatile s2_queue *q = queue_line();
	uint32_t entry, links, elements, size;
	int rc, wake;

	s2_getllar(q, ea);
	links = q->links;
	elements = q->elements;
	size = q->element_size;
	if ((rc = check_caller(q, caller, notifiers, numNotifier, wait)))
		return rc;
	if (wait && q->max_push_waiters == 0)
		return S2_NOMEM;
	if ((rc = allocate_entry(ea, &entry, caller, wait, dmaTag)))
		return rc;

	/* the element, then an end-of-list link for its entry */
	uint32_t lea = links + entry * 4;
	volatile uint32_t *link = &s_word[(lea & 12) >> 2];
	*link = NO_ENTRY;
	s2_barrier();
	mfc_put((volatile void *)(uintptr_t)data, elements + entry * size, size, dmaTag, 0, 0);
	mfc_put(link, lea, 4, dmaTag, 0, 0);
	s2_wait_tag(dmaTag);

	/* link it after the tail */
	for (;;) {
		uint32_t tail = read_word(ea + 0x2c, dmaTag);
		uint32_t tea = links + tail * 4;
		s2_getllar(s_line, tea & ~0x7fu);
		if (read_word(ea + 0x2c, dmaTag) != tail)
			continue;
		volatile uint32_t *next = (volatile uint32_t *)(s_line + (tea & 0x7c));
		if (*next != NO_ENTRY) {
			repair_tail(ea, dmaTag);
			continue;
		}
		*next = entry;
		if (s2_putllc(s_line, tea & ~0x7fu))
			break;
	}

	/* count it, move the tail on and wake a popper */
	do {
		s2_getllar(q, ea);
		q->size += 1;
		wake = (int)q->pop_queued - (int)q->pop_signalled > 0;
		if (wake)
			q->pop_signalled += 1;
		uint32_t next = read_word(links + q->tail * 4, dmaTag);
		if (next != NO_ENTRY)
			q->tail = next;
	} while (!s2_putllc(q, ea));
	if (wake && __sync2_queue_wakeup(ea + 0x10, notifiers, numNotifier, dmaTag))
		s2_halt();
	return 0;
}

int __sync2_queue_pop(uint32_t ea, void *buffer, const CellSync2CallerThreadType *caller,
                      CellSync2Notifier *const *notifiers, unsigned int numNotifier, int wait, unsigned int dmaTag)
{
	volatile s2_queue *q = queue_line();
	CellSync2SignalReceiverId receiver = 0;
	int haveReceiver = 0, queued, rc;
	uint32_t size, head;

	s2_getllar(q, ea);
	size = q->element_size;
	if ((rc = check_caller(q, caller, notifiers, numNotifier, wait)))
		return rc;
	if (wait && q->max_pop_waiters == 0)
		return S2_NOMEM;

	for (;;) {
		s2_getllar(q, ea);
		if (q->size > q->pop_queued) {
			q->size -= 1;
			queued = 0;
		} else {
			if (!wait)
				return S2_AGAIN;
			if (q->pop_queued == q->max_pop_waiters)
				return S2_NOMEM;
			if (!haveReceiver) {
				rc = caller->allocateSignalReceiver(&receiver, CELL_SYNC2_OBJECT_TYPE_QUEUE, ea,
				                                    caller->callbackArg);
				if (rc)
					return rc;
				haveReceiver = 1;
				continue;
			}
			q->pop_queued += 1;
			queued = 1;
		}
		if (s2_putllc(q, ea))
			break;
	}

	if (queued) {
		if (__sync2_queue_enter(ea + 0x10, caller->threadTypeId, receiver, caller->waitSignal,
		                        CELL_SYNC2_OBJECT_TYPE_QUEUE, ea, caller->callbackArg, dmaTag))
			s2_halt();
		do {
			s2_getllar(q, ea);
			if (q->size == 0 || q->pop_queued == 0 || q->pop_signalled == 0)
				s2_halt();
			q->pop_queued -= 1;
			q->pop_signalled -= 1;
			q->size -= 1;
		} while (!s2_putllc(q, ea));
	}
	if (haveReceiver && caller->freeSignalReceiver(receiver, caller->callbackArg))
		s2_halt();

	/* take the element after the head; that entry becomes the head */
	for (;;) {
		s2_getllar(q, ea);
		head = q->head;
		if (head == q->tail) {
			repair_tail(ea, dmaTag);
			continue;
		}
		uint32_t next = read_word(q->links + head * 4, dmaTag);
		if (next == NO_ENTRY)
			continue;
		q->head = next;
		s2_get(buffer, q->elements + next * size, size, dmaTag);
		if (s2_putllc(q, ea))
			break;
	}
	free_entry(ea, head, notifiers, numNotifier, dmaTag);
	return 0;
}
