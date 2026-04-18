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

/* ---- struct / handle aliases -------------------------------------- */
typedef videoState          CellVideoOutState;
typedef videoResolution     CellVideoOutResolution;
typedef videoConfiguration  CellVideoOutConfiguration;

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
