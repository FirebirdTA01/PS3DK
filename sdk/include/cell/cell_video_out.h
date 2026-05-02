/*
 * PS3 Custom Toolchain — <cell/cell_video_out.h>
 *
 * Cell-SDK-style surface for the video-output subsystem (resolution
 * query, mode configuration, scan-mode / aspect / buffer-format
 * constants).  Functions resolve through libsysutil_stub.a
 * (SPRX trampolines into cellSysutil); constants alias the
 * byte-identical PSL1GHT <sysutil/video.h> values.
 *
 * The cell SDK bundles this surface into <sysutil/sysutil_sysparam.h>;
 * our shim under the same path re-exports it so cell-SDK sample code
 * that only includes sysutil_sysparam.h still gets the full video API.
 */

#ifndef PS3TC_CELL_VIDEO_OUT_H
#define PS3TC_CELL_VIDEO_OUT_H

#include <stdint.h>
#include <sysutil/video.h>
#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes --------------------------------------------------- */
#define CELL_VIDEO_OUT_SUCCEEDED                        0
#define CELL_VIDEO_OUT_ERROR_NOT_IMPLEMENTED            0x8002b220
#define CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION      0x8002b221
#define CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER          0x8002b222
#define CELL_VIDEO_OUT_ERROR_PARAMETER_OUT_OF_RANGE     0x8002b223
#define CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND           0x8002b224
#define CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT      0x8002b225
#define CELL_VIDEO_OUT_ERROR_UNSUPPORTED_DISPLAY_MODE   0x8002b226
#define CELL_VIDEO_OUT_ERROR_CONDITION_BUSY             0x8002b227
#define CELL_VIDEO_OUT_ERROR_VALUE_IS_NOT_SET           0x8002b228

/* ---- output / device states --------------------------------------- */
#define CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED   0
#define CELL_VIDEO_OUT_OUTPUT_STATE_DISABLED  1
#define CELL_VIDEO_OUT_OUTPUT_STATE_PREPARING 2

#define CELL_VIDEO_OUT_DEVICE_STATE_UNAVAILABLE 0
#define CELL_VIDEO_OUT_DEVICE_STATE_AVAILABLE   1

/* ---- color space -------------------------------------------------- */
#define CELL_VIDEO_OUT_COLOR_SPACE_RGB   0x01
#define CELL_VIDEO_OUT_COLOR_SPACE_YUV   0x02
#define CELL_VIDEO_OUT_COLOR_SPACE_XVYCC 0x04

/* ---- port / identity constants ------------------------------------ */
#define CELL_VIDEO_OUT_PRIMARY            VIDEO_PRIMARY
#define CELL_VIDEO_OUT_SECONDARY          VIDEO_SECONDARY

/* ---- buffer color formats ----------------------------------------- */
/* The cell SDK names the component ordering; we alias onto PSL1GHT's enum. */
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8  VIDEO_BUFFER_FORMAT_XRGB
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8B8G8R8  VIDEO_BUFFER_FORMAT_XBGR
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_R16G16B16X16_FLOAT \
    VIDEO_BUFFER_FORMAT_FLOAT

/* ---- aspect ratio tags -------------------------------------------- */
#define CELL_VIDEO_OUT_ASPECT_AUTO        VIDEO_ASPECT_AUTO
#define CELL_VIDEO_OUT_ASPECT_4_3         VIDEO_ASPECT_4_3
#define CELL_VIDEO_OUT_ASPECT_16_9        VIDEO_ASPECT_16_9

/* ---- resolution tags ---------------------------------------------- */
#define CELL_VIDEO_OUT_RESOLUTION_UNDEFINED      VIDEO_RESOLUTION_UNDEFINED
#define CELL_VIDEO_OUT_RESOLUTION_1080           VIDEO_RESOLUTION_1080
#define CELL_VIDEO_OUT_RESOLUTION_720            VIDEO_RESOLUTION_720
#define CELL_VIDEO_OUT_RESOLUTION_480            VIDEO_RESOLUTION_480
#define CELL_VIDEO_OUT_RESOLUTION_576            VIDEO_RESOLUTION_576
#define CELL_VIDEO_OUT_RESOLUTION_1600x1080      VIDEO_RESOLUTION_1600x1080
#define CELL_VIDEO_OUT_RESOLUTION_1440x1080      VIDEO_RESOLUTION_1440x1080
#define CELL_VIDEO_OUT_RESOLUTION_1280x1080      VIDEO_RESOLUTION_1280x1080
#define CELL_VIDEO_OUT_RESOLUTION_960x1080       VIDEO_RESOLUTION_960x1080

/* ---- struct / handle aliases --------------------------------------
 *
 * Redefined here as our own structs (NOT typedef'd from PSL1GHT's
 * video.h) so we can expose both the PSL1GHT field name and the
 * cell-SDK-sample field name through an anonymous union without
 * editing the upstream PSL1GHT header.  The storage layouts are
 * identical to PSL1GHT's videoResolution / videoState /
 * videoConfiguration / videoDisplayMode down to the last padding
 * byte, so `(videoState *)cell_state` casts in the forwarders
 * below are safe.
 *
 * Field-name mapping:
 *   .resolution   (PSL1GHT)  <-->  .resolutionId  (cell SDK samples)
 *
 * Unified via `union { u8 resolution; u8 resolutionId; }` so cell-SDK
 * source code that writes `.resolutionId = ...` compiles unchanged
 * against our SDK. */

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

