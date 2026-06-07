/*
 * PS3 Custom Toolchain - cell/cell_video_out.h
 *
 * Video-output subsystem surface: resolution query, mode enumeration,
 * display-device discovery, configuration, gamma/cursor/HDCP control.
 *
 * CellVideoOut functions resolve through libsysutil_stub.a.
 * cellVideoOutSetGamma / GetGamma / GetScreenSize / ConvertCursorColor /
 * SetXVColor / SetupDisplay resolve through libsysutil_avconf_ext_stub.a
 * (cellSysutilAvconfExt module).
 *
 * This header is the authoritative source for the reference SDK video-out
 * type and constant namespace.  The sysutil/sysutil_sysparam.h alias
 * re-exports it for the reference SDK sample code that includes only the
 * sysutil path.
 */

#ifndef PS3TC_CELL_VIDEO_OUT_H
#define PS3TC_CELL_VIDEO_OUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Error codes
 * --------------------------------------------------------------------- */

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

/* ---------------------------------------------------------------------
 * Video-out identity
 * --------------------------------------------------------------------- */

typedef enum CellVideoOut {
	CELL_VIDEO_OUT_PRIMARY   = 0,
	CELL_VIDEO_OUT_SECONDARY = 1
} CellVideoOut;

/* ---------------------------------------------------------------------
 * Resolution ID
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutResolutionId {
	CELL_VIDEO_OUT_RESOLUTION_UNDEFINED     = 0,
	CELL_VIDEO_OUT_RESOLUTION_1080          = 1,
	CELL_VIDEO_OUT_RESOLUTION_720           = 2,
	CELL_VIDEO_OUT_RESOLUTION_480           = 4,
	CELL_VIDEO_OUT_RESOLUTION_576           = 5,
	CELL_VIDEO_OUT_RESOLUTION_1600x1080     = 10,
	CELL_VIDEO_OUT_RESOLUTION_1440x1080     = 11,
	CELL_VIDEO_OUT_RESOLUTION_1280x1080     = 12,
	CELL_VIDEO_OUT_RESOLUTION_960x1080      = 13,

	CELL_VIDEO_OUT_RESOLUTION_720_3D_FRAME_PACKING       = 0x81,
	CELL_VIDEO_OUT_RESOLUTION_1024x720_3D_FRAME_PACKING  = 0x88,
	CELL_VIDEO_OUT_RESOLUTION_960x720_3D_FRAME_PACKING   = 0x89,
	CELL_VIDEO_OUT_RESOLUTION_800x720_3D_FRAME_PACKING   = 0x8a,
	CELL_VIDEO_OUT_RESOLUTION_640x720_3D_FRAME_PACKING   = 0x8b,

	CELL_VIDEO_OUT_RESOLUTION_720_DUALVIEW_FRAME_PACKING       = 0x91,
	CELL_VIDEO_OUT_RESOLUTION_720_SIMULVIEW_FRAME_PACKING      = 0x91,
	CELL_VIDEO_OUT_RESOLUTION_1024x720_DUALVIEW_FRAME_PACKING  = 0x98,
	CELL_VIDEO_OUT_RESOLUTION_1024x720_SIMULVIEW_FRAME_PACKING = 0x98,
	CELL_VIDEO_OUT_RESOLUTION_960x720_DUALVIEW_FRAME_PACKING   = 0x99,
	CELL_VIDEO_OUT_RESOLUTION_960x720_SIMULVIEW_FRAME_PACKING  = 0x99,
	CELL_VIDEO_OUT_RESOLUTION_800x720_DUALVIEW_FRAME_PACKING   = 0x9a,
	CELL_VIDEO_OUT_RESOLUTION_800x720_SIMULVIEW_FRAME_PACKING  = 0x9a,
	CELL_VIDEO_OUT_RESOLUTION_640x720_DUALVIEW_FRAME_PACKING   = 0x9b,
	CELL_VIDEO_OUT_RESOLUTION_640x720_SIMULVIEW_FRAME_PACKING  = 0x9b
} CellVideoOutResolutionId;

/* ---------------------------------------------------------------------
 * Scan mode
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutScanMode {
	CELL_VIDEO_OUT_SCAN_MODE_INTERLACE   = 0,
	CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE = 1
} CellVideoOutScanMode;

/* ---------------------------------------------------------------------
 * Refresh rate (bitmask)
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutRefreshRate {
	CELL_VIDEO_OUT_REFRESH_RATE_AUTO    = 0x0000,
	CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ = 0x0001,
	CELL_VIDEO_OUT_REFRESH_RATE_50HZ    = 0x0002,
	CELL_VIDEO_OUT_REFRESH_RATE_60HZ    = 0x0004,
	CELL_VIDEO_OUT_REFRESH_RATE_30HZ    = 0x0008
} CellVideoOutRefreshRate;

/* ---------------------------------------------------------------------
 * Port type
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutPortType {
	CELL_VIDEO_OUT_PORT_NONE          = 0x00,
	CELL_VIDEO_OUT_PORT_HDMI          = 0x01,
	CELL_VIDEO_OUT_PORT_NETWORK       = 0x41,
	CELL_VIDEO_OUT_PORT_COMPOSITE_S   = 0x81,
	CELL_VIDEO_OUT_PORT_D             = 0x82,
	CELL_VIDEO_OUT_PORT_COMPONENT     = 0x83,
	CELL_VIDEO_OUT_PORT_RGB           = 0x84,
	CELL_VIDEO_OUT_PORT_AVMULTI_SCART = 0x85,
	CELL_VIDEO_OUT_PORT_DSUB          = 0x86
} CellVideoOutPortType;

/* ---------------------------------------------------------------------
 * Display aspect
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutDisplayAspect {
	CELL_VIDEO_OUT_ASPECT_AUTO = 0,
	CELL_VIDEO_OUT_ASPECT_4_3  = 1,
	CELL_VIDEO_OUT_ASPECT_16_9 = 2
} CellVideoOutDisplayAspect;

/* ---------------------------------------------------------------------
 * Buffer colour format
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutBufferColorFormat {
	CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8          = 0,
	CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8B8G8R8          = 1,
	CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_R16G16B16X16_FLOAT = 2
} CellVideoOutBufferColorFormat;

/* ---------------------------------------------------------------------
 * Output state
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutOutputState {
	CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED   = 0,
	CELL_VIDEO_OUT_OUTPUT_STATE_DISABLED  = 1,
	CELL_VIDEO_OUT_OUTPUT_STATE_PREPARING = 2
} CellVideoOutOutputState;

/* ---------------------------------------------------------------------
 * Device state
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutDeviceState {
	CELL_VIDEO_OUT_DEVICE_STATE_UNAVAILABLE = 0,
	CELL_VIDEO_OUT_DEVICE_STATE_AVAILABLE   = 1
} CellVideoOutDeviceState;

/* ---------------------------------------------------------------------
 * Colour space
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutColorSpace {
	CELL_VIDEO_OUT_COLOR_SPACE_RGB   = 0x01,
	CELL_VIDEO_OUT_COLOR_SPACE_YUV   = 0x02,
	CELL_VIDEO_OUT_COLOR_SPACE_XVYCC = 0x04
} CellVideoOutColorSpace;

/* ---------------------------------------------------------------------
 * Debug monitor type
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutDebugMonitorType {
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_UNDEFINED     = 0,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_480I_59_94HZ  = 1,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_576I_50HZ     = 2,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_480P_59_94HZ  = 3,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_576P_50HZ     = 4,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_1080I_59_94HZ = 5,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_720P_59_94HZ  = 7,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_1080P_59_94HZ = 9,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_WXGA_60HZ     = 11,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_SXGA_60HZ     = 12,
	CELL_VIDEO_OUT_DEBUG_MONITOR_TYPE_WUXGA_60HZ    = 13
} CellVideoOutDebugMonitorType;

/* ---------------------------------------------------------------------
 * Display conversion
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutDisplayConversion {
	CELL_VIDEO_OUT_DISPLAY_CONVERSION_NONE              = 0x00,
	CELL_VIDEO_OUT_DISPLAY_CONVERSION_TO_WXGA           = 0x01,
	CELL_VIDEO_OUT_DISPLAY_CONVERSION_TO_SXGA           = 0x02,
	CELL_VIDEO_OUT_DISPLAY_CONVERSION_TO_WUXGA          = 0x03,
	CELL_VIDEO_OUT_DISPLAY_CONVERSION_TO_1080           = 0x05,
	CELL_VIDEO_OUT_DISPLAY_CONVERSION_TO_REMOTEPLAY     = 0x10,
	CELL_VIDEO_OUT_DISPLAY_CONVERSION_TO_720_3D_FRAME_PACKING      = 0x80,
	CELL_VIDEO_OUT_DISPLAY_CONVERSION_TO_720_DUALVIEW_FRAME_PACKING = 0x81,
	CELL_VIDEO_OUT_DISPLAY_CONVERSION_TO_720_SIMULVIEW_FRAME_PACKING = 0x81
} CellVideoOutDisplayConversion;

/* ---------------------------------------------------------------------
 * Event
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutEvent {
	CELL_VIDEO_OUT_EVENT_DEVICE_CHANGED       = 0,
	CELL_VIDEO_OUT_EVENT_OUTPUT_DISABLED      = 1,
	CELL_VIDEO_OUT_EVENT_DEVICE_AUTHENTICATED = 2,
	CELL_VIDEO_OUT_EVENT_OUTPUT_ENABLED       = 3
} CellVideoOutEvent;

/* ---------------------------------------------------------------------
 * Copy control
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutCopyControl {
	CELL_VIDEO_OUT_COPY_CONTROL_COPY_FREE = 0,
	CELL_VIDEO_OUT_COPY_CONTROL_COPY_ONCE = 1,
	CELL_VIDEO_OUT_COPY_CONTROL_COPY_NEVER = 2
} CellVideoOutCopyControl;

/* ---------------------------------------------------------------------
 * RGB output range
 * --------------------------------------------------------------------- */

