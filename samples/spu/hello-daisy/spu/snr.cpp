/* hello-daisy SPU side of the signal-notification stream: SPU 1 produces
 * into its own local store, SPU 0 consumes by DMA from there; the two ends
 * find each other through shared PARAMETER areas in main memory.
 *
 * Before the stream each SPU checks receive-pipe allocation: four pipes
 * are accepted, and a fifth automatic id, a duplicate id or an id out of
 * range leaves the queue control invalid.
 *
 *   arg1 signal-notification area EA (256 bytes, zeroed), arg2 stream
 *   meeting area EA (128 bytes, zeroed), arg3 this thread's index in the
 *   group (0 consumer, 1 producer) | DMA tag range mask << 32 (0 = default),
 *   arg4 result EA: the consumer's Result, the producer's faults 128 bytes
 *   further on */
#include <stdint.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <cell/daisy.h>
#include "../source/daisy_common.h"

using namespace cell::Daisy;

static Result result __attribute__((aligned(128)));

/* receive pipes: four taken, the fifth and bad ids refused, all freed */
static void check_pipes(int spuNum, uint64_t paramEa)
{
	typedef QueueControl::SignalNotification<DEPTH, OUTPUT> Qctl;
	typedef QueueControl::SignalNotification<DEPTH, OUTPUT, PARAMETER> AutoQctl;
	uint64_t self = CELL_DAISY_GET_SNR1_AREA(spuNum);
	{
		Qctl a(self, 21, 0, 0), b(self, 21, 1, 1), c(self, 21, 2, 2), d(self, 21, 3, 3);
		if (!a.isValid() || !b.isValid() || !c.isValid() || !d.isValid())
			result.faults++;
		Qctl dup(self, 21, 0, 0);
		Qctl range(self, 21, 0, MAX_PIPE);
		Qctl badSend(self, 21, MAX_PIPE, 0);
		AutoQctl fifth(paramEa, 21);   /* refused before any exchange */
		PointerType e;
		if (dup.isValid() || range.isValid() || badSend.isValid() || fifth.isValid())
			result.faults++;
		if (fifth.tryReserve(&e) != (int)CELL_DAISY_ERROR_INVAL)
			result.faults++;
	}
	if (QueueControl::snrState().usedPipes != 0)
		result.faults++;
}

int main(uint64_t snrAreaEa, uint64_t paramEa, uint64_t arg3, uint64_t resultEa)
{
	int spuNum = (int)(uint32_t)arg3;
	uint32_t mask = (uint32_t)(arg3 >> 32);
	QueueControl::initializeSignalNotification(snrAreaEa, spuNum, 0);
	check_pipes(spuNum, paramEa);
	if (spuNum == 1) {
		typedef Buffer::Local<Word, DEPTH, PARAMETER> Buf;
		typedef QueueControl::SignalNotification<DEPTH, INPUT, PARAMETER> Qctl;
		Buf buf(paramEa, spuNum);
		Qctl q(paramEa, 20);
		if (mask)
			q.setDmaTagRangeMask(mask);
		{
			Pipe::InPort<Buf, Qctl> in(buf, q);
			static Word w __attribute__((aligned(16)));
			for (uint32_t i = 0; i < COUNT; i++) {
				w.v = i;
				in.push(&w);
			}
			/* the destructor terminates and waits for the consumer */
		}
		mfc_put(&result, resultEa + sizeof(Result), sizeof(result), 3, 0, 0);
		mfc_write_tag_mask(1u << 3);
		mfc_read_tag_status_all();
	} else {
		typedef Buffer::Remote<Word, DEPTH, PARAMETER> Buf;
		typedef QueueControl::SignalNotification<DEPTH, OUTPUT, PARAMETER> Qctl;
		Buf buf(paramEa, 2);
		Qctl q(paramEa, 20);
		if (mask) {
			buf.setDmaTagRangeMask(mask);
			q.setDmaTagRangeMask(mask);
		}
		Pipe::OutPort<Buf, Qctl> out(buf, q);
		static Word w __attribute__((aligned(16)));
		uint32_t expect = 0;
		int ret;
		while ((ret = out.pop(&w)) == CELL_OK) {
			if (w.v != expect)
				result.faults++;
			expect++;
			result.count++;
			result.sum += w.v;
		}
		if (ret != TERMINATED)
			result.faults++;
		mfc_put(&result, resultEa, sizeof(result), 3, 0, 0);
		mfc_write_tag_mask(1u << 3);
		mfc_read_tag_status_all();
	}
	sys_spu_thread_exit(0);
	return 0;
}
