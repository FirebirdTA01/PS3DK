/*
 * PS3 Custom Toolchain — sdk/libgcm_cmd/src/gcm_rsx_wrappers.c
 *
 * Link-time Cell-named entry points for the commands the rsx layer
 * already emits: 18 base functions and 119 *Unsafe variants, declared in
 * <cell/gcm/gcm_command_link.h>.  Each forwards to its rsx* emitter;
 * cellGcmSetupContextData is written out.
 */

#include <stdint.h>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <rsx/commands.h>
#include <sys/lv2_types.h>

typedef gcmContextData CellGcmContextData;
typedef gcmContextCallback CellGcmContextCallback;
typedef struct _cellGcmSurface CellGcmSurface;
typedef gcmTransferScale CellGcmTransferScale;
typedef gcmTransferSurface CellGcmTransferSurface;
typedef gcmTransferSwizzle CellGcmTransferSwizzle;

#include <cell/gcm/gcm_command_link.h>


/* ==================================================================== */
/* Base commands                                                        */
/* ==================================================================== */

uint32_t * cellGcmGetCurrentBuffer(void)
{
    return (uint32_t *)rsxGetCurrentBuffer();
}

void cellGcmInlineTransfer(CellGcmContextData *context, uint32_t dstOffset, const void *srcAdr, uint32_t sizeInWords, uint8_t location)
{
    rsxInlineTransfer((gcmContextData *)context, dstOffset, srcAdr, sizeInWords, location);
}

void cellGcmSetBlendEnableMrt(CellGcmContextData *context, uint32_t mrt1, uint32_t mrt2, uint32_t mrt3)
{
    rsxSetBlendEnableMrt((gcmContextData *)context, mrt1, mrt2, mrt3);
}

void cellGcmSetBlendOptimization(CellGcmContextData *context, uint32_t enable)
{
    rsxSetBlendOptimization((gcmContextData *)context, enable);
}

void cellGcmSetConvertSwizzleFormat(CellGcmContextData *context, uint32_t dstOffset, uint32_t dstWidth, uint32_t dstHeight, uint32_t dstX, uint32_t dstY, uint32_t srcOffset, uint32_t srcPitch, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height, uint32_t bytesPerPixel, uint32_t mode)
{
    rsxSetConvertSwizzleFormat((gcmContextData *)context, dstOffset, dstWidth, dstHeight, dstX, dstY, srcOffset, srcPitch, srcX, srcY, width, height, bytesPerPixel, mode);
}

void cellGcmSetCylindricalWrap(CellGcmContextData *context, uint32_t enable, uint32_t reserved)
{
    (void)reserved;
    rsxSetCylindricalWrap((gcmContextData *)context, enable);
}

void cellGcmSetDepthFormat(CellGcmContextData *context, uint32_t format)
{
    rsxSetDepthFormat((gcmContextData *)context, format);
}

void cellGcmSetPointSize(CellGcmContextData *context, float size)
{
    rsxSetPointSize((gcmContextData *)context, size);
}

void cellGcmSetPointSpriteControl(CellGcmContextData *context, uint32_t enable, uint32_t rMode, uint32_t texCoords)
{
    rsxSetPointSpriteControl((gcmContextData *)context, enable, rMode, texCoords);
}

void cellGcmSetPolygonOffsetLineEnable(CellGcmContextData *context, uint32_t enable)
{
    rsxSetPolygonOffsetLineEnable((gcmContextData *)context, enable);
}

void cellGcmSetRestartIndex(CellGcmContextData *context, uint32_t index)
{
    rsxSetRestartIndex((gcmContextData *)context, index);
}

void cellGcmSetRestartIndexEnable(CellGcmContextData *context, uint32_t enable)
{
    rsxSetRestartIndexEnable((gcmContextData *)context, enable);
}

void cellGcmSetSkipNop(CellGcmContextData *context, uint32_t count)
{
    rsxSetSkipNop((gcmContextData *)context, count);
}

void cellGcmSetTransformBranchBits(CellGcmContextData *context, uint32_t branchBits)
{
    rsxSetTransformBranchBits((gcmContextData *)context, branchBits);
}

void cellGcmSetTwoSideLightEnable(CellGcmContextData *context, uint32_t enable)
{
    rsxSetTwoSideLightEnable((gcmContextData *)context, enable);
}

