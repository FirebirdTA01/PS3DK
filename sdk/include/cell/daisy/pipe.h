/* cell/daisy/pipe.h - Pipe::InPort and Pipe::OutPort: the producer's and the
 * consumer's end of a stream, over any Buffer + QueueControl pair of the
 * same tSize.
 *
 * A push or pop is split in two so its transfer can overlap other work:
 * begin reserves an entry (and, in COPY mode, moves the data), end completes
 * the oldest begun one.  Completions happen in begin order.  try-forms return
 * QUEUE_IS_BUSY instead of waiting.  In REFERENCE mode (local buffers only)
 * begin takes no data and getCurrentReference() hands out the entry.
 *
 * A consumer's pops return TERMINATED once every producer has called
 * terminate() and the queue is drained; the consumer is then detached.  A
 * producer waits for hasUnfinishedConsumer() to turn false before tearing
 * the stream down.
 *
 * cancelPush() / cancelPop() set the newest begun entry aside: the port keeps
 * its reservation and the next begin on that port takes the same entry
 * again, so begin again soon after a cancel.  The queue's counters are never
 * rolled back, so other ends are unaffected.  Cancelling is not available to
 * COPY ports on QueueControl::Local or QueueControl::SignalNotification.
 *
 * The data forms of begin / push / pop belong to COPY ports and the no-data
 * forms and getCurrentReference() to REFERENCE ports; the other pairing does
 * not compile. */
#ifndef PS3TC_CELL_DAISY_PIPE_H
#define PS3TC_CELL_DAISY_PIPE_H

#include <cell/daisy/daisy_defs.h>

namespace cell {
namespace Daisy {
namespace Pipe {

/* begun-but-not-ended entries of one port */
static const int PENDING_MAX = 15;

/* a compile-time check on a template parameter, made where it is used */
#if __cplusplus >= 201103L
#define PS3TC_DAISY_REQUIRE(cond, msg) static_assert(cond, msg)
#else
#define PS3TC_DAISY_REQUIRE_CAT2(a, b) a##b
#define PS3TC_DAISY_REQUIRE_CAT(a, b) PS3TC_DAISY_REQUIRE_CAT2(a, b)
#define PS3TC_DAISY_REQUIRE(cond, msg) \
	typedef char PS3TC_DAISY_REQUIRE_CAT(ps3tcDaisyRequire, __LINE__)[(cond) ? 1 : -1] __attribute__((unused))
#endif

template <class tBuffer, class tQueueControl, BufferMode tMode = COPY>
class Port {
public:
	typedef typename tBuffer::DataType Type;
	static const BufferMode sBufferMode = tMode;

	Port(tBuffer &buffer, tQueueControl &queueControl, int bookmarkId)
		: mBuffer(buffer), mQueueControl(queueControl), mHead(0), mCount(0),
		  mHasSetAside(false), mSetAside(0), mIsTerminated(false), mBookmarkId(bookmarkId)
	{
	}
	/* not virtual: SPU images are position-independent and cannot carry
	 * vtables (run-time relocations); ports are used by their own type */
	~Port() {}
	const char *getClassName() { return 0; }

	/* the newest begun entry (REFERENCE ports), or NULL */
	volatile Type *getCurrentReference()
	{
		PS3TC_DAISY_REQUIRE(tMode == REFERENCE, "getCurrentReference() needs a REFERENCE port");
		if (mCount == 0)
			return 0;
		return mBuffer.getEntryReference(mPending[(mHead + mCount - 1) % (PENDING_MAX + 1)]);
	}
	bool hasPendingEntry() { return mCount != 0; }

	int terminate()
	{
		if (!mIsTerminated) {
			mIsTerminated = true;
			return mQueueControl.terminate();
		}
		return CELL_OK;
	}

protected:
	typedef char sSizeCheck[(tBuffer::sSize == tQueueControl::sSize) &&
	                        (tMode != REFERENCE || tBuffer::sBufferType == BUFFER_TYPE_LOCAL) ? 1 : -1];

	bool pendingFull() { return mCount >= PENDING_MAX; }
	void pendingPush(PointerType p)
	{
		mPending[(mHead + mCount) % (PENDING_MAX + 1)] = p;
		mCount++;
	}
	PointerType pendingOldest() { return mPending[mHead]; }
	void pendingDropOldest()
	{
		mHead = (mHead + 1) % (PENDING_MAX + 1);
		mCount--;
	}
	PointerType pendingNewest() { return mPending[(mHead + mCount - 1) % (PENDING_MAX + 1)]; }
	void pendingDropNewest() { mCount--; }

	/* set the newest begun entry aside for the next begin */
	void cancelNewest()
	{
		PS3TC_DAISY_REQUIRE(tMode != COPY ||
		                        (tQueueControl::sQueueControlType != QCTL_TYPE_LOCAL &&
		                         tQueueControl::sQueueControlType != QCTL_TYPE_SIGNAL_NOTIFICATION),
		                    "cancel is not available to COPY ports on Local or SignalNotification queues");
		if (mCount == 0 || mHasSetAside)
			return;
		PointerType p = pendingNewest();
		/* a transfer begun for it lands before the entry is used again */
		mBuffer.waitTransfer(p);
		pendingDropNewest();
		mSetAside = p;
		mHasSetAside = true;
	}
	/* an entry for begin: the one set aside, or a new reservation */
	int reserve(PointerType *p, bool wait)
	{
		if (mHasSetAside) {
			mHasSetAside = false;
			*p = mSetAside;
			return CELL_OK;
		}
		int ret;
		while ((ret = mQueueControl.tryReserve(p)) == QUEUE_IS_BUSY && wait)
			mQueueControl.relax();
		return ret;
	}

