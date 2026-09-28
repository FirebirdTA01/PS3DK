/*
 * lfqueue.c -- SPU-side cellSyncLFQueue* runtime.
 *
 * The queue is a 128-byte control block shared with the PPU side, plus a
 * ring of `depth` entries of `size` bytes.  Every control-block update is
 * a getllar/putllc read-modify-write of the whole block through the shared
 * lock line gCellSyncLockLine, retried when the reservation is lost.
 * Block layout (big-endian, offsets in bytes):
 *
 *   0x00 popCommit    u16  oldest index not yet released by a completed pop
 *   0x02 popDone      u16  out-of-order pop completions (bit 15 = popCommit)
 *   0x04 ppuPopWait   u16  bit 1: a PPU popper sleeps on its event queue
 *                          (ANY2ANY: the pop-waiter pack)
 *   0x06 popReserve   u16  next index handed to a popper
 *   0x08 pushCommit   u16  oldest index not yet committed by a completed push
 *   0x0a pushDone     u16  out-of-order push completions
 *   0x0c ppuPushWait  u16  bit 1: a PPU pusher sleeps (ANY2ANY: push-waiter pack)
 *   0x0e pushReserve  u16  next index handed to a pusher
 *   0x10 size u32, 0x14 depth u32, 0x18 buffer u64 (bit 0 set in ANY2ANY)
 *   0x20 port[4]      u8   SPU event ports to PPU sleepers (u32 ~0: none)
 *   0x24 direction u32, 0x28 v1 u32, 0x2c init u32 (2: initialized from SPU)
 *   0x30 popWaitPack  u16  + 0x32 popWaiter[15] u16   poppers waiting for data
 *   0x50 pushWaitPack u16  + 0x52 pushWaiter[15] u16  pushers waiting for space
 *   0x70 eaSignal u64 (passed to the wake callback), 0x78 v2, 0x7c eqId
 *
 * Indices run over 0 .. 2*depth-1; index p lives in ring slot p mod depth.
 * A waiter pack holds three 5-bit counters modulo 30 over its 15-entry id
 * array: registered (bits 0-4), resumed (5-9) and signalled (10-14).
 * Begin reserves an index and starts the entry DMA on the caller's tag;
 * End waits for the tag, marks the index complete, advances the commit
 * index over the completed run and wakes one waiter per index advanced.
 * Plain libsync callers never sleep: a blocking Begin spins.  The SPURS
 * layer passes wait/id/signal callbacks so its tasks sleep instead.  A PPU
 * thread sleeps on the event queue behind the queue's SPU event port; the
 * firmware PPU side does not always flag itself before sleeping, so End
 * throws the event whenever a PPU peer could be waiting on the transition
 * it made (empty -> not empty, full -> not full).
 * Independently written from the published object layout and protocol.
 */

#include <stdint.h>
#include <spu_mfcio.h>
#include <cell/sync.h>

#define ERR_AGAIN        ((int)0x80410101u)
#define ERR_INVAL        ((int)0x80410102u)
#define ERR_PERM         ((int)0x80410109u)
#define ERR_BUSY         ((int)0x8041010Au)
#define ERR_STAT         ((int)0x8041010Fu)
#define ERR_ALIGN        ((int)0x80410110u)
#define ERR_NULL_POINTER ((int)0x80410111u)

enum { SPU2SPU = 0, SPU2PPU = 1, PPU2SPU = 2, ANY2ANY = 3 };

#define NO_PORTS     0xffffffffu
#define PPU_WAITER   0xffffu      /* waiter id of a PPU thread on its event queue */
#define PPU_SLEEPING 0x0002u      /* bit in ppuPopWait / ppuPushWait */
#define MAX_WAITERS  15

typedef struct lfqueue {
	uint16_t popCommit, popDone, ppuPopWait, popReserve;
	uint16_t pushCommit, pushDone, ppuPushWait, pushReserve;
	uint32_t size;
	uint32_t depth;
	uint64_t buffer;
	uint8_t  port[4];
	uint32_t direction;
	uint32_t v1;
	uint32_t init;
	uint16_t popWaitPack;
	uint16_t popWaiter[15];
	uint16_t pushWaitPack;
	uint16_t pushWaiter[15];
	uint64_t eaSignal;
	uint32_t v2;
	uint32_t eqId;
} __attribute__((aligned(128))) lfqueue_t;

