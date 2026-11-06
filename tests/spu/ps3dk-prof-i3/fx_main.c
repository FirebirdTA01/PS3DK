/* I3a link fixture main: attach, the zones, flush, dec_write, fini.  Linked only; never executed in I3a. */
#include "prof_spu_lane.h"

void tu1_run(ps3dk_prof_spu_writer *w);
void tu2_run(ps3dk_prof_spu_writer *w);

static ps3dk_prof_spu_writer g_w __attribute__((aligned(PS3DK_PROF_SPU_WRITER_ALIGN)));
static uint8_t g_stage[PS3DK_PROF_SPU_STAGING_BYTES(64)] __attribute__((aligned(16)));

int main(unsigned long long spe, unsigned long long argp, unsigned long long envp)
{
    int rc;
    (void)spe;
    rc = ps3dk_prof_spu_attach(&g_w, argp, 1024, envp, 5, g_stage, 64, PS3DK_PROF_SPU_OWN_DEC);
    if (rc != PS3DK_PROF_SPU_OK)
        return rc;
    tu1_run(&g_w);
    tu2_run(&g_w);
    rc = ps3dk_prof_spu_flush(&g_w);
    if (rc == PS3DK_PROF_SPU_OK)
        rc = ps3dk_prof_spu_dec_write(&g_w, 0xFFFFFFF0u);
    return ps3dk_prof_spu_fini(&g_w) ? 1 : rc;
}
