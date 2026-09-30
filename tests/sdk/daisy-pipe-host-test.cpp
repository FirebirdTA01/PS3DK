/* Host test for the PPU libdaisy ports: cancelling a begun push or pop with
 * several ends on one stream.
 *
 * A cancel sets the entry aside in the port and the port's next begin takes
 * it again; the queue's counters never roll back.  Each case interleaves two
 * ends so that a rollback would fail (another end reserved after the
 * cancelled entry): the cancelled entry must come back on the next begin and
 * every entry must still complete in order.  Non-blocking try-forms are used
 * throughout so a regression fails a check instead of hanging. */
#include <cell/daisy.h>
#include <stdio.h>
#include <string.h>

using namespace cell::Daisy;

static int fails;
#define CHECK(c)                                                         \
	do {                                                                 \
		if (!(c)) {                                                      \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
			fails++;                                                     \
		}                                                                \
	} while (0)

struct Word { uint32_t v, pad[3]; };
static const SizeType DEPTH = 4;

/* the port's current entry; a missing one is a failure, not a crash */
template <class P> static volatile Word *ref(P &port)
{
	static Word dummy;
	volatile Word *r = port.getCurrentReference();
	CHECK(r != 0);
	return r ? r : &dummy;
}

/* Local queue, REFERENCE ports: P1 and P2 begin, P1 cancels and begins
 * again, both end; the consumer sees P1's value then P2's. */
static void local_reference_cancel_push()
{
	typedef Buffer::Local<Word, DEPTH> Buf;
	typedef QueueControl::Local<DEPTH, INPUT> In;
	typedef QueueControl::Local<DEPTH, OUTPUT> Out;
	static Lock lock __attribute__((aligned(128)));
	memset((void *)&lock, 0, sizeof(lock));
	Buf buf;
	In q1(lock), q2(lock);
	Out qc(lock);
	Pipe::InPort<Buf, In, REFERENCE> p1(buf, q1), p2(buf, q2);
	Pipe::OutPort<Buf, Out, REFERENCE> c(buf, qc);

	CHECK(p1.tryBeginPush() == CELL_OK);            /* entry 0 */
	volatile Word *e0 = ref(p1);
	CHECK(p2.tryBeginPush() == CELL_OK);            /* entry 1 */
	ref(p2)->v = 222;
	p1.cancelPush();
	CHECK(!p1.hasPendingEntry());
	CHECK(lock.reservedPush == 2);                  /* nothing rolled back */
	CHECK(p1.tryBeginPush() == CELL_OK);            /* entry 0 again */
	CHECK(ref(p1) == e0);
	CHECK(lock.reservedPush == 2);
	ref(p1)->v = 111;
	CHECK(p2.tryEndPush() == QUEUE_IS_BUSY);        /* entry 0 publishes first */
	CHECK(p1.tryEndPush() == CELL_OK);
	CHECK(p2.tryEndPush() == CELL_OK);
	CHECK(lock.published == 2);

	CHECK(c.tryBeginPop() == CELL_OK);
	CHECK(ref(c)->v == 111);
	CHECK(c.tryEndPop() == CELL_OK);
	CHECK(c.tryBeginPop() == CELL_OK);
	CHECK(ref(c)->v == 222);
	CHECK(c.tryEndPop() == CELL_OK);
	CHECK(lock.freed == 2);
	p1.terminate();
	p2.terminate();
	CHECK(c.tryBeginPop() == TERMINATED);
}

