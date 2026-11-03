// GPU API context type (P3.3 ctx-type fix), C++ callers.  R4B_ORDER 1/2 as ctx_api.c.  Each C++ spelling of the GCM
// context goes straight into every context-taking declaration and the enabled zone/mark macros:
// gCellGcmCurrentContext, CellGcmContextData *, struct CellGcmContextData *, gcmContextData * (CellGcmContextData is
// DERIVED from gcmContextData in C++, so its pointers convert to the base; nothing converts back).  R4B_NEG k: an
// int * to declaration k.  R4B_MAIN: a main() so the object links against the C runtime archive (extern "C"
// boundary; never run).
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
void r4b_ctx_neg()
{
    ps3dk_prof_gpu_zone z = PS3DK_PROF_GPU_ZONE_INIT;
    uint32_t g = 0;
#if R4B_NEG == 1
    r4b_ctx_sink += ps3dk_prof_gpu_attach(&bad_word, 0, 4, 64, &g);
#elif R4B_NEG == 2
    r4b_ctx_sink += ps3dk_prof_gpu_begin(g, &bad_word, nullptr, &z);
#elif R4B_NEG == 3
    r4b_ctx_sink += ps3dk_prof_gpu_end(g, &bad_word, &z);
#elif R4B_NEG == 4
    r4b_ctx_sink += ps3dk_prof_gpu_mark(g, &bad_word, nullptr);
#elif R4B_NEG == 5
    r4b_ctx_sink += ps3dk_prof_gpu_calibrate(g, &bad_word, 0);
#else
    r4b_ctx_sink += ps3dk_prof_gpu_detach(g, &bad_word, 0);
#endif
    (void)z;
}
#else
template <typename P> static void each(P ctx)
{
    ps3dk_prof_gpu_zone z = PS3DK_PROF_GPU_ZONE_INIT;
    uint32_t g = 0;
    r4b_ctx_sink += ps3dk_prof_gpu_attach(ctx, 0, 4, 64, &g);
    r4b_ctx_sink += ps3dk_prof_gpu_begin(g, ctx, nullptr, &z);
    r4b_ctx_sink += ps3dk_prof_gpu_end(g, ctx, &z);
    r4b_ctx_sink += ps3dk_prof_gpu_mark(g, ctx, nullptr);
    r4b_ctx_sink += ps3dk_prof_gpu_calibrate(g, ctx, 0);
    r4b_ctx_sink += ps3dk_prof_gpu_detach(g, ctx, 0);
    r4b_ctx_sink += PS3DK_PROF_GPU_ZONE_BEGIN(g, ctx, "zone", z);
    r4b_ctx_sink += PS3DK_PROF_GPU_ZONE_END(g, ctx, z);
    r4b_ctx_sink += PS3DK_PROF_GPU_MARK(g, ctx, "mark");
}

void r4b_ctx_cxx(CellGcmContextData *cell, struct CellGcmContextData *tagged, gcmContextData *psl)
{
    each(gCellGcmCurrentContext);
    each(cell);
    each(tagged);
    each(psl);
}

#ifdef R4B_MAIN
int main()
{
    r4b_ctx_cxx(gCellGcmCurrentContext, gCellGcmCurrentContext, gGcmContext);
    return r4b_ctx_sink & 1;
}
#endif
#endif
