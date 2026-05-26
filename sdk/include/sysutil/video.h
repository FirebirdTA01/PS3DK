/*! \file sysutil/video.h
 \brief Video-output mode management - native compat surface.

  Drop-in replacement for the legacy <sysutil/video.h> shipped with
  code written for older SDKs.  The legacy names (videoState /
  videoConfiguration / videoResolution / videoDisplayMode and the
  videoGetState / videoConfigure / videoGetResolution call set) are
  preserved bit-for-bit and routed onto the native cellVideoOut*
  imports provided by the nidgen-emitted multilib stub archive
  (libsysutil_stub.a, ILP32 + LP64) through our compact-OPD
  frame-less SPRX trampolines.  No legacy runtime (libsysutil.a
  forwarder, __get_opd32) is involved.

  The struct layouts are the video-out HLE binary contract: every
  field width and padding byte matches what the SPRX writes back,
  identical to <cell/cell_video_out.h>'s CellVideoOut* layouts down
  to the last reserved byte (the cast in the adapters below is
  layout-safe and intentional).  All width-sensitive fields are
  fixed-width; none of these structures carry pointer members that
  cross the SPRX boundary, so no pointer-EA annotation is required.

  This header is self-contained: it does NOT include
  <cell/cell_video_out.h>, so the reverse include in that header
  (which pulls this one for the legacy constant names) does not
  recurse.  The cellVideoOut* prototypes declared here are
  type-compatible with the typedef'd declarations in
  <cell/cell_video_out.h> for translation units that see both.

  Link with -lsysutil_stub (not the legacy -lsysutil).

  PS3DK-NATIVE-VIDEO-SHIM: VALIDATED
  Verified through the real cmake pipeline + RPCS3: representative
  consumers (gcm textured-quad, jpg-dec, png) build with zero
  legacy-video / forwarder symbols and zero undefined symbols, and
  at runtime the legacy-named videoConfigure routes correctly to the
  native cellVideoOutConfigure import (correct args, zero fatal
  markers; quad reaches process-exit, the decode samples idle on
  user input as designed).
*/

#ifndef __LV2_VIDEO_H__
#define __LV2_VIDEO_H__

#include <stdint.h>

/* ---- output state -------------------------------------------------- */
#define VIDEO_STATE_DISABLED                          0
#define VIDEO_STATE_ENABLED                           1
#define VIDEO_STATE_BUSY                              3

/* ---- video-out identity ------------------------------------------- */
#define VIDEO_PRIMARY                                 0
#define VIDEO_SECONDARY                               1

/* ---- scan mode ----------------------------------------------------- */
#define VIDEO_SCANMODE_INTERLACE                      0
#define VIDEO_SCANMODE_PROGRESSIVE                    1

#define VIDEO_SCANMODE2_AUTO                          0
#define VIDEO_SCANMODE2_INTERLACE                     1
#define VIDEO_SCANMODE2_PROGRESSIVE                   2

/* ---- buffer color format ------------------------------------------ */
#define VIDEO_BUFFER_FORMAT_XRGB                      0
#define VIDEO_BUFFER_FORMAT_XBGR                      1
#define VIDEO_BUFFER_FORMAT_FLOAT                     2

/* ---- aspect ratio -------------------------------------------------- */
#define VIDEO_ASPECT_AUTO                             0
#define VIDEO_ASPECT_4_3                              1
#define VIDEO_ASPECT_16_9                             2

/* ---- resolution ids ------------------------------------------------ */
#define VIDEO_RESOLUTION_UNDEFINED                    0
#define VIDEO_RESOLUTION_1080                         1
#define VIDEO_RESOLUTION_720                          2
#define VIDEO_RESOLUTION_480                          4
#define VIDEO_RESOLUTION_576                          5
#define VIDEO_RESOLUTION_1600x1080                    10
#define VIDEO_RESOLUTION_1440x1080                    11
#define VIDEO_RESOLUTION_1280x1080                    12
#define VIDEO_RESOLUTION_960x1080                     13

/* ---- color space --------------------------------------------------- */
#define VIDEO_COLOR_RGB                               0x01
#define VIDEO_COLOR_YUV                               0x02
#define VIDEO_COLOR_XVYCC                             0x04

/* ---- 3D frame-packing resolution ids ------------------------------ */
#define VIDEO_RESOLUTION_720_3D_FRAME_PACKING         0x81
#define VIDEO_RESOLUTION_1024x720_3D_FRAME_PACKING    0x88
#define VIDEO_RESOLUTION_960x720_3D_FRAME_PACKING     0x89
#define VIDEO_RESOLUTION_800x720_3D_FRAME_PACKING     0x8a
#define VIDEO_RESOLUTION_640x720_3D_FRAME_PACKING     0x8b

