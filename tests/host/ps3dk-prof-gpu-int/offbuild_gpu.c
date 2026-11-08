/* P3.3 R2 off build: with PS3DK_PROF = 0 the GPU macros are sizeof-only: they evaluate to ENOTSUP, never evaluate an
 * argument and never write the zone.  Compiled and RUN (C and C++); exit 0 only if every claim holds. */
#include <stdio.h>
#include <string.h>

#include "prof_gpu.h"

static int g_calls;
static ps3dk_prof_gpu_zone g_z[4];
static ps3dk_prof_gpu_zone *get_zone(void)
{
    g_calls++;
    return &g_z[0];
}
static unsigned get_gpu(void)
{
    g_calls++;
    return 1026u;
}

int main(void)
{
    int i = 0, bad = 0;
    unsigned char before[sizeof g_z];
    memset(g_z, 0xA5, sizeof g_z);
    memcpy(before, g_z, sizeof g_z);
    if (PS3DK_PROF_GPU_ZONE_BEGIN(get_gpu(), 0, "x", g_z[i++]) != PS3DK_PROF_ENOTSUP)
        bad++;
    if (PS3DK_PROF_GPU_ZONE_BEGIN(1026u, 0, "x", *get_zone()) != PS3DK_PROF_ENOTSUP)
        bad++;
    if (PS3DK_PROF_GPU_ZONE_END(get_gpu(), 0, g_z[i++]) != PS3DK_PROF_ENOTSUP)
        bad++;
    if (PS3DK_PROF_GPU_MARK(get_gpu(), 0, "y") != PS3DK_PROF_ENOTSUP)
        bad++;
    if (i != 0 || g_calls != 0 || memcmp(before, g_z, sizeof g_z) != 0)
        bad++;
    printf("offbuild-gpu: i %d calls %d zones %s: %s\n", i, g_calls, memcmp(before, g_z, sizeof g_z) ? "WRITTEN" : "untouched",
           bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
