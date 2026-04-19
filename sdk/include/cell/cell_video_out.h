/*
 * PS3 Custom Toolchain — <cell/cell_video_out.h>
 *
 * Sony-style surface for the video-output subsystem (resolution query,
 * mode configuration, scan-mode / aspect / buffer-format constants).
 * Runtime forwards to PSL1GHT's sysutil/video.h — CellVideoOut*
 * structs are byte-identical to the lower-case video* versions.
 *
 * Sony bundles this surface into <sysutil/sysutil_sysparam.h>; our
 * shim under the same path re-exports it so Sony sample code that
 * only includes sysutil_sysparam.h still gets the full video API.
 */

#ifndef PS3TC_CELL_VIDEO_OUT_H
#define PS3TC_CELL_VIDEO_OUT_H

#include <stdint.h>
#include <sysutil/video.h>
#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- port / identity constants ------------------------------------ */
#define CELL_VIDEO_OUT_PRIMARY            VIDEO_PRIMARY
#define CELL_VIDEO_OUT_SECONDARY          VIDEO_SECONDARY

/* ---- buffer color formats ----------------------------------------- */
/* Sony names the component ordering; we alias onto PSL1GHT's enum. */
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8  VIDEO_BUFFER_FORMAT_XRGB
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8B8G8R8  VIDEO_BUFFER_FORMAT_XBGR
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_R16G16B16X16_FLOAT \
    VIDEO_BUFFER_FORMAT_FLOAT

/* ---- aspect ratio tags -------------------------------------------- */
#define CELL_VIDEO_OUT_ASPECT_AUTO        VIDEO_ASPECT_AUTO
#define CELL_VIDEO_OUT_ASPECT_4_3         VIDEO_ASPECT_4_3
#define CELL_VIDEO_OUT_ASPECT_16_9        VIDEO_ASPECT_16_9

/* ---- struct / handle aliases --------------------------------------
 *
 * Redefined here as our own structs (NOT typedef'd from PSL1GHT's
 * video.h) so we can expose both the PSL1GHT field name and the
 * Sony-sample field name through an anonymous union without
 * editing the upstream PSL1GHT header.  The storage layouts are
 * identical to PSL1GHT's videoResolution / videoState /
 * videoConfiguration / videoDisplayMode down to the last padding
 * byte, so `(videoState *)cell_state` casts in the forwarders
 * below are safe.
 *
 * Field-name mapping:
 *   .resolution   (PSL1GHT)  <-->  .resolutionId  (Sony samples)
 *
 * Unified via `union { u8 resolution; u8 resolutionId; }` so Sony
 * source code that writes `.resolutionId = ...` compiles unchanged
 * against our SDK.  Previously carried as PSL1GHT patch 0026; moved
 * here 2026-04-19 to keep PSL1GHT's tree unmodified. */

typedef struct _cellVideoOutResolution {
    uint16_t width;
    uint16_t height;
} CellVideoOutResolution;

typedef struct _cellVideoOutDisplayMode {
    union { uint8_t resolution; uint8_t resolutionId; };
    uint8_t  scanMode;
    uint8_t  conversion;
    uint8_t  aspect;
    uint8_t  padding[2];
    uint16_t refreshRates;
} CellVideoOutDisplayMode;

typedef struct _cellVideoOutState {
    uint8_t                  state;
    uint8_t                  colorSpace;
    uint8_t                  padding[6];
    CellVideoOutDisplayMode  displayMode;
} CellVideoOutState;

typedef struct _cellVideoOutConfiguration {
    union { uint8_t resolution; uint8_t resolutionId; };
    uint8_t  format;
    uint8_t  aspect;
    uint8_t  padding[9];
    uint32_t pitch;
} CellVideoOutConfiguration;

/* ---- function forwarders ------------------------------------------ */
static inline int32_t cellVideoOutGetState(uint32_t videoOut,
                                           uint32_t deviceIndex,
                                           CellVideoOutState *state)
{
    return (int32_t)videoGetState((s32)videoOut, (s32)deviceIndex,
                                  (videoState *)state);
}

static inline int32_t cellVideoOutGetResolution(uint32_t resolutionId,
                                                CellVideoOutResolution *resolution)
{
    return (int32_t)videoGetResolution((s32)resolutionId,
                                       (videoResolution *)resolution);
}

static inline int32_t cellVideoOutConfigure(uint32_t videoOut,
                                            CellVideoOutConfiguration *config,
                                            void *option,
                                            uint32_t blocking)
{
    return (int32_t)videoConfigure((s32)videoOut,
                                   (videoConfiguration *)config,
                                   option, (s32)blocking);
}

static inline int32_t cellVideoOutGetConfiguration(uint32_t videoOut,
                                                   CellVideoOutConfiguration *config,
                                                   void *option)
{
    return (int32_t)videoGetConfiguration(videoOut,
                                          (videoConfiguration *)config,
                                          option);
}

static inline int32_t cellVideoOutGetResolutionAvailability(uint32_t videoOut,
                                                            uint32_t resolutionId,
                                                            uint32_t aspect,
                                                            uint32_t option)
{
    return (int32_t)videoGetResolutionAvailability(videoOut, resolutionId,
                                                   aspect, option);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_VIDEO_OUT_H */
