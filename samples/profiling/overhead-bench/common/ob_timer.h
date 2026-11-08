/* overhead-bench: the timing boundaries (Plan A r2 section 2.1 with probe-design-r1 11.2 A-4).
 *
 * PPU boundary, identical at the start and the end of every timed block and in every TU:
 *   a compiler barrier, isync, mftb, isync.
 * What is claimed for it (cited in the A1 packet's TIMER.txt): Book II v2.02 3.3.1 defines isync as ensuring "that all
 * instructions preceding the isync instruction have completed before the isync instruction completes, and that no
 * subsequent instructions are initiated until after the isync instruction completes", and that it "may complete before
 * storage accesses associated with instructions preceding the isync instruction have been performed".  No document
 * consulted specifies when, within its execution, mftb samples the Time Base; that stays a stated premise, as do
 * stores still in flight at the end boundary.  The emitted sequence of every timed function is printed by the A1 audit;
 * no ordering guarantee beyond the cited text is claimed.
 *
 * SPU boundary: a compiler barrier, then the decrementer channel read (rdch SPU_RdDec).  SPU figures are decrementer
 * ticks only (Plan A r2 2.3).
 *
 * Host builds (the ledger model, the tests) read a host counter through ob_host_tb (set by the test). */
#ifndef OB_TIMER_H
#define OB_TIMER_H

#include <stdint.h>

#define OB_BARRIER() __asm__ __volatile__("" ::: "memory")
/* One per loop iteration in every TU (an empty volatile asm, no clobber, no operand): the loop of an OFF TU, whose
 * body is otherwise empty, is kept (GCC deletes an empty counted loop at -O2; the A1 audit found exactly that before
 * this was added), so OFF and OFF-b time the same loop as ON with the profiler calls removed. */
#define OB_ITER() __asm__ __volatile__("")
/* Before every bench loop in every TU: no unrolling, so ON and OFF run the same rolled loop (the A1 audit found GCC
 * unrolling the OFF loop by 4 at -O2 once OB_ITER kept it). */
#define OB_NO_UNROLL _Pragma("GCC unroll 1")

#if defined(__PPU__) || defined(__powerpc64__)
static inline __attribute__((always_inline)) uint64_t ob_tb(void)
{
    uint64_t v;
    OB_BARRIER();
    __asm__ __volatile__("isync\n\tmftb %0\n\tisync" : "=r"(v) : : "memory");
    return v;
}
#elif defined(__SPU__)
#include <spu_intrinsics.h>
static inline __attribute__((always_inline)) uint32_t ob_dec(void)
{
    OB_BARRIER();
    return spu_readch(SPU_RdDec);
}
#else
extern uint64_t (*ob_host_tb)(void);
static inline uint64_t ob_tb(void)
{
    OB_BARRIER();
    return ob_host_tb();
}
#endif

#endif
