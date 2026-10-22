/* PROBE-O-early (TARGET-PLAN-r2 section 2): a STANDALONE fixture, independent of the PAL and of the profiler objects.
 * It uses the same GCC __atomic builtins and memory orders as sdk/libps3dkprof/src/core/prof_internal.h (LD_ACQ,
 * ST_REL, LD_RLX, ST_RLX, CAS_ACQREL) on 4- and 8-byte objects of the widths the core uses: u32 for the gate, the
 * latches and ack_retire, u64 for write_idx, read_idx and current_frame.  Each function is one edge; the evidence is
 * read per function from the final assembly (EXPECTED.md).  It shows the compiler's lowering of these builtins under
 * the stated flags only, NOT the code in prof_core.o / prof_drain.o (that is PROBE-O-actual, after pal_lv2).
 * Not wired into any build: compiling it needs its own frozen request. */
#include <stdint.h>

uint32_t g32_flag, g32_data;
uint64_t g64_idx, g64_data;

/* RELEASE store publishing data written before it (ST_REL, e.g. desc_count / ack_retire). */
void probe_o_release_store_u32(uint32_t v)
{
    __atomic_store_n(&g32_data, v, __ATOMIC_RELAXED);
    __atomic_store_n(&g32_flag, 1u, __ATOMIC_RELEASE);
}

/* RELEASE store of a 64-bit index (ST_REL, e.g. write_idx / read_idx / current_frame). */
void probe_o_release_store_u64(uint64_t v)
{
    __atomic_store_n(&g64_data, v, __ATOMIC_RELAXED);
    __atomic_store_n(&g64_idx, v + 1u, __ATOMIC_RELEASE);
}

/* ACQUIRE load followed by a dependent read (LD_ACQ, e.g. stop_latch / desc_count). */
uint32_t probe_o_acquire_load_u32(void)
{
    if (__atomic_load_n(&g32_flag, __ATOMIC_ACQUIRE) == 0)
        return 0;
    return __atomic_load_n(&g32_data, __ATOMIC_RELAXED);
}

/* ACQUIRE load of a 64-bit index followed by a dependent read (LD_ACQ, e.g. write_idx). */
uint64_t probe_o_acquire_load_u64(void)
{
    uint64_t i = __atomic_load_n(&g64_idx, __ATOMIC_ACQUIRE);
    if (i == 0)
        return 0;
    return __atomic_load_n(&g64_data, __ATOMIC_RELAXED);
}

/* Strong CAS, ACQ_REL on success and ACQUIRE on failure (CAS_ACQREL: the gate, drain_busy, shutdown_once; the core
 * CASes only u32 objects). */
int probe_o_cas_acqrel_u32(uint32_t expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(&g32_flag, &expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* RELAXED 64-bit fetch_add (the core's unregistered_refusals counter). */
uint64_t probe_o_fetch_add_relaxed_u64(void) { return __atomic_fetch_add(&g64_data, 1u, __ATOMIC_RELAXED); }

/* The core's static checks (PROBE-L reads the same two). */
_Static_assert(__atomic_always_lock_free(4, 0), "4-byte atomics must be lock-free");
_Static_assert(__atomic_always_lock_free(8, 0), "8-byte atomics must be lock-free");
