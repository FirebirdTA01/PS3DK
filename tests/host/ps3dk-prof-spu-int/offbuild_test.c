/* The off build of the SPU lane extension macros (prof_spu.h): with PS3DK_PROF undefined or 0, every macro yields
 * PS3DK_PROF_ENOTSUP, evaluates none of its arguments and references no runtime symbol.  Built as C and C++ and
 * linked WITHOUT the runtime (an undefined reference fails the link); the driver also checks the object's undefined
 * symbols for ps3dk_prof_.
 *
 * usage: offbuild (exit 0 and "OFFBUILD PASS") */
#include <stdio.h>

#include "prof_spu.h"

#if defined(PS3DK_PROF) && PS3DK_PROF
#error "the off-build test must be built with PS3DK_PROF undefined or 0"
#endif

static int g_calls;
static int side(void) { return ++g_calls; }

int main(void)
{
    int n = 0, bad = 0;
    ps3dk_prof_config c;
    ps3dk_prof_ext_config x;
    ps3dk_prof_spu_desc d[1];
    ps3dk_prof_spu_lane l;
    ps3dk_prof_spu_lane_stats st;
    uint32_t img = 0;
    int r[6];
    r[0] = PS3DK_PROF_INIT_EX((n++, &c), (n++, &x));
    r[1] = PS3DK_PROF_SPU_IMAGE_REGISTER_TABLE((n++, (const uint8_t *)"b"), (uint32_t)side(), (n++, d), (uint32_t)n++,
                                               (n++, &img));
    r[2] = PS3DK_PROF_SPU_LANE_CREATE((n++, "x"), (uint32_t)side(), side(), (n++, &l));
    r[3] = PS3DK_PROF_SPU_LANE_END((uint32_t)side(), (uint64_t)n++, (n++, &st));
    r[4] = PS3DK_PROF_SPU_LANE_STATS((uint32_t)n++, (n++, &st));
    r[5] = PS3DK_PROF_SPU_IMAGE_REGISTER_ELF((n++, (const void *)d), (uint64_t)side(), (n++, &img));
    for (int i = 0; i < 6; i++)
        if (r[i] != PS3DK_PROF_ENOTSUP) {
            printf("OFFBUILD FAIL macro %d returned %d\n", i, r[i]);
            bad = 1;
        }
    if (n != 0 || g_calls != 0) {
        printf("OFFBUILD FAIL arguments were evaluated (n %d, calls %d)\n", n, g_calls);
        bad = 1;
    }
    if (bad)
        return 1;
    printf("OFFBUILD PASS\n");
    return 0;
}