void cellGcmSetUserCallback(CellGcmContextCallback callback)
{
    rsxSetUserCallback((gcmContextCallback)callback);
}

void cellGcmSetWaitForIdle(CellGcmContextData *context)
{
    rsxSetWaitForIdle((gcmContextData *)context);
}

void cellGcmSetupContextData(CellGcmContextData *context, const uint32_t *addr, const uint32_t size, CellGcmContextCallback callback)
{
    /* Not forwarded to rsxSetupContextData: that also writes the inline
       transfer DMA binding into the new buffer, and a measuring context is
       set up over a NULL buffer that must never be written.  size is in
       bytes. */
    context->begin = (uint32_t *)addr;
    context->current = (uint32_t *)addr;
    context->end = (uint32_t *)((uintptr_t)addr + (size & ~3u) - 4);
    context->callback = (CellGcmContextCallback)(uintptr_t)lv2_fn_to_callback_ea((const void *)callback);
}

/* ==================================================================== */
/* 119 cellGcm*Unsafe Variants                                          */
/* ==================================================================== */

void cellGcmSetReturnCommandUnsafe(CellGcmContextData * context)
{
    rsxSetReturnCommandUnsafe((gcmContextData *)context);
}

void cellGcmSetCallCommandUnsafe(CellGcmContextData * context, uint32_t offset)
{
    rsxSetCallCommandUnsafe((gcmContextData *)context, offset);
}

void cellGcmSetJumpCommandUnsafe(CellGcmContextData * context, uint32_t offset)
{
    rsxSetJumpCommandUnsafe((gcmContextData *)context, offset);
}

void cellGcmSetNopCommandUnsafe(CellGcmContextData * context, uint32_t count)
{
    rsxSetNopCommandUnsafe((gcmContextData *)context, count);
}

void cellGcmSetSkipNopUnsafe(CellGcmContextData * context, uint32_t count)
{
    rsxSetSkipNopUnsafe((gcmContextData *)context, count);
}

void cellGcmSetClearColorUnsafe(CellGcmContextData * context, uint32_t color)
{
    rsxSetClearColorUnsafe((gcmContextData *)context, color);
}

void cellGcmSetClearDepthStencilUnsafe(CellGcmContextData * context, uint32_t value)
{
    rsxSetClearDepthStencilUnsafe((gcmContextData *)context, value);
}

void cellGcmSetReferenceCommandUnsafe(CellGcmContextData * context, uint32_t ref_value)
{
    rsxSetReferenceCommandUnsafe((gcmContextData *)context, ref_value);
}

void cellGcmSetWriteBackEndLabelUnsafe(CellGcmContextData * context, uint8_t index, uint32_t value)
{
    rsxSetWriteBackendLabelUnsafe((gcmContextData *)context, index, value);
}

void cellGcmSetWriteTextureLabelUnsafe(CellGcmContextData * context, uint8_t index, uint32_t value)
{
    rsxSetWriteTextureLabelUnsafe((gcmContextData *)context, index, value);
}

void cellGcmSetWaitLabelUnsafe(CellGcmContextData * context, uint8_t index, uint32_t value)
{
    rsxSetWaitLabelUnsafe((gcmContextData *)context, index, value);
}

void cellGcmSetWriteCommandLabelUnsafe(CellGcmContextData * context, uint8_t index, uint32_t value)
{
    rsxSetWriteCommandLabelUnsafe((gcmContextData *)context, index, value);
}

void cellGcmSetSurfaceUnsafe(CellGcmContextData *context, const CellGcmSurface *surface)
{
    rsxSetSurfaceUnsafe((gcmContextData *)context, (const gcmSurface *)surface);
}

void cellGcmSetColorMaskUnsafe(CellGcmContextData * context, uint32_t mask)
{
    rsxSetColorMaskUnsafe((gcmContextData *)context, mask);
}

void cellGcmSetColorMaskMrtUnsafe(CellGcmContextData * context, uint32_t mask)
{
    rsxSetColorMaskMrtUnsafe((gcmContextData *)context, mask);
}

void cellGcmSetShadeModeUnsafe(CellGcmContextData * context, uint32_t shadeModel)
{
    rsxSetShadeModelUnsafe((gcmContextData *)context, shadeModel);
}

void cellGcmSetViewportUnsafe(CellGcmContextData * context, uint16_t x, uint16_t y, uint16_t width, uint16_t height, float min, float max, const float scale[4], const float offset[4])
{
    rsxSetViewportUnsafe((gcmContextData *)context, x, y, width, height, min, max, scale, offset);
}

