/* sscgpu.c — GPU stereo-image converter stubs (Slice 1e link gate).
 */
#include <cell/sscgpu.h>

int32_t cellSscGpuQueryAttr(CellSscGpuAttr *pAttr)
{ (void)pAttr; return -1; }

int32_t cellSscGpuOpen(const CellSscGpuResource *pRsrc,
                       CellSscGpuHandle *pHandle)
{ (void)pRsrc; (void)pHandle; return -1; }

int32_t cellSscGpuClose(CellSscGpuHandle handle)
{ (void)handle; return -1; }

int32_t cellSscGpuExec(CellGcmContextData *thisContext,
                       CellSscGpuHandle handle,
                       const CellSscGpuInputPictureInfo *pInPicInfo,
                       const CellSscCtrlParam *pCtrl,
                       const CellSscGpuOutputSurface *pOutSurface,
                       const CellSscGpuOutputInfo *pOutInfoL,
                       const CellSscGpuOutputInfo *pOutInfoR)
{
    (void)thisContext; (void)handle; (void)pInPicInfo;
    (void)pCtrl; (void)pOutSurface;
    (void)pOutInfoL; (void)pOutInfoR;
    return -1;
}

int32_t cellSscGpuExecUnsafe(CellGcmContextData *thisContext,
                             CellSscGpuHandle handle,
                             const CellSscGpuInputPictureInfo *pInPicInfo,
                             const CellSscCtrlParam *pCtrl,
                             const CellSscGpuOutputSurface *pOutSurface,
                             const CellSscGpuOutputInfo *pOutInfoL,
                             const CellSscGpuOutputInfo *pOutInfoR)
{
    (void)thisContext; (void)handle; (void)pInPicInfo;
    (void)pCtrl; (void)pOutSurface;
    (void)pOutInfoL; (void)pOutInfoR;
    return -1;
}

int32_t cellSscGpuExecMeasure(CellGcmContextData *thisContext,
                              CellSscGpuHandle handle,
                              const CellSscGpuInputPictureInfo *pInPicInfo,
                              const CellSscCtrlParam *pCtrl,
                              const CellSscGpuOutputSurface *pOutSurface,
                              const CellSscGpuOutputInfo *pOutInfoL,
                              const CellSscGpuOutputInfo *pOutInfoR)
{
    (void)thisContext; (void)handle; (void)pInPicInfo;
    (void)pCtrl; (void)pOutSurface;
    (void)pOutInfoL; (void)pOutInfoR;
    return -1;
}

int32_t cellSscGpuExecMeasureSize(const int32_t size,
                                  CellSscGpuHandle handle,
                                  const CellSscGpuInputPictureInfo *pInPicInfo,
                                  const CellSscCtrlParam *pCtrl,
                                  const CellSscGpuOutputSurface *pOutSurface,
                                  const CellSscGpuOutputInfo *pOutInfoL,
                                  const CellSscGpuOutputInfo *pOutInfoR)
{
    (void)size; (void)handle; (void)pInPicInfo;
    (void)pCtrl; (void)pOutSurface;
    (void)pOutInfoL; (void)pOutInfoR;
    return -1;
}
