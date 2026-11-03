/* P3.3 R4b target LINK fixture (never run): references the public SPU-lane and GPU-domain APIs of the extension
 * archive, so a --gc-sections link retains every member that the API pulls in.  A link witness only, not a functional
 * test.  R4B_LINK_NONE: the negative control references nothing of the extension, so no member may be linked. */
#include <stdint.h>
#include <string.h>

#include <cell/gcm.h>

#include "prof_gpu.h"
#include "prof_spu.h"

volatile uint64_t r4b_sink;

/* OPEN FINDING (R4b target step 1): in C, <cell/gcm.h> makes CellGcmContextData a typedef of gcmContextData, so the
 * struct CellGcmContextData tag the GPU API takes is a different, incomplete type, and a C caller passing
 * gCellGcmCurrentContext gets -Wincompatible-pointer-types.  This never-executed link witness casts ONLY to continue
 * the code-shape inspection; it does not clear C API compatibility.  R4B_UNCAST keeps the reproducer: compiled
 * without the cast, the diagnostic must still appear (a separate typedef-compatible API fix is tracked). */
#ifdef R4B_UNCAST
#define R4B_CTX gCellGcmCurrentContext
#else
#define R4B_CTX ((struct CellGcmContextData *)(void *)gCellGcmCurrentContext)
#endif

#ifdef R4B_LINK_NONE
int main(int argc, char **argv)
{
    (void)argv;
    r4b_sink += (uint64_t)argc;
    return (int)(r4b_sink & 1);
}
#else
static uint8_t r4b_elf[64];

int main(int argc, char **argv)
{
    ps3dk_prof_config cfg;
    ps3dk_prof_ext_config ext;
    ps3dk_prof_spu_lane lane;
    ps3dk_prof_spu_lane_stats ls;
    ps3dk_prof_gpu_stats gs;
    ps3dk_prof_gpu_zone zone;
    ps3dk_prof_token none;
    uint32_t image = 0, gpu = 0, released = 0;
    (void)argv;
    memset(&ext, 0, sizeof ext);
    memset(&lane, 0, sizeof lane);
    memset(&none, 0, sizeof none);
    ps3dk_prof_config_defaults(&cfg);
    r4b_sink += (uint64_t)ps3dk_prof_init_ex(&cfg, &ext);
    r4b_sink += (uint64_t)ps3dk_prof_spu_image_register_elf(r4b_elf, sizeof r4b_elf, &image);
    r4b_sink += (uint64_t)ps3dk_prof_spu_lane_create("r4b", image, 1, &lane);
    r4b_sink += (uint64_t)ps3dk_prof_spu_lane_get_stats(lane.handle, &ls);
    r4b_sink += (uint64_t)ps3dk_prof_spu_lane_end(lane.handle, 0, &ls);
    r4b_sink += (uint64_t)ps3dk_prof_gpu_attach(R4B_CTX, 0, 4, 64, &gpu);
    r4b_sink += (uint64_t)ps3dk_prof_gpu_begin(gpu, R4B_CTX, NULL, &zone);
    r4b_sink += (uint64_t)ps3dk_prof_gpu_end(gpu, R4B_CTX, &zone);
    r4b_sink += (uint64_t)ps3dk_prof_gpu_mark(gpu, R4B_CTX, NULL);
    r4b_sink += (uint64_t)ps3dk_prof_gpu_frame(gpu, none);
    r4b_sink += (uint64_t)ps3dk_prof_gpu_calibrate(gpu, R4B_CTX, 0);
    r4b_sink += (uint64_t)ps3dk_prof_gpu_detach(gpu, R4B_CTX, 0);
    r4b_sink += (uint64_t)ps3dk_prof_gpu_get_stats(gpu, &gs);
    r4b_sink += (uint64_t)ps3dk_prof_gpu_release(gpu, &released);
    return (int)((r4b_sink + (uint64_t)argc) & 1);
}
#endif
