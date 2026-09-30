/* hello-daisy SPU side of the atomic streams: a producer or a consumer on a
 * QueueControl::Atomic stream whose entries are in main memory.
 *
 *   arg1 lock area EA, arg2 buffer EA, arg3 role (0 consumer, 1 producer),
 *   arg4 result EA (consumer: count, sum, order faults) */
#include <stdint.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <cell/daisy.h>
#include "../source/daisy_common.h"

using namespace cell::Daisy;

typedef Buffer::Remote<Word, DEPTH> RemoteBuffer;
static Result result __attribute__((aligned(128)));

int main(uint64_t lockEa, uint64_t bufEa, uint64_t role, uint64_t resultEa)
{
	if (role == 1) {
		RemoteBuffer buf(bufEa, 2);
		QueueControl::Atomic<DEPTH, INPUT> q(lockEa);
		Pipe::InPort<RemoteBuffer, QueueControl::Atomic<DEPTH, INPUT> > in(buf, q);
		static Word w __attribute__((aligned(16)));
		for (uint32_t i = 0; i < COUNT; i++) {
			w.v = i;
			in.push(&w);   /* push waits for the DMA before returning */
		}
		/* the destructor terminates and waits for the consumers */
	} else {
		RemoteBuffer buf(bufEa, 2);
		QueueControl::Atomic<DEPTH, OUTPUT> q(lockEa);
		Pipe::OutPort<RemoteBuffer, QueueControl::Atomic<DEPTH, OUTPUT> > out(buf, q);
		static Word w __attribute__((aligned(16)));
		uint32_t last = 0;
		bool first = true;
		while (out.pop(&w) == CELL_OK) {
			if (!first && w.v <= last)
				result.faults++;
			first = false;
			last = w.v;
			result.count++;
			result.sum += w.v;
		}
		mfc_put(&result, resultEa, sizeof(result), 3, 0, 0);
		mfc_write_tag_mask(1u << 3);
		mfc_read_tag_status_all();
	}
	sys_spu_thread_exit(0);
	return 0;
}
