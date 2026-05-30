#ifndef __PS3DK_CELL_SSCGPU_H__
#define __PS3DK_CELL_SSCGPU_H__

/* cell/sscgpu.h — GPU stereo-image converter surface (independent).
 *
 * Declares the cellSscGpu* entry points that libsscgpu.a exports.
 * Reference SDK ships these in <cell/ssc.h> alongside the CPU-side
 * cellSsc* family; we split GPU-only surface into its own header.
 */

#include <stdint.h>
#include <cell/gcm.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *CellSscGpuHandle;

typedef struct {
    uint32_t mainMemSize;
    uint32_t localMemSize;
} CellSscGpuAttr;

typedef struct {
    uint32_t mainMemAddr;
    uint32_t mainMemSize;
    uint32_t localMemAddr;
    uint32_t localMemSize;
} CellSscGpuResource;

/* Shared between CPU and GPU SSC paths; declared here because
 * cellSscGpuExec* variants reference it. */
typedef enum {
    CELL_SSC_DEPTH_TYPE_UNDEF = 0,
    CELL_SSC_DEPTH_TYPE_WEAK,
    CELL_SSC_DEPTH_TYPE_MEDIUM,
    CELL_SSC_DEPTH_TYPE_STRONG,
    CELL_SSC_DEPTH_TYPE_VERY_STRONG,
    CELL_SSC_DEPTH_TYPE_USER_CONTROL = 255
} CellSscDepthType;

typedef struct {
    CellSscDepthType depthType;
    float            screenSize;
    float            globalDepth1;
    float            globalDepth2;
    float            localDepth1;
    float            localDepth2;
    float            shift;
    int32_t          sidepanel;
} CellSscCtrlParam;

typedef struct {
    uint8_t  format;
    uint32_t remap;
    uint16_t width;
    uint16_t height;
    uint8_t  location;
    uint32_t pitch;
    uint32_t offset;
} CellSscGpuInputPictureInfo;

typedef struct {
    uint8_t  type;
    uint8_t  colorFormat;
    uint8_t  colorLocation;
    uint32_t colorOffset;
    uint32_t colorPitch;
    uint16_t width;
    uint16_t height;
    uint16_t x;
    uint16_t y;
} CellSscGpuOutputSurface;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} CellSscGpuOutputInfo;

int32_t cellSscGpuQueryAttr(CellSscGpuAttr *pAttr);
int32_t cellSscGpuOpen(const CellSscGpuResource *pRsrc,
                       CellSscGpuHandle *pHandle);
int32_t cellSscGpuClose(CellSscGpuHandle handle);
int32_t cellSscGpuExec(CellGcmContextData *thisContext,
                       CellSscGpuHandle handle,
                       const CellSscGpuInputPictureInfo *pInPicInfo,
                       const CellSscCtrlParam *pCtrl,
                       const CellSscGpuOutputSurface *pOutSurface,
                       const CellSscGpuOutputInfo *pOutInfoL,
                       const CellSscGpuOutputInfo *pOutInfoR);
int32_t cellSscGpuExecUnsafe(CellGcmContextData *thisContext,
                             CellSscGpuHandle handle,
                             const CellSscGpuInputPictureInfo *pInPicInfo,
                             const CellSscCtrlParam *pCtrl,
                             const CellSscGpuOutputSurface *pOutSurface,
                             const CellSscGpuOutputInfo *pOutInfoL,
                             const CellSscGpuOutputInfo *pOutInfoR);
int32_t cellSscGpuExecMeasure(CellGcmContextData *thisContext,
                              CellSscGpuHandle handle,
                              const CellSscGpuInputPictureInfo *pInPicInfo,
                              const CellSscCtrlParam *pCtrl,
                              const CellSscGpuOutputSurface *pOutSurface,
                              const CellSscGpuOutputInfo *pOutInfoL,
                              const CellSscGpuOutputInfo *pOutInfoR);
int32_t cellSscGpuExecMeasureSize(const int32_t size,
                                  CellSscGpuHandle handle,
                                  const CellSscGpuInputPictureInfo *pInPicInfo,
                                  const CellSscCtrlParam *pCtrl,
                                  const CellSscGpuOutputSurface *pOutSurface,
                                  const CellSscGpuOutputInfo *pOutInfoL,
                                  const CellSscGpuOutputInfo *pOutInfoR);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SSCGPU_H__ */
