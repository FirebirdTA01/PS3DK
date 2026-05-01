/*
 * hello-ppu-cellgcm-drawenv — setSurface + setDrawEnv repro.
 *
 * Mirrors hello-ppu-cellgcm-clear's init / flip / clear loop but adds
 * the EMP engine's setDrawEnv() command sequence (color mask + viewport +
 * scissor + depth state + cull / blend) between setSurface and the
 * clear, before each flip.  If RPCS3's FIFO desyncs here the same way
 * it does in the engine, the bug is on our side; if it runs clean for
 * many frames, the desync is engine-side sequencing.
 *
 * Sequence per frame:
 *   wait_flip
 *   set_render_target(buffers[cur])      -- cellGcmSetSurface
 *   set_draw_env()                       -- color/vp/scissor/depth/cull
 *   cellGcmSetClearColor + ClearSurface
 *   do_flip(buffers[cur].id)
 *
 * Cycle through six colours for 60 frames each (~6 s at 60 Hz).  START
 * exits early.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sysutil/video.h>
#include <io/pad.h>
#include <cell/gcm.h>
#include <rsx/rsx.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2

#define GCM_LABEL_INDEX 255

typedef struct {
    uint32_t *ptr;
    uint32_t  offset;
    uint16_t  width;
    uint16_t  height;
    int       id;
} display_buffer;

static u32  color_pitch;
static u32  depth_pitch;
static u32  depth_offset;
static u32 *depth_buffer;
static u16  screen_w;
static u16  screen_h;

static void wait_rsx_idle(CellGcmContextData *ctx)
{
    uint32_t label = 1;
    cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, label);
    cellGcmSetWaitLabel(ctx, GCM_LABEL_INDEX, label);
    label++;
    cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, label);
    cellGcmFlush(ctx);
    while (*(vu32 *)cellGcmGetLabelAddress(GCM_LABEL_INDEX) != label)
        usleep(30);
}

static void wait_flip(void)
{
    while (cellGcmGetFlipStatus() != 0)
        usleep(200);
    cellGcmResetFlipStatus();
}

static int do_flip(CellGcmContextData *ctx, uint8_t buffer_id)
{
    if (cellGcmSetFlip(ctx, buffer_id) == 0) {
        cellGcmFlush(ctx);
        cellGcmSetWaitFlip(ctx);
        return 1;
    }
    return 0;
}

static int make_buffer(display_buffer *b, uint16_t width, uint16_t height, int id)
{
    int pitch = (int)(width * sizeof(uint32_t));
    int size  = pitch * height;

    b->ptr = (uint32_t *)rsxMemalign(64, size);
    if (!b->ptr)
        return 0;

    if (cellGcmAddressToOffset(b->ptr, &b->offset) != 0)
        return 0;

    if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, pitch, width, height) != 0)
        return 0;

    b->width  = width;
    b->height = height;
    b->id     = id;
    return 1;
}

static int init_screen(void *host_addr, uint32_t size)
{
    videoState         state;
    videoConfiguration vcfg;
    videoResolution    res;

    if (cellGcmInit(CB_SIZE, size, host_addr) != 0)
        return 0;

    if (videoGetState(0, 0, &state) != 0 || state.state != 0)
        return 0;
    if (videoGetResolution(state.displayMode.resolution, &res) != 0)
        return 0;

    screen_w     = res.width;
    screen_h     = res.height;
    color_pitch  = res.width * sizeof(uint32_t);
    depth_pitch  = res.width * sizeof(uint32_t);

    memset(&vcfg, 0, sizeof(vcfg));
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch      = color_pitch;
    vcfg.aspect     = state.displayMode.aspect;

    wait_rsx_idle(CELL_GCM_CURRENT);

    if (videoConfigure(0, &vcfg, NULL, 0) != 0)
        return 0;
    if (videoGetState(0, 0, &state) != 0)
        return 0;

    cellGcmSetFlipMode(GCM_FLIP_VSYNC);

    /* depth-first allocation order matches the engine and our other gcm
     * samples — RSX-local heap layout matters for the surface command
     * stream RPCS3 emits. */
    depth_buffer = (u32 *)rsxMemalign(64, depth_pitch * screen_h);
    if (!depth_buffer) return 0;
    cellGcmAddressToOffset(depth_buffer, &depth_offset);

    cellGcmResetFlipStatus();
    return 1;
}

/* Mirrors the engine's PS3_renderer.cpp:setRenderTarget verbatim, except
 * for the depth format (engine uses Z24S8 — 4-byte/px so it agrees with
 * depth_pitch we computed above). */
