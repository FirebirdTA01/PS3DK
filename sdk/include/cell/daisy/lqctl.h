/* cell/daisy/lqctl.h - QueueControl::Local: a stream whose producers and
 * consumers all run on one core (one SPU, or PPU threads), controlled
 * through a shared Lock.  Use it with Buffer::Local.  0 < tSize <= 32767. */
#ifndef PS3TC_CELL_DAISY_LQCTL_H
#define PS3TC_CELL_DAISY_LQCTL_H

#include <cell/daisy/qctl.h>
#include <cell/daisy/lock.h>

namespace cell {
namespace Daisy {
namespace QueueControl {

template <SizeType tSize, QueueIO tQueueIO>
class Local : public Abstract<tSize, tQueueIO> {
public:
	static const QueueControlType sQueueControlType = QCTL_TYPE_LOCAL;

	explicit Local(Lock &lock) : mLock(lock), mDetached(false)
	{
		LockOps::attach(lock, tSize);
		if (tQueueIO == INPUT) {
			LockOps::add(&lock.producers, 1);
			LockOps::store(&lock.producerSeen, 1);
		} else {
			LockOps::add(&lock.consumers, 1);
		}
	}

	int tryReserve(PointerType *entry)
	{
		for (;;) {
			if (tQueueIO == INPUT) {
				uint32_t n = LockOps::load(&mLock.reservedPush);
				if (n - LockOps::load(&mLock.freed) >= tSize)
					return QUEUE_IS_BUSY;
				if (LockOps::cas(&mLock.reservedPush, n, n + 1)) {
					*entry = n;
					return CELL_OK;
				}
			} else {
				uint32_t n = LockOps::load(&mLock.reservedPop);
				if (n == LockOps::load(&mLock.published)) {
					if (LockOps::load(&mLock.producerSeen) && LockOps::load(&mLock.producers) == 0 &&
					    n == LockOps::load(&mLock.reservedPush))
						return TERMINATED;
					return QUEUE_IS_BUSY;
				}
				if (LockOps::cas(&mLock.reservedPop, n, n + 1)) {
					*entry = n;
					return CELL_OK;
				}
			}
		}
	}

	/* completions are published in entry order */
	bool isTurn(PointerType entry)
	{
		return LockOps::load(tQueueIO == INPUT ? &mLock.published : &mLock.freed) == entry;
	}
	void complete(PointerType entry)
	{
		while (!isTurn(entry))
			LockOps::relax();
		LockOps::store(tQueueIO == INPUT ? &mLock.published : &mLock.freed, entry + 1);
	}
	/* undo the newest reservation if nobody reserved after it */
	bool release(PointerType entry)
	{
		return LockOps::cas(tQueueIO == INPUT ? &mLock.reservedPush : &mLock.reservedPop,
		                    entry + 1, entry);
	}

	int terminate()
	{
		if (!mDetached) {
			mDetached = true;
			LockOps::add(tQueueIO == INPUT ? &mLock.producers : &mLock.consumers, -1);
		}
		return CELL_OK;
	}
	bool hasUnfinishedConsumer() { return LockOps::load(&mLock.consumers) != 0; }
	bool isOutOfOrder() { return false; }
	void relax() { LockOps::relax(); }
	const char *getClassName() { return "QueueControl::Local"; }

private:
	Lock &mLock;
	bool mDetached;
};

} /* namespace QueueControl */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_LQCTL_H */
