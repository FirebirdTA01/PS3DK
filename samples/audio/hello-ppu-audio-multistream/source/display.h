/* Shader-free gain display; independent helpers from the GCM pacing sample. */
#ifndef AUDIO_MIX_DISPLAY_H
#define AUDIO_MIX_DISPLAY_H
#include <cell/gcm.h>
#include <cell/sysutil.h>
#include <sysutil/video.h>
#include <sys/sys_time.h>
#include <sys/process.h>
#include <stdatomic.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

enum { STAMPS = 4096 };
enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20 };
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "callbacks need lock-free unsigned atomics");
static atomic_uint flip_count, callback_overflow, quit_requested;
static uint32_t width, height, color_pitch, depth_pitch;
static uint32_t color_offset[2], depth_offset;
static unsigned buffer_id = 1;
static int failed;

static uint64_t now_us(void) { return (uint64_t)sys_time_get_system_time(); }

static void on_flip(uint32_t head)
{
    (void)head;
    /* There is only one outstanding flip. Publish its completion count; the PPU
     * does not submit the next flip until it acquires this completion count. */
    unsigned n = atomic_load_explicit(&flip_count, memory_order_relaxed);
    if (n >= STAMPS) {
        atomic_store_explicit(&callback_overflow, 1, memory_order_release);
        return;
    }
    atomic_store_explicit(&flip_count, n + 1, memory_order_release);
}

static void on_sysutil(uint64_t status, uint64_t param, void *data)
{
    (void)param; (void)data;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        atomic_store_explicit(&quit_requested, 1, memory_order_release);
}

static int callbacks(void)
{
    int rc = cellSysutilCheckCallback();
    if (rc) { printf("AUDIO_MIX_DISPLAY_FAIL sysutil rc=%08x\n", (unsigned)rc); failed = 1; }
    return !failed;
}

static void target(CellGcmContextData *ctx)
{
    CellGcmSurface s = {0};
    s.colorFormat = GCM_SURFACE_X8R8G8B8;
    s.colorTarget = GCM_SURFACE_TARGET_0;
    for (unsigned i = 0; i < 4; ++i) {
        s.colorLocation[i] = GCM_LOCATION_RSX;
        s.colorPitch[i] = i ? 64 : color_pitch;
    }
    s.colorOffset[0] = color_offset[buffer_id];
    s.depthFormat = GCM_SURFACE_ZETA_Z16;
    s.depthLocation = GCM_LOCATION_RSX;
    s.depthOffset = depth_offset; s.depthPitch = depth_pitch;
    s.type = GCM_SURFACE_TYPE_LINEAR; s.antiAlias = GCM_SURFACE_CENTER_1;
    s.width = width; s.height = height;
    cellGcmSetSurface(ctx, &s);
    cellGcmSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    cellGcmSetColorMaskMrt(ctx, 0);
}

static void rectangle(CellGcmContextData *ctx, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t argb)
{
    cellGcmSetScissor(ctx, x, y, w, h);
    cellGcmSetClearColor(ctx, argb);
    cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
}

static void render(CellGcmContextData *ctx, unsigned phase, const float gains[3], unsigned progress)
{
    static const uint32_t colors[3] = {0xff30c080,0xffffa040,0xff5080ff};
    target(ctx);
    rectangle(ctx,0,0,width,height,0xff101824);
    for (unsigned n=0;n<3;++n) {
        uint32_t y=height/5+n*(height/5);
        rectangle(ctx,width/8,y,width*3/4,height/10,0xff303844);
        uint32_t w=(uint32_t)((float)(width*3/4)*gains[n]);
        if (w) rectangle(ctx,width/8,y,w,height/10,colors[n]);
    }
    for (unsigned n=0;n<=phase;++n)
        rectangle(ctx,16+n*80,8,64,12,0xffe0e0e0);
    uint32_t w=(width-32)*progress/1500;
    if (w) rectangle(ctx,16,height-16,w,8,0xffe0e0e0);
}

static int present(CellGcmContextData *ctx)
{
    unsigned before = atomic_load_explicit(&flip_count, memory_order_acquire);
    cellGcmResetFlipStatus();
    int rc = cellGcmSetFlip(ctx, (uint8_t)buffer_id);
    if (rc) { printf("AUDIO_MIX_DISPLAY_FAIL submit rc=%08x\n", (unsigned)rc); return 0; }
    cellGcmFlush(ctx);
    uint64_t start = now_us();
    for (;;) {
        unsigned after = atomic_load_explicit(&flip_count, memory_order_acquire);
        if (atomic_load_explicit(&callback_overflow, memory_order_acquire) || after > before + 1) {
            puts("AUDIO_MIX_DISPLAY_FAIL unexpected callback count"); return 0;
        }
        if (after == before + 1 && cellGcmGetFlipStatus() == 0) break;
        /* Quit is recorded here, but the one in-flight flip is drained before
         * leaving. Failure and this wait remain bounded even on a stalled RSX. */
        if (!callbacks()) return 0;
        if (now_us() - start >= 2000000) {
            printf("AUDIO_MIX_DISPLAY_FAIL flip timeout callbacks=%u status=%u\n",
                   after - before, cellGcmGetFlipStatus());
            return 0;
        }
        usleep(500);
    }
    buffer_id ^= 1u;
    return 1;
}

static int display_init(void)
{
    videoState state;
    videoResolution resolution;
    videoConfiguration video = {0};
    if (videoGetState(0, 0, &state) || state.state != 0 ||
        videoGetResolution(state.displayMode.resolution, &resolution)) return 0;
    width = resolution.width; height = resolution.height;
    if (width < 320 || height < 240 || width > 4096 || height > 4096) return 0;
    color_pitch = (width * 4 + 63) & ~63u;
    depth_pitch = (width * 2 + 63) & ~63u;
    video.resolution = state.displayMode.resolution; video.format = VIDEO_BUFFER_FORMAT_XRGB;
    video.aspect = state.displayMode.aspect; video.pitch = color_pitch;
    if (videoConfigure(0, &video, NULL, 0)) return 0;
    CellGcmConfig config;
    cellGcmGetConfiguration(&config);
    uint64_t color_size = (uint64_t)color_pitch * height;
    uint64_t total = 2 * color_size + (uint64_t)depth_pitch * height;
    if (!config.localAddress || total > config.localSize) return 0;
    for (unsigned n = 0; n < 2; ++n) {
        void *ptr = (void *)((uintptr_t)config.localAddress + (uintptr_t)(n * color_size));
        if (cellGcmAddressToOffset(ptr, &color_offset[n]) ||
            cellGcmSetDisplayBuffer((uint8_t)n, color_offset[n], color_pitch, width, height)) return 0;
    }
    void *depth = (void *)((uintptr_t)config.localAddress + (uintptr_t)(2 * color_size));
    if (cellGcmAddressToOffset(depth, &depth_offset)) return 0;
    printf("AUDIO_MIX_DISPLAY width=%u height=%u pitch=%u local_bytes=%" PRIu64 "\n",
           width, height, color_pitch, total);
    return 1;
}


#endif