static void set_render_target(CellGcmContextData *ctx, display_buffer *b)
{
    CellGcmSurface sf = {0};
    sf.colorFormat      = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget      = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0]   = b->offset;
    sf.colorPitch[0]    = color_pitch;
    sf.colorLocation[1] = GCM_LOCATION_RSX;
    sf.colorLocation[2] = GCM_LOCATION_RSX;
    sf.colorLocation[3] = GCM_LOCATION_RSX;
    sf.colorPitch[1]    = 64;
    sf.colorPitch[2]    = 64;
    sf.colorPitch[3]    = 64;
    sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
    sf.depthLocation    = GCM_LOCATION_RSX;
    sf.depthOffset      = depth_offset;
    sf.depthPitch       = depth_pitch;
    sf.type             = GCM_TEXTURE_LINEAR;
    sf.antiAlias        = GCM_SURFACE_CENTER_1;
    sf.width            = b->width;
    sf.height           = b->height;
    sf.x                = 0;
    sf.y                = 0;
    cellGcmSetSurface(ctx, &sf);
}

/* Mirrors the engine's PS3_renderer.cpp:setDrawEnv verbatim. */
static void set_draw_env(CellGcmContextData *ctx)
{
    cellGcmSetColorMask(ctx,
        GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    cellGcmSetColorMaskMrt(ctx, 0);

    float minD = 0.0f, maxD = 1.0f;
    float scale[4]  = { screen_w * 0.5f, screen_h * -0.5f, (maxD - minD) * 0.5f, 0.0f };
    float offset[4] = { screen_w * 0.5f, screen_h *  0.5f, (maxD + minD) * 0.5f, 0.0f };
    cellGcmSetViewport(ctx, 0, 0, screen_w, screen_h, minD, maxD, scale, offset);
    rsxSetScissor(ctx, 0, 0, screen_w, screen_h);

    cellGcmSetDepthTestEnable(ctx, GCM_TRUE);
    cellGcmSetDepthFunc(ctx, GCM_LESS);
    cellGcmSetDepthMask(ctx, GCM_TRUE);
    cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
    cellGcmSetFrontFace(ctx, GCM_FRONTFACE_CCW);
    cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
    cellGcmSetBlendEnable(ctx, GCM_FALSE);
}

int main(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    void                *host_addr;
    display_buffer       buffers[MAX_BUFFERS];
    int                  cur = 0;
    padInfo              padinfo;
    CellGcmContextData  *ctx;

    printf("hello-ppu-cellgcm-drawenv: setSurface+setDrawEnv repro\n");

    host_addr = memalign(1024 * 1024, HOST_SIZE);
    if (!init_screen(host_addr, HOST_SIZE)) {
        printf("  init_screen failed\n");
        free(host_addr);
        return 1;
    }
    ctx = CELL_GCM_CURRENT;
    printf("  RSX up at %ux%u (ctx=%p)\n", screen_w, screen_h, (void *)ctx);

    ioPadInit(7);

    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!make_buffer(&buffers[i], screen_w, screen_h, i)) {
            printf("  make_buffer[%d] failed\n", i);
            cellGcmFinish(ctx, 0);
            free(host_addr);
            return 1;
        }
    }
    do_flip(ctx, MAX_BUFFERS - 1);

    static const uint32_t palette[] = {
        0xff800000, 0xff408000, 0xff004080,
        0xff808000, 0xff800080, 0xff008080,
    };
    const int n_colors    = (int)(sizeof(palette) / sizeof(palette[0]));
    const int frames_each = 60;
    const int total       = n_colors * frames_each;

    int          frame = 0;
    volatile int exit_request = 0;

    while (frame < total && !exit_request) {
        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_PADS; i++) {
            if (padinfo.status[i]) {
                padData paddata = {0};
                ioPadGetData(i, &paddata);
                /* Detect rising edge — only exit on press, not hold */
                static uint16_t prev_start[MAX_PADS];
                uint16_t cur = paddata.BTN_START;
                if (cur && !prev_start[i]) {
                    exit_request = 1; break;
                }
                prev_start[i] = cur;
            }
        }

        set_render_target(ctx, &buffers[cur]);
        set_draw_env(ctx);

        uint32_t color = palette[(frame / frames_each) % n_colors];
        cellGcmSetClearColor(ctx, color);
        cellGcmSetClearSurface(ctx,
            GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_Z);

        wait_flip();
        do_flip(ctx, (uint8_t)buffers[cur].id);

        cur = (cur + 1) % MAX_BUFFERS;
        frame++;

        if ((frame % 30) == 0)
            printf("  frame=%d\n", frame);
    }

    printf("  drew %d frames; cycled through %d colors\n", frame, n_colors);
    if (exit_request)
        printf("  early exit on START button\n");

    cellGcmSetWaitFlip(ctx);
    for (int i = 0; i < MAX_BUFFERS; i++)
        rsxFree(buffers[i].ptr);
    rsxFree(depth_buffer);
    cellGcmFinish(ctx, 1);
    free(host_addr);
    ioPadEnd();

    printf("hello-ppu-cellgcm-drawenv: done\n");
    return 0;
}