	/* finish the oldest begun entry: wait for its turn, or report busy */
	int endOldest(bool wait)
	{
		if (mCount == 0)
			return CELL_DAISY_ERROR_NO_BEGIN;
		PointerType p = pendingOldest();
		if (!wait && !(mBuffer.transferDone(p) && mQueueControl.isTurn(p)))
			return QUEUE_IS_BUSY;
		/* the entry's transfer lands before the entry is handed over */
		mBuffer.waitTransfer(p);
		mQueueControl.complete(p);
		pendingDropOldest();
		return CELL_OK;
	}

	tBuffer &mBuffer;
	tQueueControl &mQueueControl;
	PointerType mPending[PENDING_MAX + 1];
	int mHead, mCount;
	bool mHasSetAside;
	PointerType mSetAside;
	bool mIsTerminated;
	int mBookmarkId;
};

template <class tBuffer, class tQueueControl, BufferMode tMode = COPY>
class InPort : public Port<tBuffer, tQueueControl, tMode> {
	typedef Port<tBuffer, tQueueControl, tMode> Base;
	typedef char sDirectionCheck[tQueueControl::sPort == INPUT ? 1 : -1];

public:
	typedef typename tBuffer::DataType Type;
	typedef typename tBuffer::DataType GlueDataType;

	explicit InPort(tBuffer &buffer, tQueueControl &queueControl, int bookmarkId = 0)
		: Base(buffer, queueControl, bookmarkId)
	{
	}
	/* detach, then wait until no consumer is still attached */
	~InPort()
	{
		this->terminate();
		while (hasUnfinishedConsumer())
			this->mQueueControl.relax();
	}
	const char *getClassName() { return "Pipe::InPort"; }

	int tryBeginPush(Type *data) { requireCopy(); return begin(data, false); }
	int tryBeginPush() { requireReference(); return begin(0, false); }
	int beginPush(Type *data) { requireCopy(); return begin(data, true); }
	int beginPush() { requireReference(); return begin(0, true); }
	int endPush() { return this->endOldest(true); }
	int tryEndPush() { return this->endOldest(false); }

	int push(Type *data)
	{
		requireCopy();
		int ret = begin(data, true);
		if (ret != CELL_OK)
			return ret;
		return endPush();
	}

	/* set the newest begun push aside; the next begin takes it again */
	void cancelPush() { this->cancelNewest(); }

	bool hasUnfinishedConsumer() { return this->mQueueControl.hasUnfinishedConsumer(); }

private:
	void requireCopy() { PS3TC_DAISY_REQUIRE(tMode == COPY, "this push form needs a COPY port"); }
	void requireReference() { PS3TC_DAISY_REQUIRE(tMode == REFERENCE, "this push form needs a REFERENCE port"); }

	int begin(Type *data, bool wait)
	{
		if (this->pendingFull())
			return CELL_DAISY_ERROR_AGAIN;
		PointerType p;
		int ret = this->reserve(&p, wait);
		if (ret != CELL_OK)
			return ret;
		if (tMode == COPY)
			this->mBuffer.copyIn(p, data);
		this->pendingPush(p);
		return CELL_OK;
	}
};

template <class tBuffer, class tQueueControl, BufferMode tMode = COPY>
class OutPort : public Port<tBuffer, tQueueControl, tMode> {
	typedef Port<tBuffer, tQueueControl, tMode> Base;
	typedef char sDirectionCheck[tQueueControl::sPort == OUTPUT ? 1 : -1];

public:
	typedef typename tBuffer::DataType Type;
	typedef typename tBuffer::DataType GlueDataType;

	explicit OutPort(tBuffer &buffer, tQueueControl &queueControl, int bookmarkId = 0)
		: Base(buffer, queueControl, bookmarkId)
	{
	}
	~OutPort() { this->terminate(); }
	const char *getClassName() { return "Pipe::OutPort"; }

	int tryBeginPop(Type *data) { requireCopy(); return begin(data, false); }
	int tryBeginPop() { requireReference(); return begin(0, false); }
	int beginPop(Type *data) { requireCopy(); return begin(data, true); }
	int beginPop() { requireReference(); return begin(0, true); }
	int endPop() { return this->endOldest(true); }
	int tryEndPop() { return this->endOldest(false); }

	int pop(Type *data)
	{
		requireCopy();
		int ret = begin(data, true);
		if (ret != CELL_OK)
			return ret;
		return endPop();
	}

	/* set the newest begun pop aside; the next begin takes it again */
	void cancelPop() { this->cancelNewest(); }

private:
	void requireCopy() { PS3TC_DAISY_REQUIRE(tMode == COPY, "this pop form needs a COPY port"); }
	void requireReference() { PS3TC_DAISY_REQUIRE(tMode == REFERENCE, "this pop form needs a REFERENCE port"); }

	int begin(Type *data, bool wait)
	{
		if (this->mIsTerminated)
			return TERMINATED;
		if (this->pendingFull())
			return CELL_DAISY_ERROR_AGAIN;
		PointerType p;
		int ret = this->reserve(&p, wait);
		if (ret == TERMINATED) {
			/* drained and every producer gone: detach */
			this->terminate();
			return TERMINATED;
		}
		if (ret != CELL_OK)
			return ret;
		if (tMode == COPY)
			this->mBuffer.copyOut(p, data);
		this->pendingPush(p);
		return CELL_OK;
	}
};

} /* namespace Pipe */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_PIPE_H */
