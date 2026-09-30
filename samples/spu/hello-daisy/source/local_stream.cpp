/* A stream inside one core: Buffer::Local + QueueControl::Local, pushes and
 * pops interleaved with the try-forms, then termination.  Built for both the
 * PPU and the SPU (spu/local.cpp includes this file). */
#include <stdint.h>
#include <cell/daisy.h>
using namespace cell::Daisy;
struct Item { uint32_t seq, sq, pad[2]; };
int run_local()
{
	int fails = 0;
	Lock lock; memset((void *)&lock, 0, sizeof(lock));
	Buffer::Local<Item, 4> buf;
	QueueControl::Local<4, INPUT> qin(lock);
	QueueControl::Local<4, OUTPUT> qout(lock);
	Pipe::InPort<Buffer::Local<Item, 4>, QueueControl::Local<4, INPUT> > in(buf, qin);
	Pipe::OutPort<Buffer::Local<Item, 4>, QueueControl::Local<4, OUTPUT> > out(buf, qout);
	uint32_t next = 0, got = 0;
	while (got < 100) {
		Item it = { next, next * next, {0, 0} };
		while (next < 100 && in.tryBeginPush(&it) == CELL_OK) { in.endPush(); next++; it.seq = next; it.sq = next * next; }
		Item o;
		while (out.tryBeginPop(&o) == CELL_OK) { out.endPop(); if (o.seq != got || o.sq != got * got) fails++; got++; }
	}
	in.terminate();
	Item o;
	if (out.tryBeginPop(&o) != TERMINATED) fails++;
	if (in.hasUnfinishedConsumer()) fails++;
	return fails;
}
