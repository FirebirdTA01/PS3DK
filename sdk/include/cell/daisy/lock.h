/* cell/daisy/lock.h - the 128-byte state QueueControl::Local instances of
 * one stream share.
 *
 * Zero it, then construct the queue controls on it; the first constructor
 * sets it up.  On the SPU it lives in local store and every end runs on that
 * SPU; on the PPU it lives in main memory and the ends may be several PPU
 * threads, so the counters are updated atomically there.
 *
 * Entry numbers count up without wrapping in the queue: entry n uses buffer
 * slot n % depth.  A producer may reserve entry n while n - freed < depth; a
 * consumer may reserve entry n while n < published.  Completions are
 * published in entry order. */
#ifndef PS3TC_CELL_DAISY_LOCK_H
#define PS3TC_CELL_DAISY_LOCK_H

#include <cell/daisy/daisy_defs.h>
#ifndef __SPU__
#include <sys/ppu_thread.h>
#endif

namespace cell {
namespace Daisy {

struct Lock {
	volatile uint32_t initialized;
	volatile uint32_t depth;
	volatile uint32_t reservedPush;    /* next entry a producer reserves */
	volatile uint32_t published;       /* entries whose push completed */
	volatile uint32_t reservedPop;     /* next entry a consumer reserves */
	volatile uint32_t freed;           /* entries whose pop completed */
	volatile uint32_t producers;       /* producers attached and not terminated */
	volatile uint32_t consumers;       /* consumers attached and not detached */
	volatile uint32_t producerSeen;    /* a producer has attached at least once */
	uint32_t reserved[23];
} __attribute__((aligned(16)));

namespace LockOps {

#ifdef __SPU__
/* One SPU runs every end: plain updates. */
static inline uint32_t load(volatile uint32_t *p) { return *p; }
static inline void store(volatile uint32_t *p, uint32_t v) { *p = v; }
static inline bool cas(volatile uint32_t *p, uint32_t expect, uint32_t v)
{
	if (*p != expect)
		return false;
	*p = v;
	return true;
}
static inline void add(volatile uint32_t *p, int32_t d) { *p = *p + (uint32_t)d; }
static inline void relax() { }
#else
static inline uint32_t load(volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void store(volatile uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline bool cas(volatile uint32_t *p, uint32_t expect, uint32_t v)
{
	return __atomic_compare_exchange_n(p, &expect, v, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static inline void add(volatile uint32_t *p, int32_t d) { __atomic_add_fetch(p, (uint32_t)d, __ATOMIC_ACQ_REL); }
/* waiting on another PPU thread: let it run */
static inline void relax() { sys_ppu_thread_yield(); }
#endif

/* the first queue control on a zeroed Lock sets it up */
static inline void attach(Lock &lock, SizeType depth)
{
	if (cas(&lock.initialized, 0, 1))
		store(&lock.depth, depth);
}

} /* namespace LockOps */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_LOCK_H */
