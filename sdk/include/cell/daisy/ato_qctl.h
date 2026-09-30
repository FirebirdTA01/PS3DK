/* cell/daisy/ato_qctl.h - QueueControl::Atomic: a stream whose ends may be on
 * different SPUs and one PPU thread, several producers and consumers,
 * synchronised through a shared 256-byte area in main memory (lockEa,
 * 128-byte aligned, zeroed before the first constructor).
 *
 * The first 128-byte line holds the same counters as a local stream's Lock;
 * SPUs update it with lock-line reservations, the PPU with atomic
 * instructions, and the two agree at that granularity.  Between PPU and
 * SPUs, the PPU end is either every producer or every consumer; its buffer
 * is a Buffer::Local over the shared entries, the SPU ends' a
 * Buffer::Remote at the same address.
 *
 * Waiting polls.  The SPURS co-operative arguments (eaSignal and the signal
 * functions) are accepted; the ends then still poll. */
#ifndef PS3TC_CELL_DAISY_ATO_QCTL_H
#define PS3TC_CELL_DAISY_ATO_QCTL_H

#include <cell/daisy/qctl.h>
#include <cell/daisy/lock.h>
#include <cell/daisy/lfqueue2_types.h>
#ifdef __SPU__
#include <spu_mfcio.h>
#else
#include <sys/spu_thread.h>
#endif

namespace cell {
namespace Daisy {
namespace QueueControl {

/* word offsets of the counters in the shared line (the Lock layout) */
enum {
	ATO_INITIALIZED, ATO_DEPTH, ATO_RESERVED_PUSH, ATO_PUBLISHED,
	ATO_RESERVED_POP, ATO_FREED, ATO_PRODUCERS, ATO_CONSUMERS, ATO_PRODUCER_SEEN
};

#ifdef __SPU__
/* One lock line, read and conditionally written back. */
class AtomicLine {
public:
	explicit AtomicLine(uint64_t ea) : mEa(ea) {}
	uint32_t read(int word)
	{
		load();
		return mLine[word];
	}
	/* apply f to a fresh copy until the conditional write lands; f returns
	 * false to give up without writing */
	template <class F> bool update(F f)
	{
		for (;;) {
			load();
			if (!f(mLine))
				return false;
			mfc_putllc(mLine, mEa, 0, 0);
			if ((mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0)
				return true;
		}
	}
	uint64_t ea() const { return mEa; }

private:
	void load()
	{
		mfc_getllar(mLine, mEa, 0, 0);
		mfc_read_atomic_status();
	}
	uint64_t mEa;
	volatile uint32_t mLine[32] __attribute__((aligned(128)));
};
#endif

template <SizeType tSize, QueueIO tQueueIO>
class Atomic : public Abstract<tSize, tQueueIO> {
public:
	static const QueueControlType sQueueControlType = QCTL_TYPE_ATOMIC;

#ifdef __SPU__
	explicit Atomic(uint64_t lockEa, uint64_t eaSignal = 0, uint32_t (*fpGetId)(void) = 0,
	                int (*fpSendSignal)(uint64_t, uint32_t) = 0, int (*fpWaitSignal)(void) = 0)
		: mLine(lockEa), mDetached(false)
	{
		(void)eaSignal; (void)fpGetId; (void)fpSendSignal; (void)fpWaitSignal;
		attach();
	}
#else
	explicit Atomic(LFQueue2 *lockEa, sys_spu_thread_t *ids = 0, unsigned int numSpus = 0,
	                void *eaSignal = 0, int (*fpSendSignal)(void *, uint32_t) = 0)
		: mWords((volatile uint32_t *)lockEa), mDetached(false)
	{
		(void)ids; (void)numSpus; (void)eaSignal; (void)fpSendSignal;
		attach();
	}
#endif

