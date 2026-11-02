// I3a C++17 caller: the enabled prof_spu_lane.h from C++, linked against the C writer and core.  Linked only.
#include "prof_spu_lane.h"

static ps3dk_prof_spu_writer g_w __attribute__((aligned(PS3DK_PROF_SPU_WRITER_ALIGN)));
static uint8_t g_stage[PS3DK_PROF_SPU_STAGING_BYTES(16)] __attribute__((aligned(16)));

int main(unsigned long long spe, unsigned long long argp, unsigned long long envp)
{
    (void)spe;
    if (ps3dk_prof_spu_attach(&g_w, argp, 1024, envp, 7, g_stage, 16, 0) != PS3DK_PROF_SPU_OK)
        return 1;
    uint32_t z = PS3DK_PROF_SPU_ZONE_BEGIN(&g_w, "cxx_zone");
    PS3DK_PROF_SPU_MARK(&g_w, "cxx_mark");
    PS3DK_PROF_SPU_ZONE_END(&g_w, z);
    return ps3dk_prof_spu_flush(&g_w) == PS3DK_PROF_SPU_OK && ps3dk_prof_spu_fini(&g_w) == PS3DK_PROF_SPU_OK ? 0 : 1;
}