/* ---- refresh rates ------------------------------------------------- */
#define VIDEO_REFRESH_AUTO                            0x00
#define VIDEO_REFRESH_59_94HZ                         0x01
#define VIDEO_REFRESH_50HZ                            0x02
#define VIDEO_REFRESH_60HZ                            0x04
#define VIDEO_REFRESH_30HZ                            0x08

/* ---- output port --------------------------------------------------- */
#define VIDEO_PORT_NONE                               0x00
#define VIDEO_PORT_HDMI                               0x01
#define VIDEO_PORT_NETWORK                            0x41
#define VIDEO_PORT_COMPOSITE                          0x81
#define VIDEO_PORT_D                                  0x82
#define VIDEO_PORT_COMPONENT                          0x83
#define VIDEO_PORT_RGB                                0x84
#define VIDEO_PORT_SCART                              0x85
#define VIDEO_PORT_DSUB                               0x86

#ifdef __cplusplus
extern "C" {
#endif

/* ---- legacy compat types ------------------------------------------
 *
 * Bit-for-bit identical to the video-out HLE contract and to the
 * CellVideoOut* layouts in <cell/cell_video_out.h>.  Kept as their
 * own struct tags (not typedef'd from the cell names) so this header
 * stays a leaf - the layout match is what makes the casts in the
 * adapters below safe, not a shared declaration. */

typedef struct _videoresolution {
    uint16_t width;          /*!< Screen width in pixels.  */
    uint16_t height;         /*!< Screen height in pixels. */
} videoResolution;

typedef struct _videodisplaymode {
    uint8_t  resolution;     /*!< VIDEO_RESOLUTION_* id.   */
    uint8_t  scanMode;
    uint8_t  conversion;
    uint8_t  aspect;         /*!< VIDEO_ASPECT_* tag.      */
    uint8_t  padding[2];
    uint16_t refreshRates;
} videoDisplayMode;

typedef struct _videostate {
    uint8_t          state;       /*!< VIDEO_STATE_* value. */
    uint8_t          colorSpace;
    uint8_t          padding[6];
    videoDisplayMode displayMode;
} videoState;

typedef struct _videoconfig {
    uint8_t  resolution;     /*!< VIDEO_RESOLUTION_* id.        */
    uint8_t  format;         /*!< VIDEO_BUFFER_FORMAT_* id.     */
    uint8_t  aspect;         /*!< VIDEO_ASPECT_* tag.           */
    uint8_t  padding[9];
    uint32_t pitch;          /*!< Byte stride between lines.    */
} videoConfiguration;

typedef struct _videoColorInfo {
    uint16_t redX;
    uint16_t redY;
    uint16_t greenX;
    uint16_t greenY;
    uint16_t blueX;
    uint16_t blueY;
    uint16_t whiteX;
    uint16_t whiteY;
    uint32_t gamma;
} videoColorInfo;

typedef struct _videoKSVList {
    uint8_t  ksv[32 * 5];
    uint8_t  padding[4];
    uint32_t count;
} videoKSVList;

typedef struct _videoDeviceInfo {
    uint8_t          portType;
    uint8_t          colorSpace;
    uint16_t         latency;
    uint8_t          availableModeCount;
    uint8_t          state;
    uint8_t          rgbOutputRange;
    uint8_t          padding[5];
    videoColorInfo   colorInfo;
    videoDisplayMode availableModes[32];
    videoKSVList     ksvList;
} videoDeviceInfo;

typedef int32_t (*videoCallback)(uint32_t slot, uint32_t videoOut,
                                 uint32_t deviceIndex, uint32_t event,
                                 videoDeviceInfo *info, void *userData);

/* ---- native imports (libsysutil_stub.a) ---------------------------
 *
 * Forward-declared tags + prototypes that are type-compatible
 * with the typedef'd cellVideoOut* declarations in
 * <cell/cell_video_out.h>.  Declaring them here (rather than
 * including that header) keeps this file a leaf and breaks the
 * otherwise-circular include. */
struct CellVideoOutState;
struct CellVideoOutResolution;
struct CellVideoOutConfiguration;
struct CellVideoOutDeviceInfo;
struct CellVideoOutOption;

extern int cellVideoOutGetState(uint32_t videoOut, uint32_t deviceIndex,
                                struct CellVideoOutState *state);
extern int cellVideoOutGetResolution(uint32_t resolutionId,
                                     struct CellVideoOutResolution *resolution);
extern int cellVideoOutConfigure(uint32_t videoOut,
                                 struct CellVideoOutConfiguration *config,
                                 struct CellVideoOutOption *option,
                                 uint32_t blocking);
extern int cellVideoOutGetConfiguration(uint32_t videoOut,
                                        struct CellVideoOutConfiguration *config,
                                        struct CellVideoOutOption *option);
extern int cellVideoOutGetResolutionAvailability(uint32_t videoOut,
                                                 uint32_t resolutionId,
                                                 uint32_t aspect,
                                                 uint32_t option);
extern int cellVideoOutGetNumberOfDevice(uint32_t videoOut);
extern int cellVideoOutGetDeviceInfo(uint32_t videoOut, uint32_t deviceIndex,
                                     struct CellVideoOutDeviceInfo *info);
extern int cellVideoOutDebugSetMonitorType(uint32_t videoOut,
                                           uint32_t monitorType);
extern int cellVideoOutGetConvertCursorColorInfo(uint8_t *rgbOutputRange);

/* ---- legacy-named adapters ----------------------------------------
 *
 * Thin pass-throughs.  The legacy structs are layout-identical to
 * the cell structs, so the pointer casts reinterpret the same
 * storage; no field is touched here. */

static inline int32_t videoGetState(int32_t videoOut, int32_t deviceIndex,
                                    videoState *state)
{
    return (int32_t)cellVideoOutGetState((uint32_t)videoOut,
                                         (uint32_t)deviceIndex,
                                         (struct CellVideoOutState *)state);
}

static inline int32_t videoGetResolution(int32_t resolutionId,
                                         videoResolution *resolution)
{
    return (int32_t)cellVideoOutGetResolution(
        (uint32_t)resolutionId,
        (struct CellVideoOutResolution *)resolution);
}

static inline int32_t videoConfigure(int32_t videoOut,
                                     videoConfiguration *config,
                                     void *option, int32_t blocking)
{
    return (int32_t)cellVideoOutConfigure(
        (uint32_t)videoOut,
        (struct CellVideoOutConfiguration *)config,
        (struct CellVideoOutOption *)option, (uint32_t)blocking);
}

static inline int32_t videoGetConfiguration(uint32_t videoOut,
                                            videoConfiguration *config,
                                            void *option)
{
    return (int32_t)cellVideoOutGetConfiguration(
        videoOut, (struct CellVideoOutConfiguration *)config,
        (struct CellVideoOutOption *)option);
}

static inline int32_t videoGetNumberOfDevice(uint32_t videoOut)
{
    return (int32_t)cellVideoOutGetNumberOfDevice(videoOut);
}

static inline int32_t videoGetDeviceInfo(uint32_t videoOut,
                                         uint32_t deviceIndex,
                                         videoDeviceInfo *info)
{
    return (int32_t)cellVideoOutGetDeviceInfo(
        videoOut, deviceIndex, (struct CellVideoOutDeviceInfo *)info);
}

static inline int32_t videoGetResolutionAvailability(uint32_t videoOut,
                                                     uint32_t resolutionId,
                                                     uint32_t aspect,
                                                     uint32_t option)
{
    return (int32_t)cellVideoOutGetResolutionAvailability(
        videoOut, resolutionId, aspect, option);
}

static inline int32_t videoDebugSetMonitorType(uint32_t videoOut,
                                               uint32_t monitorType)
{
    return (int32_t)cellVideoOutDebugSetMonitorType(videoOut, monitorType);
}

static inline int32_t videoGetConvertCursorColorInfo(uint8_t *rgbOutputRange)
{
    return (int32_t)cellVideoOutGetConvertCursorColorInfo(rgbOutputRange);
}

/* Callback register/unregister map onto the cell callback set.  The
 * extern is declared with a function-pointer type built from the
 * forward-declared device-info tag so it is a type-compatible
 * redeclaration of the typedef'd CellVideoOutCallback form in
 * <cell/cell_video_out.h> for TUs that see both (both reduce to the
 * same composite function-pointer type, same struct tag).  The legacy
 * videoCallback has the same ABI shape (six 32-bit args, a
 * layout-identical device-info pointer) and is cast through. */
typedef int (*__ps3dk_cellVideoOutCallback)(uint32_t slot, uint32_t videoOut,
                                            uint32_t deviceIndex,
                                            uint32_t event,
                                            struct CellVideoOutDeviceInfo *info,
                                            void *userData);

extern int cellVideoOutRegisterCallback(uint32_t slot,
                                        __ps3dk_cellVideoOutCallback function,
                                        void *userData);
extern int cellVideoOutUnregisterCallback(uint32_t slot);

static inline int32_t videoRegisterCallback(uint32_t slot,
                                            videoCallback cbVideo,
                                            void *userData)
{
    return (int32_t)cellVideoOutRegisterCallback(
        slot, (__ps3dk_cellVideoOutCallback)cbVideo, userData);
}

static inline int32_t videoUnregisterCallback(uint32_t slot)
{
    return (int32_t)cellVideoOutUnregisterCallback(slot);
}

#ifdef __cplusplus
}
#endif

#endif /* __LV2_VIDEO_H__ */
