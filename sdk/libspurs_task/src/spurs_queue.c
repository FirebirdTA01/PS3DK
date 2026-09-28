/* SPU side of the SPURS queue.
 *
 * A CellSpursQueue is one 128-byte control line in main memory plus a ring
 * of `depth` entries of `entrySize` bytes at `buffer`.  PPU threads and SPU
 * tasks share the line, so every update is a getllar/putllc
 * read-modify-write retried until the reservation holds.  Line layout
 * (big-endian, offsets in bytes):
 *
 *   0x00 popIndex    u32  next entry to pop; ~index while a pop is in flight
 *   0x04 pushIndex   u32  next entry to push; ~index while a push is in flight
 *   0x08 entrySize   u32
 *   0x0c depth       u32
 *   0x10 buffer      u64  EA of the ring
 *   0x18 spuPort     u32  SPU event port to a blocked PPU thread (~0: none)
 *   0x1c direction   u32
 *   0x20 popWait[16]      u8  [0] waiting poppers, [1..15] their task ids, FIFO
 *   0x30 pushWait[16]     u8  the same for pushers
 *   0x40 popWaitWkl[16]   u8  [1..15] workload ids beside popWait
 *   0x50 pushWaitWkl[16]  u8  workload ids beside pushWait
 *   0x60 taskset     u64  owning taskset (0 for a queue bound to a SPURS instance)
 *   0x68 spurs       u64  SPURS instance
 *   0x70             PPU side; left untouched
 *
 * Indices run over 0 .. 2*depth-1, so a full ring (push - pop == depth)
 * differs from an empty one.  Begin reserves an index by storing it
 * complemented, then moves the entry between the caller's LS buffer and
 * the ring by DMA on the caller's tag; End waits for that tag and
 * publishes the index.  A task that cannot proceed queues itself in the
 * wait list and blocks in cellSpursWaitSignal; End reserves the next index
 * on the head waiter's behalf before signalling it, and a blocked PPU
 * thread is woken through spuPort.  Independently written from the
 * published object layout and wake protocol.
 */
#include <stdint.h>
#include <spu_mfcio.h>

#define ERR_AGAIN        0x80410901u
#define ERR_INVAL        0x80410902u
#define ERR_PERM         0x80410909u
#define ERR_BUSY         0x8041090Au
#define ERR_ALIGN        0x80410910u
#define ERR_NULL_POINTER 0x80410911u
#define ERR_FATAL        0x80410914u

enum { SPU2SPU = 0, SPU2PPU = 1, PPU2SPU = 2 };
#define NO_PORT      0xffffffffu
#define MAX_WAITERS  15
#define MAX_ENTRY    0x4000u

/* LS 0x2fd8: the object a blocked task waits on, tagged with its kind */
#define TASK_WAIT_OBJECT 0x2fd8
#define WAIT_KIND_QUEUE  4

typedef struct queue_line {
	uint32_t popIndex;
	uint32_t pushIndex;
	uint32_t entrySize;
	uint32_t depth;
	uint64_t buffer;
	uint32_t spuPort;
	uint32_t direction;
	uint8_t  popWait[16];
	uint8_t  pushWait[16];
	uint8_t  popWaitWkl[16];
	uint8_t  pushWaitWkl[16];
	uint64_t taskset;
	uint64_t spurs;
	uint8_t  ppuSide[16];
} __attribute__((aligned(128))) queue_line_t;

_Static_assert(sizeof(queue_line_t) == 128, "queue is one line");
_Static_assert(__builtin_offsetof(queue_line_t, spuPort) == 0x18, "port");
_Static_assert(__builtin_offsetof(queue_line_t, popWait) == 0x20, "pop waiters");
_Static_assert(__builtin_offsetof(queue_line_t, pushWaitWkl) == 0x50, "push waiter workloads");
_Static_assert(__builtin_offsetof(queue_line_t, taskset) == 0x60, "taskset");
_Static_assert(__builtin_offsetof(queue_line_t, spurs) == 0x68, "spurs");