_Static_assert(sizeof(lfqueue_t) == 128, "control block is one line");
_Static_assert(__builtin_offsetof(lfqueue_t, port) == 0x20, "ports");
_Static_assert(__builtin_offsetof(lfqueue_t, init) == 0x2c, "init");
_Static_assert(__builtin_offsetof(lfqueue_t, popWaitPack) == 0x30, "pop waiters");
_Static_assert(__builtin_offsetof(lfqueue_t, pushWaitPack) == 0x50, "push waiters");
_Static_assert(__builtin_offsetof(lfqueue_t, eaSignal) == 0x70, "signal");

typedef int (*lfq_wait_fn)(uint64_t arg);
typedef uint32_t (*lfq_id_fn)(void);
typedef int (*lfq_signal_fn)(uint64_t eaSignal, uint32_t id);

int _cellSyncLFQueueWakeUp(uint32_t throwFlag, const uint32_t *ids, uint32_t count,
                           lfq_signal_fn signal, uint32_t allowThrow);

/* The lock line every control-block access goes through.  The transfer
 * helpers read size/depth/buffer from the snapshot the preceding
 * Get*Pointer left here. */
lfqueue_t gCellSyncLockLine;
#define Q (&gCellSyncLockLine)

static void line_get(uint64_t ea)
{
	mfc_getllar(Q, ea, 0, 0);
	(void)mfc_read_atomic_status();
	spu_dsync();
}