typedef enum CellVideoOutRGBOutputRange {
	CELL_VIDEO_OUT_RGB_OUTPUT_RANGE_LIMITED = 0,
	CELL_VIDEO_OUT_RGB_OUTPUT_RANGE_FULL    = 1
} CellVideoOutRGBOutputRange;

/* ---------------------------------------------------------------------
 * Struct definitions (oracle struct-tag names)
 * --------------------------------------------------------------------- */

typedef struct CellVideoOutResolution {
	uint16_t width;
	uint16_t height;
} CellVideoOutResolution;

typedef struct CellVideoOutDisplayMode {
	union { uint8_t resolution; uint8_t resolutionId; };
	uint8_t  scanMode;
	uint8_t  conversion;
	uint8_t  aspect;
	uint8_t  padding[2];
	uint16_t refreshRates;
} CellVideoOutDisplayMode;

typedef struct CellVideoOutState {
	uint8_t                  state;
	uint8_t                  colorSpace;
	uint8_t                  padding[6];
	CellVideoOutDisplayMode  displayMode;
} CellVideoOutState;

typedef struct CellVideoOutConfiguration {
	union { uint8_t resolution; uint8_t resolutionId; };
	uint8_t  format;
	uint8_t  aspect;
	uint8_t  padding[9];
	uint32_t pitch;
} CellVideoOutConfiguration;

