/* I3a link fixture TU1: three zones on live paths. */
#include "prof_spu_lane.h"

void tu1_run(ps3dk_prof_spu_writer *w);
void tu1_run(ps3dk_prof_spu_writer *w)
{
    uint32_t a = PS3DK_PROF_SPU_ZONE_BEGIN(w, "tu1_outer");
    uint32_t b = PS3DK_PROF_SPU_ZONE_BEGIN(w, "tu1_inner");
    PS3DK_PROF_SPU_ZONE_END(w, b);
    PS3DK_PROF_SPU_MARK(w, "tu1_mark");
    PS3DK_PROF_SPU_ZONE_END(w, a);
}