/* libspurs.a services this file builds on */
extern unsigned int cellSpursGetTaskId(void);
extern uint64_t cellSpursGetTasksetAddress(void);
extern uint64_t cellSpursGetSpursAddress(void);
extern unsigned int cellSpursGetWorkloadId(void);
extern int cellSpursSendSignal(uint64_t eaTaskset, unsigned int idTask);
extern int cellSpursWaitSignal(void);
extern int _cellSpursTaskCanCallBlockWait(void);

static queue_line_t line;

static void line_get(uint64_t ea)
{
	mfc_getllar(&line, ea, 0, 0);
	(void)mfc_read_atomic_status();
}

/* 1 when the conditional put kept the reservation */
static int line_put(uint64_t ea)
{
	mfc_putllc(&line, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

static int in_flight(uint32_t v) { return (int32_t)v < 0; }
static uint32_t index_of(uint32_t v) { return in_flight(v) ? ~v : v; }

static uint32_t entries(uint32_t pop, uint32_t push, uint32_t depth)
{
	return pop > push ? push - pop + 2 * depth : push - pop;
}

static uint32_t next_index(uint32_t i, uint32_t depth)
{
	return i + 1 < 2 * depth ? i + 1 : 0;
}

/* Remove the head waiter from a count + FIFO list */
static void dequeue(uint8_t list[16])
{
	unsigned i;
	for (i = 1; i < 15; ++i)
		list[i] = list[i + 1];
	list[15] = 0;
	list[0] = list[0] ? (uint8_t)(list[0] - 1) : 0;
}

/* Fire-and-forget SPU-thread event to the PPU queue behind `port`
 * (the sys_spu_thread_throw_event mailbox protocol). */
static void throw_port_event(uint32_t port)
{
	if (port > 63)
		return;
	spu_writech(SPU_WrOutMbox, 0);
	spu_writech(SPU_WrOutIntrMbox, (port << 24) | 0x40000000u);
}

/* The taskset a waiter belongs to: the queue's own taskset, or for a
 * queue bound to a SPURS instance, workload wkl's argument, which for a
 * taskset workload is the taskset's address. */
static int waiter_taskset(unsigned wkl, uint64_t *taskset)
{
	uint64_t arg[2] __attribute__((aligned(16)));
	uint64_t info;
	if (line.taskset) {
		*taskset = line.taskset;
		return 0;
	}
	if (wkl >= 32)
		return 1;
	/* CellSpurs workload info: 32-byte entries at 0xb00 (0-15) and 0x1000
	   (16-31); the 64-bit argument is at +0x08. */
	info = line.spurs + ((wkl & 0x10) ? 0x1000u : 0xb00u) + (wkl & 0xfu) * 0x20u + 0x08u;
	mfc_get(&arg[(info >> 3) & 1], info, 8, 31, 0, 0);
	mfc_write_tag_mask(1u << 31);
	(void)mfc_read_tag_status_all();
	*taskset = arg[(info >> 3) & 1];
	return 0;
}

static int wake(uint8_t taskId, uint8_t wkl)
{
	uint64_t taskset;
	if (waiter_taskset(wkl, &taskset))
		return (int)ERR_FATAL;
	return cellSpursSendSignal(taskset, taskId) ? (int)ERR_FATAL : 0;
}

static int check_ea(uint64_t ea)
{
	if (!ea)
		return (int)ERR_NULL_POINTER;
	if (ea & 127)
		return (int)ERR_ALIGN;
	return 0;
}

static void wait_tag(unsigned tag)
{
	mfc_write_tag_mask(1u << tag);
	(void)mfc_read_tag_status_all();
}

int _cellSpursQueueInitialize(uint64_t ea, uint64_t buffer, unsigned int size,
                              unsigned int depth, unsigned direction, unsigned isIwl)
{
	unsigned i;
	if (!ea || (size && !buffer))
		return (int)ERR_NULL_POINTER;
	if (size > MAX_ENTRY || (size & 15) || !depth || (depth & 0xc0000000u)
	    || direction > PPU2SPU)
		return (int)ERR_INVAL;
	if ((ea & 127) || (buffer & 15))
		return (int)ERR_ALIGN;
	line_get(ea);
	line.popIndex = 0;
	line.pushIndex = 0;
	line.entrySize = size;
	line.depth = depth;
	line.buffer = buffer;
	line.spuPort = NO_PORT;
	line.direction = direction;
	for (i = 0; i < 16; ++i) {
		line.popWait[i] = 0;
		line.pushWait[i] = 0;
		line.popWaitWkl[i] = 0;
		line.pushWaitWkl[i] = 0;
	}
	line.taskset = isIwl ? 0 : cellSpursGetTasksetAddress();
	line.spurs = cellSpursGetSpursAddress();
	mfc_putlluc(&line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	return 0;
}

/* Reserve the next entry for a push or a pop and start its DMA */
static int queue_begin(uint64_t ea, void *ls, unsigned tag, unsigned block, int push)
{
	uint32_t index = 0, slot;
	uint64_t entry;
	int woken = 0, rc;
	if (!ea || !ls)
		return (int)ERR_NULL_POINTER;
	if (((uintptr_t)ls & 15) || (ea & 127))
		return (int)ERR_ALIGN;
	if (tag > 31)
		return (int)ERR_INVAL;
	if (block && (rc = _cellSpursTaskCanCallBlockWait()) != 0)
		return rc;

	for (;;) {
		uint32_t *mine;
		uint8_t *waitList, *waitWkl;
		uint32_t n;
		int mustWait;
		do {
			line_get(ea);
			if (line.direction == (push ? PPU2SPU : SPU2PPU))
				return (int)ERR_PERM;
			mine = push ? &line.pushIndex : &line.popIndex;
			waitList = push ? line.pushWait : line.popWait;
			waitWkl = push ? line.pushWaitWkl : line.popWaitWkl;
			n = entries(index_of(line.popIndex), index_of(line.pushIndex), line.depth);
			/* wait behind an entry already in flight, a full (push) or
			   empty (pop) ring, or tasks already waiting their turn */
			mustWait = in_flight(*mine) || n == (push ? line.depth : 0) || waitList[0];
			if (!block) {
				if (n == (push ? line.depth : 0))
					return (int)ERR_AGAIN;
				if (mustWait)
					return (int)ERR_BUSY;
			}
			index = index_of(*mine);
			if (woken) {
				/* the waker reserved the index for us */
				dequeue(waitList);
				dequeue(waitWkl);
			} else if (mustWait) {
				if (waitList[0] >= MAX_WAITERS)
					return (int)ERR_AGAIN;
				waitList[0]++;
				waitList[waitList[0]] = (uint8_t)cellSpursGetTaskId();
				waitWkl[0] = waitList[0];
				waitWkl[waitList[0]] = (uint8_t)cellSpursGetWorkloadId();
			}
			if (woken || !mustWait)
				*mine = ~index;
		} while (!line_put(ea));

		if (woken || !mustWait)
			break;
		if ((rc = _cellSpursTaskCanCallBlockWait()) != 0)
			return rc;
		*(volatile uint64_t *)TASK_WAIT_OBJECT = ea | WAIT_KIND_QUEUE;
		rc = cellSpursWaitSignal();
		*(volatile uint64_t *)TASK_WAIT_OBJECT = 0;
		if (rc)
			return rc;
		woken = 1;
	}

	slot = index < line.depth ? index : index - line.depth;
	entry = line.buffer + (uint64_t)slot * line.entrySize;
	if (push)
		mfc_put(ls, entry, line.entrySize, tag, 0, 0);
	else
		mfc_get(ls, entry, line.entrySize, tag, 0, 0);
	return 0;
}

/* Complete a push or pop: publish the index and wake whoever can now run */
static int queue_end(uint64_t ea, unsigned tag, int push, unsigned peek)
{
	int wakePop, wakePush, throwPpu, rc;
	uint8_t popTask, popWkl, pushTask, pushWkl;
	uint32_t port;
	if ((rc = check_ea(ea)) != 0)
		return rc;
	if (tag > 31)
		return (int)ERR_INVAL;
	wait_tag(tag);

	do {
		uint32_t a, b, n;
		line_get(ea);
		if (line.direction == (push ? PPU2SPU : SPU2PPU))
			return (int)ERR_PERM;
		a = index_of(line.popIndex);
		b = index_of(line.pushIndex);
		n = entries(a, b, line.depth);
		if (push) {
			/* n excludes the entry being published */
			wakePop = line.popWait[0] && !in_flight(line.popIndex);
			wakePush = line.pushWait[0] && n + 1 < line.depth;
			throwPpu = line.spuPort != NO_PORT && in_flight(line.popIndex) && n == 0;
			b = next_index(b, line.depth);
			if (wakePop)
				line.popIndex = ~a;
			line.pushIndex = wakePush ? ~b : b;
		} else {
			/* n includes the entry being popped */
			wakePop = line.popWait[0] && (peek ? n : n - 1) > 0;
			wakePush = line.pushWait[0] && !peek && !in_flight(line.pushIndex);
			throwPpu = line.spuPort != NO_PORT && !peek
			           && in_flight(line.pushIndex) && n == line.depth;
			if (!peek)
				a = next_index(a, line.depth);
			line.popIndex = wakePop ? ~a : a;
			if (wakePush)
				line.pushIndex = ~b;
		}
		port = line.spuPort;
		popTask = line.popWait[1];
		popWkl = line.popWaitWkl[1];
		pushTask = line.pushWait[1];
		pushWkl = line.pushWaitWkl[1];
	} while (!line_put(ea));

	if (throwPpu)
		throw_port_event(port);
	if (wakePop && (rc = wake(popTask, popWkl)) != 0)
		return rc;
	if (wakePush && (rc = wake(pushTask, pushWkl)) != 0)
		return rc;
	return 0;
}

int _cellSpursQueuePushBegin(uint64_t ea, const void *buffer, unsigned int tag,
                             unsigned isBlocking)
{
	return queue_begin(ea, (void *)buffer, tag, isBlocking, 1);
}

int cellSpursQueuePushEnd(uint64_t ea, unsigned int tag)
{
	return queue_end(ea, tag, 1, 0);
}

int _cellSpursQueuePopBegin(uint64_t ea, void *buffer, unsigned int tag,
                            unsigned isBlocking)
{
	return queue_begin(ea, buffer, tag, isBlocking, 0);
}

int _cellSpursQueuePopEnd(uint64_t ea, unsigned int tag, unsigned isPeek)
{
	return queue_end(ea, tag, 0, isPeek);
}

int cellSpursQueueSize(uint64_t ea, unsigned int *size)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!size)
		return (int)ERR_NULL_POINTER;
	line_get(ea);
	*size = entries(index_of(line.popIndex), index_of(line.pushIndex), line.depth);
	return 0;
}

int cellSpursQueueDepth(uint64_t ea, unsigned int *depth)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!depth)
		return (int)ERR_NULL_POINTER;
	line_get(ea);
	*depth = line.depth;
	return 0;
}

int cellSpursQueueGetEntrySize(uint64_t ea, unsigned int *entry_size)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!entry_size)
		return (int)ERR_NULL_POINTER;
	line_get(ea);
	*entry_size = line.entrySize;
	return 0;
}

int cellSpursQueueGetDirection(uint64_t ea, unsigned *direction)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!direction)
		return (int)ERR_NULL_POINTER;
	line_get(ea);
	*direction = line.direction;
	return 0;
}

int cellSpursQueueGetTasksetAddress(uint64_t ea, uint64_t *taskset)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!taskset)
		return (int)ERR_NULL_POINTER;
	line_get(ea);
	*taskset = line.taskset;
	return 0;
}

int cellSpursQueueClear(uint64_t ea)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	do {
		line_get(ea);
		if (in_flight(line.popIndex) || in_flight(line.pushIndex)
		    || line.popWait[0] || line.pushWait[0])
			return (int)ERR_BUSY;
		line.popIndex = 0;
		line.pushIndex = 0;
	} while (!line_put(ea));
	return 0;
}