	int tryReserve(PointerType *entry)
	{
		int result = QUEUE_IS_BUSY;
		PointerType n = 0;
#ifdef __SPU__
		struct Reserve {
			int *result;
			PointerType *n;
			bool operator()(volatile uint32_t *w)
			{
				return reserveStep(w, result, n);
			}
		} f = { &result, &n };
		mLine.update(f);
#else
		for (;;) {
			uint32_t snapshot[9];
			for (int i = 0; i < 9; i++)
				snapshot[i] = LockOps::load(&mWords[i]);
			if (!reserveStep(snapshot, &result, &n))
				break;
			int word = tQueueIO == INPUT ? ATO_RESERVED_PUSH : ATO_RESERVED_POP;
			if (LockOps::cas(&mWords[word], n, n + 1))
				break;
		}
#endif
		if (result == CELL_OK)
			*entry = n;
		return result;
	}

	bool isTurn(PointerType entry) { return read(tQueueIO == INPUT ? ATO_PUBLISHED : ATO_FREED) == entry; }

	void complete(PointerType entry)
	{
		int word = tQueueIO == INPUT ? ATO_PUBLISHED : ATO_FREED;
		while (!isTurn(entry))
			relax();
#ifndef __SPU__
		/* the entry's contents before the counter that hands it over */
		__atomic_thread_fence(__ATOMIC_RELEASE);
#endif
		add(word, 1);
	}

	bool release(PointerType entry)
	{
		int word = tQueueIO == INPUT ? ATO_RESERVED_PUSH : ATO_RESERVED_POP;
		return cas(word, entry + 1, entry);
	}

	int terminate()
	{
		if (!mDetached) {
			mDetached = true;
			add(tQueueIO == INPUT ? ATO_PRODUCERS : ATO_CONSUMERS, -1);
		}
		return CELL_OK;
	}
	bool hasUnfinishedConsumer() { return read(ATO_CONSUMERS) != 0; }
	bool isOutOfOrder() { return false; }
	void relax()
	{
#ifndef __SPU__
		LockOps::relax();
#endif
	}
	const char *getClassName() { return "QueueControl::Atomic"; }

private:
	/* one reservation attempt on a copy of the counters: sets *result and
	 * *n; returns true when the copy should be written back with n + 1 */
	static bool reserveStep(volatile uint32_t *w, int *result, PointerType *n)
	{
		if (tQueueIO == INPUT) {
			*n = w[ATO_RESERVED_PUSH];
			if (*n - w[ATO_FREED] >= tSize) {
				*result = QUEUE_IS_BUSY;
				return false;
			}
			w[ATO_RESERVED_PUSH] = *n + 1;
		} else {
			*n = w[ATO_RESERVED_POP];
			if (*n == w[ATO_PUBLISHED]) {
				*result = (w[ATO_PRODUCER_SEEN] && w[ATO_PRODUCERS] == 0 && *n == w[ATO_RESERVED_PUSH])
				              ? TERMINATED : QUEUE_IS_BUSY;
				return false;
			}
			w[ATO_RESERVED_POP] = *n + 1;
		}
		*result = CELL_OK;
		return true;
	}

	void attach()
	{
		cas(ATO_INITIALIZED, 0, 1);
		if (tQueueIO == INPUT) {
			add(ATO_PRODUCERS, 1);
			cas(ATO_PRODUCER_SEEN, 0, 1);
		} else {
			add(ATO_CONSUMERS, 1);
		}
	}

#ifdef __SPU__
	uint32_t read(int word) { return mLine.read(word); }
	struct Add {
		int word;
		int32_t d;
		bool operator()(volatile uint32_t *w) { w[word] += (uint32_t)d; return true; }
	};
	struct Cas {
		int word;
		uint32_t expect, v;
		bool operator()(volatile uint32_t *w)
		{
			if (w[word] != expect)
				return false;
			w[word] = v;
			return true;
		}
	};
	void add(int word, int32_t d) { Add f = { word, d }; mLine.update(f); }
	bool cas(int word, uint32_t expect, uint32_t v) { Cas f = { word, expect, v }; return mLine.update(f); }
	AtomicLine mLine;
#else
	uint32_t read(int word) { return LockOps::load(&mWords[word]); }
	void add(int word, int32_t d) { LockOps::add(&mWords[word], d); }
	bool cas(int word, uint32_t expect, uint32_t v) { return LockOps::cas(&mWords[word], expect, v); }
	volatile uint32_t *mWords;
#endif
	bool mDetached;
};

} /* namespace QueueControl */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_ATO_QCTL_H */
