/* hello-daisy SPU side of the signal-notification stream: SPU 1 produces
 * into its own local store, SPU 0 consumes by DMA from there; the two ends
 * find each other through shared PARAMETER areas in main memory.
 *
 *   arg1 signal-notification area EA (256 bytes, zeroed), arg2 stream
 *   meeting area EA (128 bytes, zeroed), arg3 this thread's index in the
 *   group (0 consumer, 1 producer), arg4 result EA (consumer) */
#include <stdint.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <cell/daisy.h>
#include <cell/daisy/snr_qctl.h>
#include "../source/daisy_common.h"

using namespace cell::Daisy;

static Result result __attribute__((aligned(128)));

int main(uint64_t snrAreaEa, uint64_t paramEa, uint64_t spuNum, uint64_t resultEa)
{
	QueueControl::initializeSignalNotification(snrAreaEa, (int)spuNum, 0);
	if (spuNum == 1) {
		typedef Buffer::Local<Word, DEPTH, PARAMETER> Buf;
		typedef QueueControl::SignalNotification<DEPTH, INPUT, PARAMETER> Qctl;
		Buf buf(paramEa, (int)spuNum);
		Qctl q(paramEa, 20);
		Pipe::InPort<Buf, Qctl> in(buf, q);
		static Word w __attribute__((aligned(16)));
		for (uint32_t i = 0; i < COUNT; i++) {
			w.v = i;
			in.push(&w);
		}
		/* the destructor terminates and waits for the consumer */
	} else {
		typedef Buffer::Remote<Word, DEPTH, PARAMETER> Buf;
		typedef QueueControl::SignalNotification<DEPTH, OUTPUT, PARAMETER> Qctl;
		Buf buf(paramEa, 2);
		Qctl q(paramEa, 20);
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
