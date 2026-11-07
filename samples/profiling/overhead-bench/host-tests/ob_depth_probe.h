/* host ledger model only: the zone depth at the innermost point of a bench case (prof_test_hooks.h) */
#include <stdint.h>
#include "prof_test_hooks.h"
extern uint64_t g_max_depth;
static inline void ob_depth_probe(void)
{
    uint64_t d = ps3dk_prof_test_read(PS3DK_PROF_TEST_DEPTH);
    if (d > g_max_depth)
        g_max_depth = d;
}
