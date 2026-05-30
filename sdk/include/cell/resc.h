#ifndef __PS3DK_CELL_RESC_H__
#define __PS3DK_CELL_RESC_H__

#include <cell/cell_video_out.h>
#include <cell/error.h>
#include <cell/gcm.h>
#include <ppu-types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_RESC_ERROR_NOT_INITIALIZED   0x80210301
#define CELL_RESC_ERROR_REINITIALIZED     0x80210302
#define CELL_RESC_ERROR_BAD_ALIGNMENT     0x80210303
#define CELL_RESC_ERROR_BAD_ARGUMENT      0x80210304
#define CELL_RESC_ERROR_LESS_MEMORY       0x80210305
#define CELL_RESC_ERROR_GCM_FLIP_QUE_FULL 0x80210306
#define CELL_RESC_ERROR_BAD_COMBINATION   0x80210307

typedef enum CellRescResourcePolicy {
    CELL_RESC_CONSTANT_VRAM = 0,
    CELL_RESC_MINIMUM_VRAM = 1,
    CELL_RESC_CONSTANT_GPU_LOAD = 0,
    CELL_RESC_MINIMUM_GPU_LOAD = 2
} CellRescResourcePolicy;

typedef enum CellRescBufferMode {
    CELL_RESC_UNDEFINED = 0,
    CELL_RESC_720x480 = 1,
    CELL_RESC_720x576 = 2,
    CELL_RESC_1280x720 = 4,
    CELL_RESC_1920x1080 = 8
} CellRescBufferMode;

typedef enum CellRescRatioConvertMode {
    CELL_RESC_FULLSCREEN = 0,
    CELL_RESC_LETTERBOX = 1,
    CELL_RESC_PANSCAN = 2
} CellRescRatioConvertMode;

typedef enum CellRescPalTemporalMode {
    CELL_RESC_PAL_50 = 0,
    CELL_RESC_PAL_60_DROP = 1,
    CELL_RESC_PAL_60_INTERPOLATE = 2,
    CELL_RESC_PAL_60_INTERPOLATE_30_DROP = 3,
    CELL_RESC_PAL_60_INTERPOLATE_DROP_FLEXIBLE = 4,
    CELL_RESC_PAL_60_FOR_HSYNC = 5
} CellRescPalTemporalMode;

typedef enum CellRescInterlaceMode {
    CELL_RESC_NORMAL_BILINEAR = 0,
    CELL_RESC_INTERLACE_FILTER = 1,
    CELL_RESC_3X3_GAUSSIAN = 2,
    CELL_RESC_2X3_QUINCUNX = 3,
    CELL_RESC_2X3_QUINCUNX_ALT = 4
} CellRescInterlaceMode;

typedef enum CellRescTableElement {
    CELL_RESC_ELEMENT_HALF = 0,
    CELL_RESC_ELEMENT_FLOAT = 1
} CellRescTableElement;

typedef enum CellRescFlipMode {
    CELL_RESC_DISPLAY_VSYNC = 0,
    CELL_RESC_DISPLAY_HSYNC = 1
} CellRescFlipMode;

typedef enum CellRescSurfaceFormat {
    CELL_RESC_SURFACE_A8R8G8B8 = CELL_GCM_SURFACE_A8R8G8B8,
    CELL_RESC_SURFACE_F_W16Z16Y16X16 = CELL_GCM_SURFACE_F_W16Z16Y16X16
} CellRescSurfaceFormat;

typedef struct CellRescInitConfig {
    uint32_t size;
    CellRescResourcePolicy resourcePolicy;
    uint32_t supportModes;
    CellRescRatioConvertMode ratioMode;
    CellRescPalTemporalMode palTemporalMode;
    CellRescInterlaceMode interlaceMode;
    CellRescFlipMode flipMode;
} CellRescInitConfig;

typedef struct CellRescSrc {
    uint32_t format;
    uint32_t pitch;
    uint16_t width;
    uint16_t height;
    uint32_t offset;
} CellRescSrc;

typedef struct CellRescDsts {
    CellRescSurfaceFormat format;
    uint32_t pitch;
    uint32_t heightAlign;
} CellRescDsts;

typedef void (*CellRescHandler)(uint32_t head);

int32_t cellRescInit(const CellRescInitConfig * const initConfig);
void cellRescExit(void);
int32_t cellRescSetDsts(const CellRescBufferMode dstsMode, const CellRescDsts * const dsts);
int32_t cellRescSetDisplayMode(const CellRescBufferMode bufferMode);
int32_t cellRescGetNumColorBuffers(const CellRescBufferMode dstMode, const CellRescPalTemporalMode palTemporalMode, const uint32_t reserved);
int32_t cellRescGetBufferSize(int32_t * const colorBuffers, int32_t * const vertexArray, int32_t * const fragmentShader);
int32_t cellRescSetBufferAddress(const void * const colorBuffers, const void * const vertexArray, const void * const fragmentShader);
int32_t cellRescSetSrc(const int32_t idx, const CellRescSrc * const src);
int32_t cellRescSetConvertAndFlip(CellGcmContextData *con, const int32_t idx);
void cellRescSetWaitFlip(CellGcmContextData *con);
system_time_t cellRescGetLastFlipTime(void);
void cellRescResetFlipStatus(void);
uint32_t cellRescGetFlipStatus(void);
int32_t cellRescGetRegisterCount(void);
void cellRescSetRegisterCount(const int32_t regCount);
int32_t cellRescSetPalInterpolateDropFlexRatio(const float ratio);
int32_t cellRescCreateInterlaceTable(void *ea, const float srcH, const CellRescTableElement depth, const int length);
int32_t cellRescAdjustAspectRatio(const float horizontal, const float vertical);
void cellRescSetVBlankHandler(const CellRescHandler handler);
void cellRescSetFlipHandler(const CellRescHandler handler);
int32_t cellRescGcmSurface2RescSrc(const CellGcmSurface * const gcmSurface, CellRescSrc * const rescSrc);
int32_t cellRescVideoOutResolutionId2RescBufferMode(const CellVideoOutResolutionId resolutionId, CellRescBufferMode * const bufferMode);

#ifdef __cplusplus
}

static inline int32_t cellRescSetConvertAndFlip(const int32_t idx)
{
    return cellRescSetConvertAndFlip(static_cast<CellGcmContextData *>(0), idx);
}

static inline void cellRescSetWaitFlip(void)
{
    cellRescSetWaitFlip(static_cast<CellGcmContextData *>(0));
}
#endif

#endif /* __PS3DK_CELL_RESC_H__ */
