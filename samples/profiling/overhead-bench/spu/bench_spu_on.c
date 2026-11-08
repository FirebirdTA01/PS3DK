/* overhead-bench SPU TU "on": the shared loop source on the lane writer (PS3DK_PROF=1). */
#include <stdint.h>

#include "../common/ob_timer.h"
#include "prof_spu_lane.h"

extern ps3dk_prof_spu_writer ob_w;
#define OB_TU on
#define OB_DEC() ob_dec()
#define OB_SB(name) PS3DK_PROF_SPU_ZONE_BEGIN(&ob_w, name)
#define OB_SE(z) PS3DK_PROF_SPU_ZONE_END(&ob_w, z)
#define OB_SM(name) PS3DK_PROF_SPU_MARK(&ob_w, name)
#define OB_FLUSH() ps3dk_prof_spu_flush(&ob_w)
#include "bench_spu.inc"
