/* cell/daisy/snr_qctl.h - QueueControl::SignalNotification (SPU): a stream
 * between one producer SPU and one consumer SPU of the same thread group.
 *
 * Each end keeps its completed count and flags in a slot of its own local
 * store; the other end reads that slot by DMA from the SPU's local store
 * window.  After updating its slot an end signals the other's notification
 * register (SNR1 towards the consumer, SNR2 towards the producer; OR mode),
 * and an end that finds the queue busy blocks reading its register.  An end
 * checks the queue again after every wake-up, so a signal read on behalf of
 * another stream is never lost.
 *
 * Every SPU using these calls initializeSignalNotification() once first: it
 * publishes where this SPU's slots are, in a 256-byte zeroed area all the
 * SPUs share (CELL_DAISY_NBSNR_PARAM_SIZE bytes).  Pipe ids are
 * 0..MAX_PIPE-1, distinct among one SPU's receiving ends; a producer's send
 * id is its consumer's receive id and the other way round. */
#ifndef PS3TC_CELL_DAISY_SNR_QCTL_H
#define PS3TC_CELL_DAISY_SNR_QCTL_H

#include <cell/daisy/qctl.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#define CELL_DAISY_NBSNR_PARAM_SIZE 256

namespace cell {
namespace Daisy {

static const int MAX_PIPE = 4;

/* base of the message receivers (kept for the class hierarchy) */
class MessageReceiverStub {};

namespace QueueControl {

/* flag word bits */
enum { SNR_FLAG_TERMINATED = 1, SNR_FLAG_DETACHED = 2 };

struct SnrState {
	volatile uint32_t slot[MAX_PIPE][4];   /* per receive pipe: our count, flags */
	uint64_t paramEa;
	int mySpuNum, groupNum;
	uint32_t usedPipes;
	uint32_t publish[4] __attribute__((aligned(16)));
} __attribute__((aligned(128)));

/* one per SPU program (vague linkage keeps it single across files) */
inline SnrState &snrState()
{
	static SnrState state;
	return state;
}

static inline uint64_t snrLsEa(int spuNum)
{
	return (uint64_t)SYS_SPU_THREAD_BASE_LOW + (uint64_t)SYS_SPU_THREAD_OFFSET * spuNum + SYS_SPU_THREAD_LS_BASE;
}

static inline void snrDmaWait(uint32_t tag)
{
	mfc_write_tag_mask(1u << tag);
	mfc_read_tag_status_all();
}

/* read 16 bytes at ea (16-byte aligned) */
static inline void snrRead16(uint64_t ea, volatile uint32_t *out, uint32_t tag)
{
	mfc_get(out, ea, 16, tag, 0, 0);
	snrDmaWait(tag);
}

/* publish where this SPU's flag slots are: slot mySpuNum of the area is
 * {local store address of the slots, 1} */
inline void initializeSignalNotification(uint64_t parameterEa, int mySpuNum, int groupNum)
{
	SnrState &s = snrState();
	s.paramEa = parameterEa;
	s.mySpuNum = mySpuNum;
	s.groupNum = groupNum;
	s.publish[0] = (uint32_t)(uintptr_t)&s.slot[0][0];
	s.publish[1] = 1;
	mfc_put(s.publish, parameterEa + 16 * (uint64_t)mySpuNum, 16, 31, 0, 0);
	snrDmaWait(31);
}

/* where spuNum's flag slots are, once it has initialised */
static inline uint32_t snrPeerSlots(int spuNum)
{
	static volatile uint32_t buf[4] __attribute__((aligned(16)));
	do
		snrRead16(snrState().paramEa + 16 * (uint64_t)spuNum, buf, 31);
	while (buf[1] == 0);
	return buf[0];
}

template <SizeType tSize, QueueIO tQueueIO, ConstructorMode tConstructorMode = NO_PARAMETER>
class SignalNotification : public Abstract<tSize, tQueueIO>, public MessageReceiverStub {
public:
	static const QueueControlType sQueueControlType = QCTL_TYPE_SIGNAL_NOTIFICATION;

	explicit SignalNotification(uint64_t snrEa, uint32_t dmaTag, uint32_t sendPipeId, uint32_t receivePipeId)
	{
		setup(snrEa, dmaTag, sendPipeId, receivePipeId);
	}

