/* On-screen text for the DS3 bench: the GCM setup of
 * samples/gcm/hello-ppu-dbgfont (clear + flip, no geometry), wrapped in a
 * small non-blocking begin/text/end API. See display.h. */
#include "display.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "gcm_host_buffer.h"

#include <ppu-types.h>
#include <sysutil/video.h>
#include <cell/gcm.h>
#include <cell/dbgfont.h>
#include <rsx/rsx.h>
#include <sys/sys_time.h>

#define CB_SIZE 0x100000
#define HOST_SIZE (32 * 1024 * 1024)
#define BUFFERS 2
#define LABEL_INDEX 255
#define WAIT_LIMIT_US 1000000u /* every application-owned wait gives up after 1 s */

typedef struct {
    uint32_t *ptr;
    uint32_t offset;
    uint16_t width, height;
    int id;
} buffer;

static buffer buffers[BUFFERS];
static int current;
static int ready;        /* opened successfully */
static int failed;       /* a submission failed: draw nothing more */
static int flip_pending; /* a flip was requested and has not completed */
static int font_ready;
static uint32_t pitch;
static uint32_t depth_offset;
static uint32_t label_value;
static CellGcmContextData *ctx;

/* Write a fresh label behind all queued work and poll for it until a
 * monotonic deadline WAIT_LIMIT_US away. Returns 0 when the RSX reached it.
 * The deadline bounds this poll only; the GCM and OS calls it makes are not
 * themselves interruptible. */
static int wait_idle_bounded(void)
{
    label_value++;
    cellGcmSetWriteBackEndLabel(ctx, LABEL_INDEX, label_value);
    cellGcmFlush(ctx);
    const uint64_t deadline = (uint64_t)sys_time_get_system_time() + WAIT_LIMIT_US;
    for (;;) {
        if (*(vu32 *)cellGcmGetLabelAddress(LABEL_INDEX) == label_value)
            return 0;
        if ((uint64_t)sys_time_get_system_time() >= deadline)
            return -1;
        usleep(100);
    }
}

int ds3_display_open(void)
{
    sys_addr_t host = 0;
    videoState state;
    videoConfiguration vcfg;
    videoResolution res;

    /* LV2 pages, never freed after GCM init (the RSX may still fetch
     * from them); process exit reclaims them. */
    if (gcm_host_buffer_alloc(HOST_SIZE, &host) != 0)
        return -1;
    if (cellGcmInit(CB_SIZE, HOST_SIZE, (void *)(uintptr_t)host) != 0)
        return -2;
    ctx = CELL_GCM_CURRENT;
    if (videoGetState(0, 0, &state) != 0 || state.state != 0)
        return -3;
    if (videoGetResolution(state.displayMode.resolution, &res) != 0)
        return -4;
    memset(&vcfg, 0, sizeof vcfg);
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch = res.width * sizeof(uint32_t);
    vcfg.aspect = state.displayMode.aspect;
    if (wait_idle_bounded() != 0)
        return -5;
    if (videoConfigure(0, &vcfg, NULL, 0) != 0)
        return -6;
    cellGcmSetFlipMode(GCM_FLIP_VSYNC);
    pitch = res.width * sizeof(uint32_t);
    uint32_t *depth = (uint32_t *)rsxMemalign(64, res.height * pitch);
    if (!depth || cellGcmAddressToOffset(depth, &depth_offset) != 0)
        return -7;
    for (int i = 0; i < BUFFERS; i++) {
        buffer *b = &buffers[i];
        b->ptr = (uint32_t *)rsxMemalign(64, pitch * res.height);
        if (!b->ptr || cellGcmAddressToOffset(b->ptr, &b->offset) != 0)
            return -8;
        if (cellGcmSetDisplayBuffer((uint8_t)i, b->offset, pitch, res.width, res.height) != 0)
            return -9;
        b->width = res.width;
        b->height = res.height;
        b->id = i;
    }

    size_t size = CELL_DBGFONT_FRAGMENT_SIZE + CELL_DBGFONT_TEXTURE_SIZE + 4096 * CELL_DBGFONT_VERTEX_SIZE;
    void *font = rsxMemalign(128, size);
    if (!font)
        return -10;
    CellDbgFontConfigGcm cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.localBufAddr = (sys_addr_t)(uintptr_t)font;
    cfg.localBufSize = (uint32_t)size;
    cfg.option = CELL_DBGFONT_VERTEX_LOCAL | CELL_DBGFONT_TEXTURE_LOCAL;
    if (cellDbgFontInitGcm(&cfg) != 0)
        return -11;
    font_ready = 1;
    cellGcmResetFlipStatus();
    flip_pending = 0;
    ready = 1;
    return 0;
}

int ds3_display_ready(void)
{
    if (!ready || failed)
        return -1;
    if (flip_pending) {
        if (cellGcmGetFlipStatus() != 0)
            return 0; /* previous flip still queued: skip this frame */
        cellGcmResetFlipStatus();
        flip_pending = 0;
    }
    return 1;
}

void ds3_display_begin(void)
{
    if (!ready || failed)
        return;
    buffer *b = &buffers[current];
    CellGcmSurface sf;
    memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0] = b->offset;
    sf.colorPitch[0] = pitch;
    for (int i = 1; i < 4; i++) {
        sf.colorLocation[i] = GCM_LOCATION_RSX;
        sf.colorPitch[i] = 64;
    }
    sf.depthFormat = GCM_SURFACE_ZETA_Z16;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = depth_offset;
    sf.depthPitch = pitch;
    sf.type = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = b->width;
    sf.height = b->height;
    cellGcmSetSurface(ctx, &sf);
    float scale[4] = {b->width / 2.0f, -(float)b->height / 2.0f, 0.5f, 0.0f};
    float offset[4] = {b->width / 2.0f, b->height / 2.0f, 0.5f, 0.0f};
    cellGcmSetViewport(ctx, 0, 0, b->width, b->height, 0.0f, 1.0f, scale, offset);
    cellGcmSetScissor(ctx, 0, 0, b->width, b->height);
    cellGcmSetClearColor(ctx, 0xff101822);
    cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
}

void ds3_display_text(float x, float y, float scale, uint32_t argb, const char *text)
{
    if (ready && !failed)
        cellDbgFontPrintf(x, y, scale, argb, "%s", text);
}

int ds3_display_end(void)
{
    if (!ready || failed)
        return -1;
    if (cellDbgFontDrawGcm() != 0) {
        failed = 1;
        return -2;
    }
    if (cellGcmSetFlip(ctx, (uint8_t)buffers[current].id) != 0) {
        failed = 1;
        return -3;
    }
    cellGcmFlush(ctx);
    flip_pending = 1;
    current = (current + 1) % BUFFERS;
    return 0;
}

void ds3_display_close(void)
{
    if (font_ready) {
        cellDbgFontExitGcm();
        font_ready = 0;
    }
    if (ready) {
        /* Bounded: if the RSX does not drain in time, exit anyway rather
         * than hang the process on a blocking finish. */
        (void)wait_idle_bounded();
        ready = 0;
    }
}
