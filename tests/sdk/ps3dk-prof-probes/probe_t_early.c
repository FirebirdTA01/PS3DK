/* PROBE-T-early (TARGET-PLAN-r2 section 2): a STANDALONE defined memory-access fixture, independent of the PAL and of
 * the profiler objects.  It has the shape of the core's p_tb (prof_core.c): a compiler barrier, one full-width timebase
 * read, a compiler barrier, with a store before and a load after.  Compiler barriers are not instructions, so the
 * evidence is the source, the RTL dump and the final assembly (EXPECTED.md), never a barrier "seen in objdump".
 * Guarantee stated: NO hardware serialization and NO register-only-motion guarantee; only that the compiler kept this
 * fixture's memory accesses on their side of the barriers.  Not wired into any build. */
#include <stdint.h>

volatile uint32_t t_before;   /* volatile: the access itself is not removed, so its position is observable */
uint32_t t_after_src = 7, t_after;

uint64_t probe_t_timebase_between_barriers(uint32_t v)
{
    uint64_t tb;
    t_before = v;                              /* ACCESS-BEFORE */
    __asm__ __volatile__("" ::: "memory");     /* BARRIER-1 */
    tb = __builtin_ppc_get_timebase();         /* READ */
    __asm__ __volatile__("" ::: "memory");     /* BARRIER-2 */
    t_after = t_after_src;                     /* ACCESS-AFTER: plain accesses, free to move without BARRIER-2 */
    return tb;
}

/* Control: the same accesses with no barriers.  Whatever order the compiler picks is recorded, not judged: the control
 * only shows that the barriers are the variable under test. */
uint64_t probe_t_control_no_barriers(uint32_t v)
{
    uint64_t tb;
    t_before = v;
    tb = __builtin_ppc_get_timebase();
    t_after = t_after_src;
    return tb;
}