	/* PARAMETER: the two ends meet in the 128-byte area at parameterEa;
	 * each writes {spu number, receive pipe id, 1} to its half (producer
	 * first 16 bytes, consumer next 16) and reads the other's */
	explicit SignalNotification(uint64_t parameterEa, uint32_t dmaTag, int receivePipeId = -1)
	{
		SnrState &s = snrState();
		uint32_t recv = receivePipeId >= 0 ? (uint32_t)receivePipeId : freePipe();
		mInfo[0] = (uint32_t)s.mySpuNum;
		mInfo[1] = recv;
		mInfo[2] = 1;
		mInfo[3] = 0;
		uint64_t mine = parameterEa + (tQueueIO == INPUT ? 0 : 16);
		uint64_t theirs = parameterEa + (tQueueIO == INPUT ? 16 : 0);
		mfc_put(mInfo, mine, 16, dmaTag % 32, 0, 0);
		snrDmaWait(dmaTag % 32);
		do
			snrRead16(theirs, mInfo, dmaTag % 32);
		while (mInfo[2] == 0);
		uint64_t peerSnr = snrLsEa((int)mInfo[0]) - SYS_SPU_THREAD_LS_BASE +
		                   (tQueueIO == INPUT ? SYS_SPU_THREAD_SNR1 : SYS_SPU_THREAD_SNR2);
		setup(peerSnr, dmaTag, mInfo[1], recv);
	}

	void setDmaTagRangeMask(uint32_t mask) { (void)mask; }
	uint32_t getTag() const { return mTag; }

	int tryReserve(PointerType *entry)
	{
		volatile uint32_t *peer = readPeer();
		if (tQueueIO == INPUT) {
			if (mReserved - peer[0] >= tSize)
				return QUEUE_IS_BUSY;
		} else if (mReserved == peer[0]) {
			return (peer[1] & SNR_FLAG_TERMINATED) ? TERMINATED : QUEUE_IS_BUSY;
		}
		*entry = mReserved++;
		return CELL_OK;
	}
	/* one producer and one consumer: completions come in order */
	bool isTurn(PointerType entry) { return entry == mCompleted; }
	void complete(PointerType entry)
	{
		mCompleted = entry + 1;
		publish();
	}
	bool release(PointerType entry)
	{
		if (entry + 1 != mReserved)
			return false;
		mReserved = entry;
		return true;
	}
	int terminate()
	{
		if (!(mFlags & (SNR_FLAG_TERMINATED | SNR_FLAG_DETACHED))) {
			mFlags |= tQueueIO == INPUT ? SNR_FLAG_TERMINATED : SNR_FLAG_DETACHED;
			publish();
		}
		return CELL_OK;
	}
	bool hasUnfinishedConsumer() { return !(readPeer()[1] & SNR_FLAG_DETACHED); }
	bool isOutOfOrder() { return false; }
	/* sleep until the other end signals */
	void relax()
	{
		if (tQueueIO == INPUT)
			spu_readch(SPU_RdSigNotify2);
		else
			spu_readch(SPU_RdSigNotify1);
	}
	const char *getClassName() { return "QueueControl::SignalNotification"; }

private:
	void setup(uint64_t snrEa, uint32_t dmaTag, uint32_t sendPipeId, uint32_t receivePipeId)
	{
		SnrState &s = snrState();
		int peer = (int)((snrEa - SYS_SPU_THREAD_BASE_LOW) / SYS_SPU_THREAD_OFFSET);
		mPeerSnrEa = snrEa;
		mPeerSlotEa = snrLsEa(peer) + snrPeerSlots(peer) + 16 * (uint64_t)sendPipeId;
		mTag = dmaTag % 32;
		mRecvPipe = receivePipeId;
		s.usedPipes |= 1u << receivePipeId;
		s.slot[receivePipeId][0] = 0;
		s.slot[receivePipeId][1] = 0;
		mReserved = mCompleted = 0;
		mFlags = 0;
	}
	uint32_t freePipe()
	{
		for (int i = 0; i < MAX_PIPE; i++)
			if (!(snrState().usedPipes & (1u << i)))
				return (uint32_t)i;
		return 0;
	}
	/* the other end's slot: its completed count and flags */
	volatile uint32_t *readPeer()
	{
		snrRead16(mPeerSlotEa, mPeer, mTag);
		return mPeer;
	}
	/* our slot, then the wake-up: the entries' transfers have landed (the
	 * port waits for them) and dsync orders our local-store writes before
	 * the signal */
	void publish()
	{
		volatile uint32_t *mine = snrState().slot[mRecvPipe];
		mine[1] = mFlags;
		mine[0] = mCompleted;
		spu_dsync();
		mSignal[3] = 1u << mRecvPipe;
		mfc_sndsig(&mSignal[3], mPeerSnrEa, mTag, 0, 0);
		snrDmaWait(mTag);
	}

	volatile uint32_t mPeer[4] __attribute__((aligned(16)));
	volatile uint32_t mSignal[4] __attribute__((aligned(16)));
	volatile uint32_t mInfo[4] __attribute__((aligned(16)));
	uint64_t mPeerSnrEa, mPeerSlotEa;
	uint32_t mTag, mRecvPipe;
	uint32_t mReserved, mCompleted, mFlags;
};

} /* namespace QueueControl */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_SNR_QCTL_H */