void cellGcmSetUserClipPlaneControlUnsafe(CellGcmContextData * context, uint32_t plane0, uint32_t plane1, uint32_t plane2, uint32_t plane3, uint32_t plane4, uint32_t plane5)
{
    rsxSetUserClipPlaneControlUnsafe((gcmContextData *)context, plane0, plane1, plane2, plane3, plane4, plane5);
}

void cellGcmSetDepthTestEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetDepthTestEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetDepthFuncUnsafe(CellGcmContextData * context, uint32_t func)
{
    rsxSetDepthFuncUnsafe((gcmContextData *)context, func);
}

void cellGcmSetDepthMaskUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetDepthWriteEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetDepthFormatUnsafe(CellGcmContextData * context, uint32_t format)
{
    rsxSetDepthFormatUnsafe((gcmContextData *)context, format);
}

void cellGcmSetCullFaceEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetCullFaceEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetCullFaceUnsafe(CellGcmContextData * context, uint32_t cull)
{
    rsxSetCullFaceUnsafe((gcmContextData *)context, cull);
}

void cellGcmSetFrontFaceUnsafe(CellGcmContextData * context, uint32_t dir)
{
    rsxSetFrontFaceUnsafe((gcmContextData *)context, dir);
}

void cellGcmSetFrontPolygonModeUnsafe(CellGcmContextData * context, uint32_t mode)
{
    rsxSetFrontPolygonModeUnsafe((gcmContextData *)context, mode);
}

void cellGcmSetBackPolygonModeUnsafe(CellGcmContextData * context, uint32_t mode)
{
    rsxSetBackPolygonModeUnsafe((gcmContextData *)context, mode);
}

void cellGcmSetPolygonOffsetFillEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetPolygonOffsetFillEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetPolygonOffsetUnsafe(CellGcmContextData * context, float factor, float units)
{
    rsxSetPolygonOffsetUnsafe((gcmContextData *)context, factor, units);
}

void cellGcmSetPolygonOffsetLineEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetPolygonOffsetLineEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetClearSurfaceUnsafe(CellGcmContextData * context, uint32_t clear_mask)
{
    rsxClearSurfaceUnsafe((gcmContextData *)context, clear_mask);
}

