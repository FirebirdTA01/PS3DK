/*
 * samples/gcm/common/gcm_segment_display.h
 *
 * Display setup, render-target state and a bounded flip for the
 * command-segment samples: two X8R8G8B8 colour buffers and one Z16 depth
 * buffer in RSX local memory, shader-free scissored clears.  Static
 * inline, so it builds for ILP32 and LP64 with no extra library.
 */
#ifndef PS3TC_SAMPLES_GCM_SEGMENT_DISPLAY_H
#define PS3TC_SAMPLES_GCM_SEGMENT_DISPLAY_H

#include <cell/gcm.h>
#include <sysutil/video.h>
#include <sys/sys_time.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef struct {
    uint32_t width, height, color_pitch, depth_pitch;
    uint32_t color_offset[2], depth_offset;
    uint32_t local_used;        /* bytes of local memory the buffers take */
    unsigned buffer_id;         /* the buffer being drawn                  */
} SegDisplay;

static inline int seg_display_init(SegDisplay *d)
{
    videoState state;
    videoResolution resolution;
    videoConfiguration video = {0};
    CellGcmConfig config;
    uint64_t color_size, total;

    if (videoGetState(0, 0, &state) || state.state != 0 ||
        videoGetResolution(state.displayMode.resolution, &resolution)) return 0;
    d->width = resolution.width; d->height = resolution.height;
    if (d->width < 320 || d->height < 240 || d->width > 4096 || d->height > 4096) return 0;
    d->color_pitch = (d->width * 4 + 63) & ~63u;
    d->depth_pitch = (d->width * 2 + 63) & ~63u;
    video.resolution = state.displayMode.resolution; video.format = VIDEO_BUFFER_FORMAT_XRGB;
    video.aspect = state.displayMode.aspect; video.pitch = d->color_pitch;
    if (videoConfigure(0, &video, NULL, 0)) return 0;
    cellGcmGetConfiguration(&config);
    color_size = (uint64_t)d->color_pitch * d->height;
    total = 2 * color_size + (uint64_t)d->depth_pitch * d->height;
    if (!config.localAddress || total > config.localSize) return 0;
    for (unsigned n = 0; n < 2; ++n) {
        void *ptr = (void *)((uintptr_t)config.localAddress + (uintptr_t)(n * color_size));
        if (cellGcmAddressToOffset(ptr, &d->color_offset[n]) ||
            cellGcmSetDisplayBuffer((uint8_t)n, d->color_offset[n], d->color_pitch,
                                    d->width, d->height)) return 0;
    }
    if (cellGcmAddressToOffset((void *)((uintptr_t)config.localAddress + (uintptr_t)(2 * color_size)),
                               &d->depth_offset)) return 0;
    d->local_used = (uint32_t)total;
    d->buffer_id = 0;
    return 1;
}

/* Render-target state for the current buffer. */
static inline void seg_display_target(CellGcmContextData *ctx, const SegDisplay *d)
{
    CellGcmSurface s = {0};
    s.colorFormat = GCM_SURFACE_X8R8G8B8;
    s.colorTarget = GCM_SURFACE_TARGET_0;
    for (unsigned i = 0; i < 4; ++i) {
        s.colorLocation[i] = GCM_LOCATION_RSX;
        s.colorPitch[i] = i ? 64 : d->color_pitch;
    }
    s.colorOffset[0] = d->color_offset[d->buffer_id];
    s.depthFormat = GCM_SURFACE_ZETA_Z16;
    s.depthLocation = GCM_LOCATION_RSX;
    s.depthOffset = d->depth_offset; s.depthPitch = d->depth_pitch;
    s.type = GCM_SURFACE_TYPE_LINEAR; s.antiAlias = GCM_SURFACE_CENTER_1;
    s.width = d->width; s.height = d->height;
    cellGcmSetSurface(ctx, &s);
    cellGcmSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    cellGcmSetColorMaskMrt(ctx, 0);
}

static inline void seg_rect(CellGcmContextData *ctx, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h, uint32_t argb)
{
    cellGcmSetScissor(ctx, x, y, w, h);
    cellGcmSetClearColor(ctx, argb);
    cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
}

/* Flip the current buffer and poll the flip status at most max_polls times
 * (200 us apart).  An iteration count, not a timer, bounds the wait. */
#define SEG_FLIP_POLLS 10000u
static inline int seg_display_flip(CellGcmContextData *ctx, SegDisplay *d, uint32_t max_polls)
{
    uint32_t polls = 0;
    cellGcmResetFlipStatus();
    if (cellGcmSetFlip(ctx, (uint8_t)d->buffer_id)) return 0;
    cellGcmFlush(ctx);
    while (cellGcmGetFlipStatus() != 0) {
        if (polls++ == max_polls) return 0;
        usleep(200);
    }
    d->buffer_id ^= 1u;
    return 1;
}

/* Bounded wait for a label to reach `value` (modulo 2^32). */
static inline int seg_wait_label(uint8_t index, uint32_t value, uint32_t max_polls,
                                 uint32_t poll_us, uint32_t *polls_used)
{
    volatile uint32_t *label = (volatile uint32_t *)cellGcmGetLabelAddress(index);
    uint32_t polls = 0;
    int ok = 0;
    for (;;) {
        if ((int32_t)(*label - value) >= 0) { ok = 1; break; }
        if (polls == max_polls) break;
        ++polls;
        usleep(poll_us);
    }
    if (polls_used) *polls_used = polls;
    return ok;
}

#endif