/* Local queue, REFERENCE ports: the symmetric case for two consumers. */
static void local_reference_cancel_pop()
{
	typedef Buffer::Local<Word, DEPTH> Buf;
	typedef QueueControl::Local<DEPTH, INPUT> In;
	typedef QueueControl::Local<DEPTH, OUTPUT> Out;
	static Lock lock __attribute__((aligned(128)));
	memset((void *)&lock, 0, sizeof(lock));
	Buf buf;
	In qp(lock);
	Out q1(lock), q2(lock);
	Pipe::InPort<Buf, In, REFERENCE> p(buf, qp);
	Pipe::OutPort<Buf, Out, REFERENCE> c1(buf, q1), c2(buf, q2);

	for (uint32_t i = 0; i < 2; i++) {
		CHECK(p.tryBeginPush() == CELL_OK);
		ref(p)->v = 10 + i;
		CHECK(p.tryEndPush() == CELL_OK);
	}
	CHECK(c1.tryBeginPop() == CELL_OK);             /* entry 0 */
	CHECK(c2.tryBeginPop() == CELL_OK);             /* entry 1 */
	CHECK(ref(c2)->v == 11);
	c1.cancelPop();
	CHECK(lock.reservedPop == 2);
	CHECK(c1.tryBeginPop() == CELL_OK);             /* entry 0 again */
	CHECK(ref(c1)->v == 10);
	CHECK(c2.tryEndPop() == QUEUE_IS_BUSY);
	CHECK(c1.tryEndPop() == CELL_OK);
	CHECK(c2.tryEndPop() == CELL_OK);
	CHECK(lock.freed == 2);
}

/* Atomic queue, COPY ports: cancelling is available here, and the second
 * begin carries new data into the same entry. */
static void atomic_copy_cancel_push()
{
	typedef Buffer::Local<Word, DEPTH> Buf;
	typedef QueueControl::Atomic<DEPTH, INPUT> In;
	typedef QueueControl::Atomic<DEPTH, OUTPUT> Out;
	static LFQueue2 area;
	static Word entries[DEPTH] __attribute__((aligned(128)));
	memset(&area, 0, sizeof(area));
	Buf buf(entries);
	In q1(&area), q2(&area);
	Out qc(&area);
	Pipe::InPort<Buf, In> p1(buf, q1), p2(buf, q2);
	Pipe::OutPort<Buf, Out> c(buf, qc);
	volatile uint32_t *w = (volatile uint32_t *)&area;

	Word a = { 1, {0, 0, 0} }, b = { 2, {0, 0, 0} }, a2 = { 3, {0, 0, 0} }, out;
	CHECK(p1.tryBeginPush(&a) == CELL_OK);          /* entry 0 */
	CHECK(p2.tryBeginPush(&b) == CELL_OK);          /* entry 1 */
	p1.cancelPush();
	CHECK(w[QueueControl::ATO_RESERVED_PUSH] == 2);
	CHECK(p1.tryBeginPush(&a2) == CELL_OK);         /* entry 0, new data */
	CHECK(w[QueueControl::ATO_RESERVED_PUSH] == 2);
	CHECK(p2.tryEndPush() == QUEUE_IS_BUSY);
	CHECK(p1.tryEndPush() == CELL_OK);
	CHECK(p2.tryEndPush() == CELL_OK);
	CHECK(c.tryBeginPop(&out) == CELL_OK && out.v == 3);
	CHECK(c.tryEndPop() == CELL_OK);
	CHECK(c.tryBeginPop(&out) == CELL_OK && out.v == 2);
	CHECK(c.tryEndPop() == CELL_OK);
}

/* A second cancel with nothing begun since the first keeps the first. */
static void cancel_twice_keeps_first()
{
	typedef Buffer::Local<Word, DEPTH> Buf;
	typedef QueueControl::Local<DEPTH, INPUT> In;
	static Lock lock __attribute__((aligned(128)));
	memset((void *)&lock, 0, sizeof(lock));
	Buf buf;
	In q(lock);
	Pipe::InPort<Buf, In, REFERENCE> p(buf, q);
	CHECK(p.tryBeginPush() == CELL_OK);             /* entry 0 */
	CHECK(p.tryBeginPush() == CELL_OK);             /* entry 1 */
	volatile Word *e1 = ref(p);
	p.cancelPush();                                 /* entry 1 set aside */
	p.cancelPush();                                 /* ignored */
	CHECK(p.hasPendingEntry());                     /* entry 0 still begun */
	CHECK(p.tryBeginPush() == CELL_OK);
	CHECK(ref(p) == e1);
	CHECK(lock.reservedPush == 2);
	CHECK(p.tryEndPush() == CELL_OK);
	CHECK(p.tryEndPush() == CELL_OK);
	CHECK(lock.published == 2);
	/* no consumer attached: the destructor's wait ends at once */
}

int main()
{
	local_reference_cancel_push();
	local_reference_cancel_pop();
	atomic_copy_cancel_push();
	cancel_twice_keeps_first();
	printf("daisy-pipe: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