typedef struct CellVideoOutOption {
	uint32_t reserved;
} CellVideoOutOption;

typedef struct CellVideoOutColorInfo {
	uint16_t redX;
	uint16_t redY;
	uint16_t greenX;
	uint16_t greenY;
	uint16_t blueX;
	uint16_t blueY;
	uint16_t whiteX;
	uint16_t whiteY;
	uint32_t gamma;
} CellVideoOutColorInfo;

typedef struct CellVideoOutKSVList {
	uint8_t  ksv[32 * 5];
	uint8_t  reserved[4];
	uint32_t count;
} CellVideoOutKSVList;

typedef struct CellVideoOutDeviceInfo {
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

/* ---------------------------------------------------------------------
 * Callback
 * --------------------------------------------------------------------- */

typedef int (*CellVideoOutCallback)(uint32_t slot,
                                    uint32_t videoOut,
                                    uint32_t deviceIndex,
                                    uint32_t event,
                                    CellVideoOutDeviceInfo *info,
                                    void *userData);

/* ---------------------------------------------------------------------
 * Function prototypes
 *   libsysutil_stub.a:  GetNumberOfDevice, GetDeviceInfo, RegisterCallback,
 *                       UnregisterCallback, GetState, GetResolution,
 *                       Configure, GetConfiguration,
 *                       GetResolutionAvailability, DebugSetMonitorType,
 *                       GetConvertCursorColorInfo
 *   libsysutil_avconf_ext_stub.a: SetGamma, GetGamma, GetScreenSize,
 *                       ConvertCursorColor, SetXVColor, SetupDisplay
 *   No stub (NID not yet identified): SetCopyControl
 * --------------------------------------------------------------------- */

int cellVideoOutGetNumberOfDevice(uint32_t videoOut);

int cellVideoOutGetDeviceInfo(uint32_t videoOut,
                              uint32_t deviceIndex,
                              CellVideoOutDeviceInfo *info);

int cellVideoOutRegisterCallback(uint32_t slot,
                                 CellVideoOutCallback function,
                                 void *userData);

int cellVideoOutUnregisterCallback(uint32_t slot);

int cellVideoOutGetState(uint32_t videoOut,
                         uint32_t deviceIndex,
                         CellVideoOutState *state);

int cellVideoOutGetResolution(uint32_t resolutionId,
                              CellVideoOutResolution *resolution);

int cellVideoOutConfigure(uint32_t videoOut,
                          CellVideoOutConfiguration *config,
                          CellVideoOutOption *option,
                          uint32_t blocking);

int cellVideoOutGetConfiguration(uint32_t videoOut,
                                 CellVideoOutConfiguration *config,
                                 CellVideoOutOption *option);

int cellVideoOutGetResolutionAvailability(uint32_t videoOut,
                                          uint32_t resolutionId,
                                          uint32_t aspect,
                                          uint32_t option);

int cellVideoOutDebugSetMonitorType(uint32_t videoOut,
                                    uint32_t monitorType);

int cellVideoOutGetConvertCursorColorInfo(uint8_t *rgbOutputRange);

/*
 * cellVideoOutSetCopyControl - HDCP copy-control.
 *
 * Prototype provided for source compatibility.  The NID has not been
 * identified yet; linking will fail until it is added to the stub
 * archive.
 */
int cellVideoOutSetCopyControl(uint32_t videoOut, uint32_t control);

/* libsysutil_avconf_ext_stub.a entry points */

int cellVideoOutSetGamma(uint32_t videoOut, float gamma);
int cellVideoOutGetGamma(uint32_t videoOut, float *gamma);
int cellVideoOutGetScreenSize(uint32_t videoOut, float *screenSize);
int cellVideoOutConvertCursorColor(uint32_t videoOut,
                                   int displaybuffer_format,
                                   float gamma,
                                   int source_buffer_format,
                                   void *src_addr,
                                   uint32_t *dest_addr,
                                   int num);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_VIDEO_OUT_H */