/* ---- function declarations ----------------------------------------- *
 * Resolved by libsysutil_stub.a (SPRX trampolines into cellSysutil).
 * Previously these were static-inline forwarders that called PSL1GHT's
 * videoGetState / videoConfigure / etc. through libsysutil.a, which
 * forced reference Makefiles to pull in -lsysutil on top of the
 * _stub archive.  Now they're plain extern declarations — samples
 * that only link -lsysutil_stub resolve them directly. */
extern int cellVideoOutGetState(uint32_t videoOut,
                                uint32_t deviceIndex,
                                CellVideoOutState *state);
extern int cellVideoOutGetResolution(uint32_t resolutionId,
                                     CellVideoOutResolution *resolution);
extern int cellVideoOutConfigure(uint32_t videoOut,
                                 CellVideoOutConfiguration *config,
                                 void *option,
                                 uint32_t blocking);
extern int cellVideoOutGetConfiguration(uint32_t videoOut,
                                        CellVideoOutConfiguration *config,
                                        void *option);
extern int cellVideoOutGetResolutionAvailability(uint32_t videoOut,
                                                 uint32_t resolutionId,
                                                 uint32_t aspect,
                                                 uint32_t option);

#define CELL_VIDEO_OUT_PORT_NONE           0x00
#define CELL_VIDEO_OUT_PORT_HDMI           0x01
#define CELL_VIDEO_OUT_PORT_NETWORK        0x41
#define CELL_VIDEO_OUT_PORT_COMPOSITE_S    0x81
#define CELL_VIDEO_OUT_PORT_D              0x82
#define CELL_VIDEO_OUT_PORT_COMPONENT      0x83
#define CELL_VIDEO_OUT_PORT_RGB            0x84
#define CELL_VIDEO_OUT_PORT_AVMULTI_SCART  0x85
#define CELL_VIDEO_OUT_PORT_DSUB           0x86

#define CELL_VIDEO_OUT_EVENT_DEVICE_CHANGED        0
#define CELL_VIDEO_OUT_EVENT_OUTPUT_DISABLED       1
#define CELL_VIDEO_OUT_EVENT_DEVICE_AUTHENTICATED  2
#define CELL_VIDEO_OUT_EVENT_OUTPUT_ENABLED        3

#define CELL_VIDEO_OUT_RGB_OUTPUT_RANGE_LIMITED  0
#define CELL_VIDEO_OUT_RGB_OUTPUT_RANGE_FULL     1

#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_UNDEFINED      0
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_480I_59_94HZ   1
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_576I_50HZ      2
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_480P_59_94HZ   3
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_576P_50HZ      4
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_1080I_59_94HZ  5
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_720P_59_94HZ   7
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_1080P_59_94HZ  9
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_WXGA_60HZ      11
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_SXGA_60HZ      12
#define CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_WUXGA_60HZ     13

typedef struct _cellVideoOutColorInfo {
    uint16_t  redX;
    uint16_t  redY;
    uint16_t  greenX;
    uint16_t  greenY;
    uint16_t  blueX;
    uint16_t  blueY;
    uint16_t  whiteX;
    uint16_t  whiteY;
    uint32_t  gamma;
} CellVideoOutColorInfo;

typedef struct _cellVideoOutKSVList {
    uint8_t   ksv[32 * 5];
    uint8_t   reserved[4];
    uint32_t  count;
} CellVideoOutKSVList;

typedef struct _cellVideoOutDeviceInfo {
    uint8_t                  portType;
    uint8_t                  colorSpace;
    uint16_t                 latency;
    uint8_t                  availableModeCount;
    uint8_t                  state;
    uint8_t                  rgbOutputRange;
    uint8_t                  reserved[5];
    CellVideoOutColorInfo    colorInfo;
    CellVideoOutDisplayMode  availableModes[32];
    CellVideoOutKSVList      ksvList;
} CellVideoOutDeviceInfo;

typedef int (*CellVideoOutCallback)(uint32_t slot,
                                    uint32_t videoOut,
                                    uint32_t deviceIndex,
                                    uint32_t event,
                                    CellVideoOutDeviceInfo *info,
                                    void *userData);

extern int cellVideoOutGetNumberOfDevice(uint32_t videoOut);
extern int cellVideoOutGetDeviceInfo(uint32_t videoOut,
                                     uint32_t deviceIndex,
                                     CellVideoOutDeviceInfo *info);
extern int cellVideoOutRegisterCallback(uint32_t slot,
                                        CellVideoOutCallback function,
                                        void *userData);
extern int cellVideoOutUnregisterCallback(uint32_t slot);
extern int cellVideoOutDebugSetMonitorType(uint32_t videoOut,
                                           uint32_t monitorType);
extern int cellVideoOutGetConvertCursorColorInfo(uint8_t *rgbOutputRange);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_VIDEO_OUT_H */