void cellGcmSetCylindricalWrapUnsafe(CellGcmContextData *context, uint32_t enable, uint32_t reserved)
{
    (void)reserved;
    rsxSetCylindricalWrapUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetTwoSideLightEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetTwoSideLightEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetStencilFuncUnsafe(CellGcmContextData *context, uint32_t func, int32_t ref, uint32_t mask)
{
    rsxSetStencilFuncUnsafe((gcmContextData *)context, func, (uint32_t)ref, mask);
}

void cellGcmSetStencilMaskUnsafe(CellGcmContextData * context, uint32_t mask)
{
    rsxSetStencilMaskUnsafe((gcmContextData *)context, mask);
}

void cellGcmSetStencilOpUnsafe(CellGcmContextData * context, uint32_t fail, uint32_t depthFail, uint32_t depthPass)
{
    rsxSetStencilOpUnsafe((gcmContextData *)context, fail, depthFail, depthPass);
}

void cellGcmSetStencilTestEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetStencilTestEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetBackStencilFuncUnsafe(CellGcmContextData *context, uint32_t func, int32_t ref, uint32_t mask)
{
    rsxSetBackStencilFuncUnsafe((gcmContextData *)context, func, (uint32_t)ref, mask);
}

void cellGcmSetBackStencilMaskUnsafe(CellGcmContextData * context, uint32_t mask)
{
    rsxSetBackStencilMaskUnsafe((gcmContextData *)context, mask);
}

void cellGcmSetBackStencilOpUnsafe(CellGcmContextData * context, uint32_t fail, uint32_t depthFail, uint32_t depthPass)
{
    rsxSetBackStencilOpUnsafe((gcmContextData *)context, fail, depthFail, depthPass);
}

void cellGcmSetTwoSidedStencilTestEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetTwoSidedStencilTestEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetRenderEnableUnsafe(CellGcmContextData * context, uint8_t mode, uint32_t index)
{
    rsxSetRenderEnableUnsafe((gcmContextData *)context, mode, index);
}

void cellGcmSetReportUnsafe(CellGcmContextData * context, uint32_t type, uint32_t index)
{
    rsxSetReportUnsafe((gcmContextData *)context, type, index);
}

void cellGcmSetClearReportUnsafe(CellGcmContextData * context, uint32_t type)
{
    rsxSetClearReportUnsafe((gcmContextData *)context, type);
}

void cellGcmSetZpassPixelCountEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetZPixelCountEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetScullControlUnsafe(CellGcmContextData * context, uint8_t sFunc, uint8_t sRef, uint8_t sMask)
{
    rsxSetSCullControlUnsafe((gcmContextData *)context, sFunc, sRef, sMask);
}

void cellGcmSetZcullLimitUnsafe(CellGcmContextData * context, uint16_t moveforwardlimit, uint16_t pushbacklimit)
{
    rsxSetZCullLimitUnsafe((gcmContextData *)context, moveforwardlimit, pushbacklimit);
}

void cellGcmSetZcullStatsEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetZCullStatsEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetZcullControlUnsafe(CellGcmContextData * context, uint8_t zculldir, uint8_t zcullformat)
{
    rsxSetZCullControlUnsafe((gcmContextData *)context, zculldir, zcullformat);
}

void cellGcmSetClearZcullSurfaceUnsafe(CellGcmContextData * context, uint32_t depth, uint32_t stencil)
{
    rsxSetClearZCullSurfaceUnsafe((gcmContextData *)context, depth, stencil);
}

void cellGcmSetZcullEnableUnsafe(CellGcmContextData * context, uint32_t depth, uint32_t stencil)
{
    rsxSetZCullEnableUnsafe((gcmContextData *)context, depth, stencil);
}

void cellGcmSetInvalidateZcullUnsafe(CellGcmContextData * context)
{
    rsxSetZCullInvalidateUnsafe((gcmContextData *)context);
}

void cellGcmSetPolySmoothEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetPolygonSmoothEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetLineSmoothEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetLineSmoothEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetDitherEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetDitherEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetUpdateFragmentProgramParameterUnsafe(CellGcmContextData *context, uint32_t offset)
{
    rsxUpdateFragmentProgramLocationUnsafe((gcmContextData *)context, offset, GCM_LOCATION_RSX);
}

void cellGcmSetVertexProgramParameterBlockUnsafe(CellGcmContextData *context, uint32_t baseConst, uint32_t constCount, const float *values)
{
    rsxLoadVertexProgramParameterBlockUnsafe((gcmContextData *)context, baseConst, constCount, values);
}

void cellGcmSetVertexProgramConstantsUnsafe(CellGcmContextData * context, uint32_t start, uint32_t count, const float * data)
{
    rsxSetVertexProgramConstantsUnsafe((gcmContextData *)context, start, count, data);
}

void cellGcmSetVertexAttribOutputMaskUnsafe(CellGcmContextData * context, uint32_t mask)
{
    rsxSetVertexAttribOutputMaskUnsafe((gcmContextData *)context, mask);
}

void cellGcmSetFragmentProgramGammaEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetFragmentProgramGammaEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetDrawBeginUnsafe(CellGcmContextData *context, uint8_t mode)
{
    rsxDrawVertexBeginUnsafe((gcmContextData *)context, mode);
}

void cellGcmSetDrawEndUnsafe(CellGcmContextData * context)
{
    rsxDrawVertexEndUnsafe((gcmContextData *)context);
}

void cellGcmSetVertexData1fUnsafe(CellGcmContextData * context, uint8_t idx, float v)
{
    rsxDrawVertex1fUnsafe((gcmContextData *)context, idx, v);
}

void cellGcmSetVertexData2fUnsafe(CellGcmContextData * context, uint8_t idx, const float v[2])
{
    rsxDrawVertex2fUnsafe((gcmContextData *)context, idx, v);
}

void cellGcmSetVertexData3fUnsafe(CellGcmContextData * context, uint8_t idx, const float v[3])
{
    rsxDrawVertex3fUnsafe((gcmContextData *)context, idx, v);
}

void cellGcmSetVertexData4fUnsafe(CellGcmContextData * context, uint8_t idx, const float v[4])
{
    rsxDrawVertex4fUnsafe((gcmContextData *)context, idx, v);
}

void cellGcmSetVertexData4sUnsafe(CellGcmContextData * context, uint8_t idx, const int16_t v[4])
{
    rsxDrawVertex4sUnsafe((gcmContextData *)context, idx, v);
}

void cellGcmSetVertexDataScaled4sUnsafe(CellGcmContextData * context, uint8_t idx, const int16_t v[4])
{
    rsxDrawVertexScaled4sUnsafe((gcmContextData *)context, idx, v);
}

void cellGcmSetVertexData2sUnsafe(CellGcmContextData * context, uint8_t idx, const int16_t v[2])
{
    rsxDrawVertex2sUnsafe((gcmContextData *)context, idx, v);
}

void cellGcmSetVertexData4ubUnsafe(CellGcmContextData * context, uint8_t idx, const uint8_t v[4])
{
    rsxDrawVertex4ubUnsafe((gcmContextData *)context, idx, v);
}

void cellGcmSetFrequencyDividerOperationUnsafe(CellGcmContextData * context, uint16_t operation)
{
    rsxSetFrequencyDividerOperationUnsafe((gcmContextData *)context, operation);
}

void cellGcmSetInvalidateVertexCacheUnsafe(CellGcmContextData * context)
{
    rsxInvalidateVertexCacheUnsafe((gcmContextData *)context);
}

void cellGcmSetInvalidateTextureCacheUnsafe(CellGcmContextData * context, uint32_t type)
{
    rsxInvalidateTextureCacheUnsafe((gcmContextData *)context, type);
}

void cellGcmSetVertexTextureControlUnsafe(CellGcmContextData * context, uint8_t index, uint32_t enable, uint16_t minlod, uint16_t maxlod)
{
    rsxVertexTextureControlUnsafe((gcmContextData *)context, index, enable, minlod, maxlod);
}

void cellGcmSetVertexTextureFilterUnsafe(CellGcmContextData * context, uint8_t index, uint16_t bias)
{
    rsxVertexTextureFilterUnsafe((gcmContextData *)context, index, bias);
}

void cellGcmSetVertexTextureAddressUnsafe(CellGcmContextData * context, uint8_t index, uint8_t wraps, uint8_t wrapt)
{
    rsxVertexTextureWrapModeUnsafe((gcmContextData *)context, index, wraps, wrapt);
}

void cellGcmSetTextureBorderColorUnsafe(CellGcmContextData * context, uint8_t index, uint32_t color)
{
    rsxTextureBorderColorUnsafe((gcmContextData *)context, index, color);
}

void cellGcmSetVertexTextureBorderColorUnsafe(CellGcmContextData * context, uint8_t index, uint32_t color)
{
    rsxVertexTextureBorderColorUnsafe((gcmContextData *)context, index, color);
}

void cellGcmSetAnisoSpreadUnsafe(CellGcmContextData * context, uint8_t index, uint8_t reduceSamplesEnable, uint8_t hReduceSamplesEnable, uint8_t vReduceSamplesEnable, uint8_t spacingSelect, uint8_t hSpacingSelect, uint8_t vSpacingSelect)
{
    rsxTextureAnisoSpreadUnsafe((gcmContextData *)context, index, reduceSamplesEnable, hReduceSamplesEnable, vReduceSamplesEnable, spacingSelect, hSpacingSelect, vSpacingSelect);
}

void cellGcmSetZMinMaxControlUnsafe(CellGcmContextData *context, uint32_t cullNearFarEnable, uint32_t zclampEnable, uint32_t cullIgnoreW)
{
    rsxSetZMinMaxControlUnsafe((gcmContextData *)context, (uint8_t)cullNearFarEnable, (uint8_t)zclampEnable, (uint8_t)cullIgnoreW);
}

void cellGcmSetDepthBoundsTestEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetDepthBoundsTestEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetDepthBoundsUnsafe(CellGcmContextData * context, float zMin, float zMax)
{
    rsxSetDepthBoundsUnsafe((gcmContextData *)context, zMin, zMax);
}

void cellGcmSetVertexDataArrayUnsafe(CellGcmContextData *context, uint8_t index, uint16_t frequency, uint8_t stride, uint8_t size, uint8_t type, uint8_t location, uint32_t offset)
{
    rsxBindVertexArrayAttribUnsafe((gcmContextData *)context, index, frequency, offset, stride, size, type, location);
}

void cellGcmSetDrawArraysUnsafe(CellGcmContextData *context, uint8_t mode, uint32_t start, uint32_t count)
{
    rsxDrawVertexArrayUnsafe((gcmContextData *)context, mode, start, count);
}

void cellGcmSetDrawInlineArrayUnsafe(CellGcmContextData * context, uint8_t type, uint32_t count, const void * data)
{
    rsxDrawInlineVertexArrayUnsafe((gcmContextData *)context, type, count, data);
}

void cellGcmSetDrawIndexArrayUnsafe(CellGcmContextData *context, uint8_t mode, uint32_t count, uint8_t type, uint8_t location, uint32_t indicies)
{
    rsxDrawIndexArrayUnsafe((gcmContextData *)context, mode, indicies, count, type, location);
}

void cellGcmSetDrawInlineIndexArray16Unsafe(CellGcmContextData * context, uint8_t type, uint32_t start, uint32_t count, const uint16_t * data)
{
    rsxDrawInlineIndexArray16Unsafe((gcmContextData *)context, type, start, count, data);
}

void cellGcmSetDrawInlineIndexArray32Unsafe(CellGcmContextData * context, uint8_t type, uint32_t start, uint32_t count, const uint32_t * data)
{
    rsxDrawInlineIndexArray32Unsafe((gcmContextData *)context, type, start, count, data);
}

void cellGcmSetRestartIndexEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetRestartIndexEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetRestartIndexUnsafe(CellGcmContextData * context, uint32_t index)
{
    rsxSetRestartIndexUnsafe((gcmContextData *)context, index);
}

void cellGcmSetScissorUnsafe(CellGcmContextData * context, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    rsxSetScissorUnsafe((gcmContextData *)context, x, y, w, h);
}

void cellGcmSetAntiAliasingControlUnsafe(CellGcmContextData * context, uint32_t enable, uint32_t alphaToCoverage, uint32_t alphaToOne, uint32_t sampleMask)
{
    rsxSetAntialiasingControlUnsafe((gcmContextData *)context, enable, alphaToCoverage, alphaToOne, sampleMask);
}

void cellGcmInlineTransferUnsafe(CellGcmContextData * context, uint32_t dstOffset, const void * srcAddress, uint32_t sizeInWords, uint8_t location)
{
    rsxInlineTransferUnsafe((gcmContextData *)context, dstOffset, srcAddress, sizeInWords, location);
}

void cellGcmSetAlphaFuncUnsafe(CellGcmContextData * context, uint32_t alphaFunc, uint32_t ref)
{
    rsxSetAlphaFuncUnsafe((gcmContextData *)context, alphaFunc, ref);
}

void cellGcmSetAlphaTestEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetAlphaTestEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetBlendFuncUnsafe(CellGcmContextData * context, uint16_t sfcolor, uint16_t dfcolor, uint16_t sfalpha, uint16_t dfalpha)
{
    rsxSetBlendFuncUnsafe((gcmContextData *)context, sfcolor, dfcolor, sfalpha, dfalpha);
}

void cellGcmSetBlendEquationUnsafe(CellGcmContextData * context, uint16_t color, uint16_t alpha)
{
    rsxSetBlendEquationUnsafe((gcmContextData *)context, color, alpha);
}

void cellGcmSetBlendColorUnsafe(CellGcmContextData * context, uint32_t color0, uint32_t color1)
{
    rsxSetBlendColorUnsafe((gcmContextData *)context, color0, color1);
}

void cellGcmSetBlendEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetBlendEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetBlendOptimizationUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetBlendOptimizationUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetBlendEnableMrtUnsafe(CellGcmContextData * context, uint32_t mrt1, uint32_t mrt2, uint32_t mrt3)
{
    rsxSetBlendEnableMrtUnsafe((gcmContextData *)context, mrt1, mrt2, mrt3);
}

void cellGcmSetLogicOpUnsafe(CellGcmContextData * context, uint32_t op)
{
    rsxSetLogicOpUnsafe((gcmContextData *)context, op);
}

void cellGcmSetLogicOpEnableUnsafe(CellGcmContextData * context, uint32_t enable)
{
    rsxSetLogicOpEnableUnsafe((gcmContextData *)context, enable);
}

void cellGcmSetFogModeUnsafe(CellGcmContextData * context, uint32_t mode)
{
    rsxSetFogModeUnsafe((gcmContextData *)context, mode);
}

void cellGcmSetFogParamsUnsafe(CellGcmContextData * context, float p0, float p1)
{
    rsxSetFogParamsUnsafe((gcmContextData *)context, p0, p1);
}

void cellGcmSetTransformBranchBitsUnsafe(CellGcmContextData * context, uint32_t branchBits)
{
    rsxSetTransformBranchBitsUnsafe((gcmContextData *)context, branchBits);
}

void cellGcmSetPointSpriteControlUnsafe(CellGcmContextData * context, uint32_t enable, uint32_t rmode, uint32_t texcoordMask)
{
    rsxSetPointSpriteControlUnsafe((gcmContextData *)context, enable, rmode, texcoordMask);
}

void cellGcmSetPointSizeUnsafe(CellGcmContextData * context, float size)
{
    rsxSetPointSizeUnsafe((gcmContextData *)context, size);
}

void cellGcmSetTransferDataUnsafe(CellGcmContextData * context, uint8_t mode, uint32_t dst, uint32_t outpitch, uint32_t src, uint32_t inpitch, uint32_t linelength, uint32_t linecount)
{
    rsxSetTransferDataUnsafe((gcmContextData *)context, mode, dst, outpitch, src, inpitch, linelength, linecount);
}

void cellGcmSetTransferDataModeUnsafe(CellGcmContextData * context, uint8_t mode)
{
    rsxSetTransferDataModeUnsafe((gcmContextData *)context, mode);
}

void cellGcmSetTransferDataOffsetUnsafe(CellGcmContextData * context, uint32_t dst, uint32_t src)
{
    rsxSetTransferDataOffsetUnsafe((gcmContextData *)context, dst, src);
}

void cellGcmSetTransferDataFormatUnsafe(CellGcmContextData * context, int32_t inpitch, int32_t outpitch, uint32_t linelength, uint32_t linecount, uint8_t inbytes, uint8_t outbytes)
{
    rsxSetTransferDataFormatUnsafe((gcmContextData *)context, inpitch, outpitch, linelength, linecount, inbytes, outbytes);
}

void cellGcmSetTransferImageUnsafe(CellGcmContextData * context, uint8_t mode, uint32_t dstOffset, uint32_t dstPitch, uint32_t dstX, uint32_t dstY, uint32_t srcOffset, uint32_t srcPitch, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height, uint32_t bytesPerPixel)
{
    rsxSetTransferImageUnsafe((gcmContextData *)context, mode, dstOffset, dstPitch, dstX, dstY, srcOffset, srcPitch, srcX, srcY, width, height, bytesPerPixel);
}

void cellGcmSetTransferScaleModeUnsafe(CellGcmContextData *context, uint8_t mode, uint8_t surface)
{
    rsxSetTransferScaleModeUnsafe((gcmContextData *)context, mode, surface);
}

void cellGcmSetTransferScaleSurfaceUnsafe(CellGcmContextData *context, const CellGcmTransferScale *scale, const CellGcmTransferSurface *surface)
{
    rsxSetTransferScaleSurfaceUnsafe((gcmContextData *)context, (const gcmTransferScale *)scale, (const gcmTransferSurface *)surface);
}

void cellGcmSetTransferScaleSwizzleUnsafe(CellGcmContextData *context, const CellGcmTransferScale *scale, const CellGcmTransferSwizzle *swizzle)
{
    rsxSetTransferScaleSwizzleUnsafe((gcmContextData *)context, (const gcmTransferScale *)scale, (const gcmTransferSwizzle *)swizzle);
}

void cellGcmSetConvertSwizzleFormatUnsafe(CellGcmContextData *context, uint32_t dstOffset, uint32_t dstWidth, uint32_t dstHeight, uint32_t dstX, uint32_t dstY, uint32_t srcOffset, uint32_t srcPitch, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height, uint32_t bytesPerPixel, uint32_t mode)
{
    rsxSetConvertSwizzleFormatUnsafe((gcmContextData *)context, dstOffset, dstWidth, dstHeight, dstX, dstY, srcOffset, srcPitch, srcX, srcY, width, height, bytesPerPixel, mode);
}

void cellGcmSetWaitForIdleUnsafe(CellGcmContextData * context)
{
    rsxSetWaitForIdleUnsafe((gcmContextData *)context);
}

void cellGcmResetDefaultCommandBufferUnsafe(CellGcmContextData * context)
{
    rsxResetCommandBufferUnsafe((gcmContextData *)context);
}

