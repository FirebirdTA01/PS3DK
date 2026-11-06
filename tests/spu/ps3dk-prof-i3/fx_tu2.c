/* I3a link fixture TU2: one zone on a live path, one in a function reached only through a pointer. */
#include "prof_spu_lane.h"

void tu2_run(ps3dk_prof_spu_writer *w);
static void tu2_via_ptr(ps3dk_prof_spu_writer *w)
{
    uint32_t z = PS3DK_PROF_SPU_ZONE_BEGIN(w, "tu2_via_ptr");
    PS3DK_PROF_SPU_ZONE_END(w, z);
}
void (*volatile tu2_fp)(ps3dk_prof_spu_writer *) = tu2_via_ptr;
void tu2_run(ps3dk_prof_spu_writer *w)
{
    PS3DK_PROF_SPU_MARK(w, "tu2_live");
    tu2_fp(w);
}
