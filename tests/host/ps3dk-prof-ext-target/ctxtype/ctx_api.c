/* GPU API context type (P3.3 ctx-type fix), C callers, compile-only.  R4B_ORDER 1: <cell/gcm.h> before prof_gpu.h;
 * 2: after.  Every context-taking declaration and every ENABLED zone/mark macro is called with each C spelling of
 * the GCM context: gCellGcmCurrentContext, CellGcmContextData *, gcmContextData *.  R4B_NEG k (1..6) instead passes
 * an int * to declaration k (a negative control: the pointer diagnostic must fire).  R4B_OLD: compiled against the
 * sealed old header (ctxtype/old), the old defect must reproduce. */
#include <stdint.h>

#if R4B_ORDER == 2
#include "prof_gpu.h"
#include <cell/gcm.h>
#else
#include <cell/gcm.h>
#include "prof_gpu.h"
#endif

volatile int r4b_ctx_sink;

#ifdef R4B_NEG
static int bad_word;
#define BAD (&bad_word)
void r4b_ctx_neg(void)
{
    ps3dk_prof_gpu_zone z = PS3DK_PROF_GPU_ZONE_INIT;
    uint32_t g = 0;
#if R4B_NEG == 1
    r4b_ctx_sink += ps3dk_prof_gpu_attach(BAD, 0, 4, 64, &g);
#elif R4B_NEG == 2
    r4b_ctx_sink += ps3dk_prof_gpu_begin(g, BAD, 0, &z);
#elif R4B_NEG == 3
    r4b_ctx_sink += ps3dk_prof_gpu_end(g, BAD, &z);
#elif R4B_NEG == 4
    r4b_ctx_sink += ps3dk_prof_gpu_mark(g, BAD, 0);
#elif R4B_NEG == 5
    r4b_ctx_sink += ps3dk_prof_gpu_calibrate(g, BAD, 0);
#else
    r4b_ctx_sink += ps3dk_prof_gpu_detach(g, BAD, 0);
#endif
    (void)z;
}
#elif defined(R4B_OLD)
void r4b_ctx_old(void)
{
    uint32_t g = 0;
    r4b_ctx_sink += ps3dk_prof_gpu_attach(gCellGcmCurrentContext, 0, 4, 64, &g);
}
#else
static void all_calls(ps3dk_prof_gcm_ctx *ctx)
{
    ps3dk_prof_gpu_zone z = PS3DK_PROF_GPU_ZONE_INIT;
    uint32_t g = 0;
    r4b_ctx_sink += ps3dk_prof_gpu_attach(ctx, 0, 4, 64, &g);
    r4b_ctx_sink += PS3DK_PROF_GPU_ZONE_BEGIN(g, ctx, "zone", z);
    r4b_ctx_sink += PS3DK_PROF_GPU_ZONE_END(g, ctx, z);
    r4b_ctx_sink += PS3DK_PROF_GPU_MARK(g, ctx, "mark");
    r4b_ctx_sink += ps3dk_prof_gpu_end(g, ctx, &z);
    r4b_ctx_sink += ps3dk_prof_gpu_calibrate(g, ctx, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_detach(g, ctx, 0);
}

void r4b_ctx_c(CellGcmContextData *cell, gcmContextData *psl)
{
    ps3dk_prof_gpu_zone z = PS3DK_PROF_GPU_ZONE_INIT;
    uint32_t g = 0;
    /* each spelling straight into each declaration (no helper conversion in between) */
    r4b_ctx_sink += ps3dk_prof_gpu_attach(gCellGcmCurrentContext, 0, 4, 64, &g);
    r4b_ctx_sink += ps3dk_prof_gpu_attach(cell, 0, 4, 64, &g);
    r4b_ctx_sink += ps3dk_prof_gpu_attach(psl, 0, 4, 64, &g);
    r4b_ctx_sink += ps3dk_prof_gpu_begin(g, gCellGcmCurrentContext, 0, &z);
    r4b_ctx_sink += ps3dk_prof_gpu_begin(g, cell, 0, &z);
    r4b_ctx_sink += ps3dk_prof_gpu_begin(g, psl, 0, &z);
    r4b_ctx_sink += ps3dk_prof_gpu_end(g, gCellGcmCurrentContext, &z);
    r4b_ctx_sink += ps3dk_prof_gpu_end(g, cell, &z);
    r4b_ctx_sink += ps3dk_prof_gpu_end(g, psl, &z);
    r4b_ctx_sink += ps3dk_prof_gpu_mark(g, gCellGcmCurrentContext, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_mark(g, cell, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_mark(g, psl, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_calibrate(g, gCellGcmCurrentContext, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_calibrate(g, cell, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_calibrate(g, psl, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_detach(g, gCellGcmCurrentContext, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_detach(g, cell, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_detach(g, psl, 0);
    r4b_ctx_sink += PS3DK_PROF_GPU_ZONE_BEGIN(g, gCellGcmCurrentContext, "zone", z);
    r4b_ctx_sink += PS3DK_PROF_GPU_ZONE_END(g, cell, z);
    r4b_ctx_sink += PS3DK_PROF_GPU_MARK(g, psl, "mark");
    all_calls(gCellGcmCurrentContext);
}
#endif