/* 1 when the conditional put kept the reservation */
static int line_put(uint64_t ea)
{
	spu_dsync();
	mfc_putllc(Q, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

static void halt(void)
{
	__asm__ volatile ("stopd $0, $0, $0");
	for (;;)
		;
}

/* ---- index and waiter-pack arithmetic --------------------------------- */

static int dist(int x, int y, int depth)   /* (x - y) mod 2*depth */
{
	return x >= y ? x - y : x - y + 2 * depth;
}

static int advance(int x, int by, int depth)
{
	x += by;
	return x >= 2 * depth ? x - 2 * depth : x;
}

static int pk_lo(uint16_t p)  { return p & 31; }
static int pk_mid(uint16_t p) { return (p >> 5) & 31; }
static int pk_hi(uint16_t p)  { return (p >> 10) & 31; }
static int mod30(int x)       { return x < 0 ? x + 30 : x; }
static int inc30(int c)       { return c == 29 ? 0 : c + 1; }
static int slot15(int c)      { return c <= 14 ? c : c - 15; }
static int pk_signalled(uint16_t p) { return mod30(pk_hi(p) - pk_mid(p)); }   /* A */
static int pk_registered(uint16_t p) { return mod30(pk_lo(p) - pk_hi(p)); }  /* B */

static uint16_t pk_set_lo(uint16_t p, int v)  { return (uint16_t)((p & ~0x001f) | v); }
static uint16_t pk_set_mid(uint16_t p, int v) { return (uint16_t)((p & 0xfc1f) | (v << 5)); }
static uint16_t pk_set_hi(uint16_t p, int v)  { return (uint16_t)((p & 0x83ff) | (v << 10)); }

/* In ANY2ANY the waiter packs live in the PPU-sleeper words */
static uint16_t *pop_pack(void)
{
	return Q->direction == ANY2ANY ? &Q->ppuPopWait : &Q->popWaitPack;
}

static uint16_t *push_pack(void)
{
	return Q->direction == ANY2ANY ? &Q->ppuPushWait : &Q->pushWaitPack;
}

/* an SPU event port connects the queue to a PPU thread's event queue */
static int ports_attached(void)
{
	uint32_t ports;
	__builtin_memcpy(&ports, Q->port, sizeof ports);
	return ports != NO_PORTS;
}

static int check_ea(uint64_t ea)
{
	if (!ea)
		return ERR_NULL_POINTER;
	if (ea & 0x7f)
		return ERR_ALIGN;
	return 0;
}

/* ---- initialization and queries ----------------------------------------- */

int cellSyncLFQueueInitialize(uint64_t ea, uint64_t buffer, unsigned int size,
                              unsigned int depth, CellSyncQueueDirection direction,
                              uint64_t eaSignal)
{
	unsigned i;
	if (!ea || (size && !buffer))
		return ERR_NULL_POINTER;
	if ((size & 15) || !depth || size > 0x4000 || (depth & 0xffff8000u)
	    || (unsigned)direction > ANY2ANY)
		return ERR_INVAL;
	if ((ea & 0x7f) || (buffer & 15))
		return ERR_ALIGN;

	for (;;) {
		line_get(ea);
		if (Q->init == 1)
			return ERR_BUSY;
		if (Q->init == 0) {
			const volatile uint8_t *b = (const volatile uint8_t *)Q;
			for (i = 0; i < sizeof *Q; ++i)
				if (b[i])
					return ERR_STAT;
			Q->size = size;
			Q->depth = depth;
			Q->direction = direction;
			Q->eaSignal = eaSignal;
			Q->init = 2;
			if ((unsigned)direction == ANY2ANY) {
				Q->buffer = buffer | 1;
				Q->port[0] = Q->port[1] = 0xff;
				Q->v1 = 0xffffffffu;
				Q->popWaitPack = Q->pushWaitPack = 0xffff;
			} else {
				Q->buffer = buffer;
				Q->port[0] = Q->port[1] = Q->port[2] = Q->port[3] = 0xff;
			}
		} else if (Q->init == 2) {
			/* already initialized: only an identical request succeeds */
			if (Q->size != size || Q->depth != depth || Q->buffer != buffer
			    || Q->eaSignal != eaSignal
			    || (Q->direction != (uint32_t)direction && (unsigned)direction != SPU2SPU))
				return ERR_INVAL;
		} else {
			return ERR_STAT;
		}
		if (line_put(ea))
			return 0;
	}
}

int cellSyncLFQueueSize(uint64_t ea, unsigned int *size)
{
	if (!ea || !size)
		return ERR_NULL_POINTER;
	if (ea & 0x7f)
		return ERR_ALIGN;
	line_get(ea);
	*size = (unsigned)dist(Q->pushCommit, Q->popCommit, (int)Q->depth);
	return 0;
}

int cellSyncLFQueueDepth(uint64_t ea, unsigned int *depth)
{
	if (!ea || !depth)
		return ERR_NULL_POINTER;
	if (ea & 0x7f)
		return ERR_ALIGN;
	line_get(ea);
	*depth = Q->depth;
	return 0;
}

int cellSyncLFQueueGetDirection(uint64_t ea, CellSyncQueueDirection *direction)
{
	if (!ea || !direction)
		return ERR_NULL_POINTER;
	if (ea & 0x7f)
		return ERR_ALIGN;
	line_get(ea);
	*direction = (CellSyncQueueDirection)Q->direction;
	return 0;
}

int cellSyncLFQueueGetEntrySize(uint64_t ea, unsigned int *pSize)
{
	if (!ea || !pSize)
		return ERR_NULL_POINTER;
	if (ea & 0x7f)
		return ERR_ALIGN;
	line_get(ea);
	*pSize = Q->size;
	return 0;
}

int _cellSyncLFQueueGetSignalAddress(uint64_t ea, uint64_t *pSignal)
{
	if (!ea || !pSignal)
		return ERR_NULL_POINTER;
	if (ea & 0x7f)
		return ERR_ALIGN;
	line_get(ea);
	*pSignal = Q->eaSignal;
	return 0;
}

int cellSyncLFQueueClear(uint64_t ea)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	do {
		line_get(ea);
		if (Q->direction == ANY2ANY) {
			Q->popWaitPack = Q->ppuPopWait;
			Q->pushWaitPack = Q->ppuPushWait;
		}
		if (Q->popReserve != Q->popCommit || Q->pushReserve != Q->pushCommit
		    || pk_hi(Q->popWaitPack) != pk_lo(Q->popWaitPack)
		    || pk_hi(Q->pushWaitPack) != pk_lo(Q->pushWaitPack))
			return ERR_BUSY;
		Q->popCommit = Q->popDone = Q->popReserve = 0;
		Q->pushCommit = Q->pushDone = Q->pushReserve = 0;
	} while (!line_put(ea));
	return 0;
}

/* ---- reservation --------------------------------------------------------- */

/* Reserve the next push (push != 0) or pop index.  Without waitFn a caller
 * that must wait gets AGAIN; with it, the caller registers its id and
 * sleeps in waitFn(ea | 5), and a completer reserves on its behalf. */
static int get_pointer(uint64_t ea, int *pPointer, lfq_wait_fn wait, lfq_id_fn id, int push)
{
	uint16_t myId = id ? (uint16_t)id() : 0;
	int woken = 0;

	for (;;) {
		int depth, h1, h4, h5, h8, used, out, a, b, mustWait, grant = 0, sleep = 0, index;
		uint16_t *pack, *waiter;
		line_get(ea);
		if (Q->direction == (uint32_t)(push ? PPU2SPU : SPU2PPU))
			return ERR_PERM;
		depth = (int)Q->depth;
		h1 = Q->popCommit;
		h4 = (int16_t)Q->popReserve;
		h5 = Q->pushCommit;
		h8 = (int16_t)Q->pushReserve;
		pack = push ? push_pack() : pop_pack();
		waiter = push ? Q->pushWaiter : Q->popWaiter;
		a = pk_signalled(*pack);
		b = pk_registered(*pack);
		if (push) {
			used = dist(h8, h1, depth);          /* slots taken by pushers */
			out = dist(h8, h5, depth);           /* pushes not yet committed */
			mustWait = woken ? (used >= depth || out > 15)
			                 : (used + a >= depth || out + a > 15 || b);
		} else {
			used = dist(h5, h4, depth);          /* committed, unreserved entries */
			out = dist(h4, h1, depth);           /* pops not yet committed */
			mustWait = woken ? (used <= 0 || out > 15)
			                 : (used - a <= 0 || out + a > 15 || b);
		}
		index = push ? h8 : h4;

		if (woken) {
			if (!mustWait) {
				*pack = pk_set_mid(*pack, inc30(pk_mid(*pack)));
				grant = 1;
			}
		} else if (!mustWait) {
			grant = 1;
		} else if (!wait) {
			*pPointer = index;
			return ERR_AGAIN;
		} else if (a + b >= MAX_WAITERS) {
			if (!line_put(ea))
				continue;
			*pPointer = index;
			return ERR_AGAIN;
		} else {
			int lo = pk_lo(*pack);
			waiter[slot15(lo)] = myId;
			*pack = pk_set_lo(*pack, inc30(lo));
			sleep = 1;
		}
		if (grant) {
			if (push)
				Q->pushReserve = (uint16_t)advance(h8, 1, depth);
			else
				Q->popReserve = (uint16_t)advance(h4, 1, depth);
		}
		if (!line_put(ea))
			continue;
		*pPointer = index;
		if (grant)
			return 0;
		if (sleep) {
			int rc = wait(ea | 5);
			if (rc)
				return rc;
			woken = 1;
		}
	}
}

int _cellSyncLFQueueGetPushPointer(uint64_t ea, int *pPointer, lfq_wait_fn wait, lfq_id_fn id)
{
	return get_pointer(ea, pPointer, wait, id, 1);
}

int _cellSyncLFQueueGetPopPointer(uint64_t ea, int *pPointer, lfq_wait_fn wait, lfq_id_fn id)
{
	return get_pointer(ea, pPointer, wait, id, 0);
}

/* ---- completion ---------------------------------------------------------- */

/* Mark `pointer` complete in the commit/done pair and return how far the
 * commit index advanced over the contiguous run of completed indices. */
static int complete(uint16_t *commit, uint16_t *done, int pointer, int depth)
{
	int var = dist(pointer, *commit, depth), n = 0;
	uint32_t bm = *done;
	if (var <= 15)
		bm |= 1u << (15 - var);
	while (n < 16 && (bm & (0x8000u >> n)))
		++n;
	*commit = (uint16_t)advance(*commit, n, depth);
	*done = (uint16_t)((bm << n) & 0xffff);
	return n;
}

/* Take the next signalled waiter off a list: its id, or for a PPU thread
 * sleeping on its event queue, 0xff00 | its event port. */
static uint32_t signal_next(uint16_t *pack, const uint16_t *waiter, uint8_t port)
{
	int hi = pk_hi(*pack);
	uint16_t id = waiter[slot15(hi)];
	*pack = pk_set_hi(*pack, inc30(hi));
	return id == PPU_WAITER ? (0xff00u | port) : id;
}

static int complete_pointer(uint64_t ea, int pointer, lfq_signal_fn signal,
                            uint32_t allowThrow, uint32_t arg5, int push)
{
	uint32_t list[32], count, throwFlag;

	for (;;) {
		int depth, h1, h4, h5, h8, n, i, popCand, pushCand, popRoom, pushRoom;
		uint16_t *pp, *up;
		line_get(ea);
		if (Q->direction == (uint32_t)(push ? PPU2SPU : SPU2PPU))
			return ERR_PERM;
		depth = (int)Q->depth;
		h1 = Q->popCommit;
		h4 = (int16_t)Q->popReserve;
		h5 = Q->pushCommit;
		h8 = (int16_t)Q->pushReserve;
		throwFlag = 0;
		count = 0;
		pp = pop_pack();
		up = push_pack();

		if (push) {
			n = complete(&Q->pushCommit, &Q->pushDone, pointer, depth);
			if (h5 == h8)
				Q->pushReserve = Q->pushCommit;
			h8 = (int16_t)Q->pushReserve;
			/* a PPU popper may be asleep on its event queue: the queue had no
			   unreserved entries and either the sleeper flagged itself or an
			   event port is attached (the firmware PPU side sleeps without
			   publishing the flag) */
			if (Q->direction == SPU2PPU && h5 == h4
			    && ((Q->ppuPopWait & PPU_SLEEPING) || ports_attached())) {
				Q->ppuPopWait &= (uint16_t)~PPU_SLEEPING;
				throwFlag = 1;
			}
			popRoom = dist(h4, h1, depth) + pk_signalled(*pp);      /* popOut + A, <= 15 */
			popCand = popRoom <= 15 ? pk_registered(*pp) : 0;
			pushRoom = dist(h8, h1, depth) + pk_signalled(*up);     /* fill + A, < depth */
			pushCand = (depth > pushRoom || arg5) ? pk_registered(*up) : 0;
			for (i = 0; i < n; ++i) {
				if (popCand > 0 && popRoom <= 15) {
					list[count++] = signal_next(pp, Q->popWaiter, Q->port[0]);
					--popCand;
					++popRoom;
				}
				if (pushCand > 0 && (depth > pushRoom || arg5)) {
					list[count++] = signal_next(up, Q->pushWaiter, Q->port[1]);
					--pushCand;
					++pushRoom;
				}
			}
		} else {
			/* a PPU pusher may be asleep on a full queue (see above) */
			if (Q->direction == PPU2SPU && dist(h8, h1, depth) == depth
			    && ((Q->ppuPushWait & PPU_SLEEPING) || ports_attached())) {
				Q->ppuPushWait &= (uint16_t)~PPU_SLEEPING;
				throwFlag = 1;
			}
			n = complete(&Q->popCommit, &Q->popDone, pointer, depth);
			popRoom = dist(h5, h4, depth) - pk_signalled(*pp);      /* avail - A, > 0 */
			popCand = popRoom > 0 ? pk_registered(*pp) : 0;
			pushRoom = dist(h8, h5, depth) + pk_signalled(*up);     /* pushOut + A, <= 15 */
			pushCand = (pushRoom <= 15 && !arg5) ? pk_registered(*up) : 0;
			for (i = 0; i < n; ++i) {
				if (popCand > 0 && popRoom > 0) {
					list[count++] = signal_next(pp, Q->popWaiter, Q->port[0]);
					--popCand;
					--popRoom;
				}
				if (pushCand > 0 && pushRoom <= 15) {
					list[count++] = signal_next(up, Q->pushWaiter, Q->port[1]);
					--pushCand;
					++pushRoom;
				}
			}
		}
		if (line_put(ea))
			break;
	}
	return _cellSyncLFQueueWakeUp(throwFlag, list, count, signal, allowThrow);
}

int _cellSyncLFQueueCompletePushPointer(uint64_t ea, int pointer, lfq_signal_fn signal,
                                        uint32_t allowThrow, uint32_t arg5)
{
	return complete_pointer(ea, pointer, signal, allowThrow, arg5, 1);
}

int _cellSyncLFQueueCompletePopPointer(uint64_t ea, int pointer, lfq_signal_fn signal,
                                       uint32_t allowThrow, uint32_t noQueueFull)
{
	return complete_pointer(ea, pointer, signal, allowThrow, noQueueFull, 0);
}

/* Fire-and-forget SPU-thread event to the PPU queue behind `port` */
static void throw_event(uint32_t port)
{
	port &= 0xff;
	if (port > 63)
		return;
	spu_writech(SPU_WrOutMbox, 0);
	spu_writech(SPU_WrOutIntrMbox, (port << 24) | 0x40000000u);
}

/* Wake what a completion collected: a PPU sleeper flagged in the control
 * block (throwFlag), PPU threads on their event queues, and waiters that
 * `signal` wakes.  Reads eaSignal and the ports from the lock line. */
int _cellSyncLFQueueWakeUp(uint32_t throwFlag, const uint32_t *ids, uint32_t count,
                           lfq_signal_fn signal, uint32_t allowThrow)
{
	uint32_t ports, i;
	int thrown = 0, rc;
	__builtin_memcpy(&ports, Q->port, sizeof ports);
	if (throwFlag && allowThrow) {
		if (ports == NO_PORTS)
			halt();
		throw_event(ports);
		thrown = 1;
	}
	if (count && !signal)
		return ERR_PERM;
	for (i = 0; i < count; ++i) {
		uint32_t e = ids[i];
		if ((e & 0xff00) == 0xff00) {
			if ((e & 0xff) == 0xff || thrown || !allowThrow)
				halt();
			throw_event(e);
		} else if ((rc = signal(Q->eaSignal, e)) != 0) {
			return rc;
		}
	}
	return 0;
}

/* ---- entry transfer ------------------------------------------------------ */

static uint64_t entry_ea(int pointer)
{
	int depth = (int)Q->depth;
	int slot = pointer >= depth ? pointer - depth : pointer;
	return (Q->buffer & ~1ull) + (uint64_t)(int64_t)(int32_t)((uint32_t)slot * Q->size);
}

int _cellSyncLFQueuePutTransfer(const void *ls, int pointer, unsigned int tag)
{
	mfc_put((volatile void *)ls, entry_ea(pointer), Q->size, tag, 0, 0);
	return 0;
}

int _cellSyncLFQueueGetTransfer(void *ls, int pointer, unsigned int tag)
{
	mfc_get(ls, entry_ea(pointer), Q->size, tag, 0, 0);
	return 0;
}

/* ---- public push/pop (spinning) ----------------------------------------- */

static int check_container(uint64_t ea, const void *c, const void *buffer, unsigned tag)
{
	if (!ea || !c || !buffer)
		return ERR_NULL_POINTER;
	if (((uintptr_t)buffer & 15) || (ea & 0x7f))
		return ERR_ALIGN;
	if (tag > 31)
		return ERR_INVAL;
	return 0;
}

static void wait_tag(unsigned tag)
{
	mfc_write_tag_mask(1u << tag);
	(void)mfc_read_tag_status_all();
}

int _cellSyncLFQueuePushBeginBody(uint64_t ea, CellSyncLFQueuePushContainer *c,
                                  unsigned int isBlocking)
{
	int rc = check_container(ea, c, c ? c->buffer : 0, c ? c->tag : 0);
	if (rc)
		return rc;
	do
		rc = _cellSyncLFQueueGetPushPointer(ea, &c->pointer, 0, 0);
	while (isBlocking && rc == ERR_AGAIN);
	if (rc)
		return rc;
	return _cellSyncLFQueuePutTransfer(c->buffer, c->pointer, c->tag);
}

int cellSyncLFQueuePushEnd(uint64_t ea, CellSyncLFQueuePushContainer *c)
{
	int rc = check_container(ea, c, c ? c->buffer : 0, c ? c->tag : 0);
	if (rc)
		return rc;
	wait_tag(c->tag);
	return _cellSyncLFQueueCompletePushPointer(ea, c->pointer, 0, 0, 0);
}

int _cellSyncLFQueuePopBeginBody(uint64_t ea, CellSyncLFQueuePopContainer *c,
                                 unsigned int isBlocking)
{
	int rc = check_container(ea, c, c ? c->buffer : 0, c ? c->tag : 0);
	if (rc)
		return rc;
	do
		rc = _cellSyncLFQueueGetPopPointer(ea, &c->pointer, 0, 0);
	while (isBlocking && rc == ERR_AGAIN);
	if (rc)
		return rc;
	return _cellSyncLFQueueGetTransfer(c->buffer, c->pointer, c->tag);
}

int cellSyncLFQueuePopEnd(uint64_t ea, CellSyncLFQueuePopContainer *c)
{
	int rc = check_container(ea, c, c ? c->buffer : 0, c ? c->tag : 0);
	if (rc)
		return rc;
	wait_tag(c->tag);
	return _cellSyncLFQueueCompletePopPointer(ea, c->pointer, 0, 0, 0);
}
