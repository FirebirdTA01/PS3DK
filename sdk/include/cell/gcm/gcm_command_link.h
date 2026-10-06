/*
 * PS3 Custom Toolchain — sdk/include/cell/gcm/gcm_command_link.h
 *
 * Link-time Cell GCM commands that the rsx command layer already emits:
 * 18 base commands and 119 *Unsafe variants, 137 in all, defined in
 * libgcm_cmd.a.
 *
 * Prototypes only, no typedefs: <cell/gcm.h> includes this after the
 * context, surface and transfer types and the static inline commands.
 * None of these names is declared anywhere else.
 */

#ifndef __PS3TC_CELL_GCM_COMMAND_LINK_H__
#define __PS3TC_CELL_GCM_COMMAND_LINK_H__

#include <stdint.h>
#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================== */
/* Base commands                                                        */
/* ==================================================================== */

uint32_t * cellGcmGetCurrentBuffer(void);
void cellGcmInlineTransfer(CellGcmContextData *context, uint32_t dstOffset, const void *srcAdr, uint32_t sizeInWords, uint8_t location);
void cellGcmSetBlendEnableMrt(CellGcmContextData *context, uint32_t mrt1, uint32_t mrt2, uint32_t mrt3);
void cellGcmSetBlendOptimization(CellGcmContextData *context, uint32_t enable);
void cellGcmSetConvertSwizzleFormat(CellGcmContextData *context, uint32_t dstOffset, uint32_t dstWidth, uint32_t dstHeight, uint32_t dstX, uint32_t dstY, uint32_t srcOffset, uint32_t srcPitch, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height, uint32_t bytesPerPixel, uint32_t mode);
void cellGcmSetCylindricalWrap(CellGcmContextData *context, uint32_t enable, uint32_t reserved);
void cellGcmSetDepthFormat(CellGcmContextData *context, uint32_t format);
void cellGcmSetPointSize(CellGcmContextData *context, float size);
void cellGcmSetPointSpriteControl(CellGcmContextData *context, uint32_t enable, uint32_t rMode, uint32_t texCoords);
void cellGcmSetPolygonOffsetLineEnable(CellGcmContextData *context, uint32_t enable);
void cellGcmSetRestartIndex(CellGcmContextData *context, uint32_t index);
void cellGcmSetRestartIndexEnable(CellGcmContextData *context, uint32_t enable);
void cellGcmSetSkipNop(CellGcmContextData *context, uint32_t count);
void cellGcmSetTransformBranchBits(CellGcmContextData *context, uint32_t branchBits);
void cellGcmSetTwoSideLightEnable(CellGcmContextData *context, uint32_t enable);
void cellGcmSetUserCallback(CellGcmContextCallback callback);
void cellGcmSetWaitForIdle(CellGcmContextData *context);
void cellGcmSetupContextData(CellGcmContextData *context, const uint32_t *addr, const uint32_t size, CellGcmContextCallback callback);

/* ==================================================================== */
/* 119 cellGcm*Unsafe Variants                                          */
/* ==================================================================== */

void cellGcmSetReturnCommandUnsafe(CellGcmContextData * context);
void cellGcmSetCallCommandUnsafe(CellGcmContextData * context, uint32_t offset);
void cellGcmSetJumpCommandUnsafe(CellGcmContextData * context, uint32_t offset);
void cellGcmSetNopCommandUnsafe(CellGcmContextData * context, uint32_t count);
void cellGcmSetSkipNopUnsafe(CellGcmContextData * context, uint32_t count);
void cellGcmSetClearColorUnsafe(CellGcmContextData * context, uint32_t color);
void cellGcmSetClearDepthStencilUnsafe(CellGcmContextData * context, uint32_t value);
void cellGcmSetReferenceCommandUnsafe(CellGcmContextData * context, uint32_t ref_value);
void cellGcmSetWriteBackEndLabelUnsafe(CellGcmContextData * context, uint8_t index, uint32_t value);
void cellGcmSetWriteTextureLabelUnsafe(CellGcmContextData * context, uint8_t index, uint32_t value);
void cellGcmSetWaitLabelUnsafe(CellGcmContextData * context, uint8_t index, uint32_t value);
void cellGcmSetWriteCommandLabelUnsafe(CellGcmContextData * context, uint8_t index, uint32_t value);
void cellGcmSetSurfaceUnsafe(CellGcmContextData *context, const CellGcmSurface *surface);
void cellGcmSetColorMaskUnsafe(CellGcmContextData * context, uint32_t mask);
void cellGcmSetColorMaskMrtUnsafe(CellGcmContextData * context, uint32_t mask);
void cellGcmSetShadeModeUnsafe(CellGcmContextData * context, uint32_t shadeModel);
void cellGcmSetViewportUnsafe(CellGcmContextData * context, uint16_t x, uint16_t y, uint16_t width, uint16_t height, float min, float max, const float scale[4], const float offset[4]);
void cellGcmSetUserClipPlaneControlUnsafe(CellGcmContextData * context, uint32_t plane0, uint32_t plane1, uint32_t plane2, uint32_t plane3, uint32_t plane4, uint32_t plane5);
void cellGcmSetDepthTestEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetDepthFuncUnsafe(CellGcmContextData * context, uint32_t func);
void cellGcmSetDepthMaskUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetDepthFormatUnsafe(CellGcmContextData * context, uint32_t format);
void cellGcmSetCullFaceEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetCullFaceUnsafe(CellGcmContextData * context, uint32_t cull);
void cellGcmSetFrontFaceUnsafe(CellGcmContextData * context, uint32_t dir);
void cellGcmSetFrontPolygonModeUnsafe(CellGcmContextData * context, uint32_t mode);
void cellGcmSetBackPolygonModeUnsafe(CellGcmContextData * context, uint32_t mode);
void cellGcmSetPolygonOffsetFillEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetPolygonOffsetUnsafe(CellGcmContextData * context, float factor, float units);
void cellGcmSetPolygonOffsetLineEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetClearSurfaceUnsafe(CellGcmContextData * context, uint32_t clear_mask);
void cellGcmSetCylindricalWrapUnsafe(CellGcmContextData *context, uint32_t enable, uint32_t reserved);
void cellGcmSetTwoSideLightEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetStencilFuncUnsafe(CellGcmContextData *context, uint32_t func, int32_t ref, uint32_t mask);
void cellGcmSetStencilMaskUnsafe(CellGcmContextData * context, uint32_t mask);
void cellGcmSetStencilOpUnsafe(CellGcmContextData * context, uint32_t fail, uint32_t depthFail, uint32_t depthPass);
void cellGcmSetStencilTestEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetBackStencilFuncUnsafe(CellGcmContextData *context, uint32_t func, int32_t ref, uint32_t mask);
void cellGcmSetBackStencilMaskUnsafe(CellGcmContextData * context, uint32_t mask);
void cellGcmSetBackStencilOpUnsafe(CellGcmContextData * context, uint32_t fail, uint32_t depthFail, uint32_t depthPass);
void cellGcmSetTwoSidedStencilTestEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetRenderEnableUnsafe(CellGcmContextData * context, uint8_t mode, uint32_t index);
void cellGcmSetReportUnsafe(CellGcmContextData * context, uint32_t type, uint32_t index);
void cellGcmSetClearReportUnsafe(CellGcmContextData * context, uint32_t type);
void cellGcmSetZpassPixelCountEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetScullControlUnsafe(CellGcmContextData * context, uint8_t sFunc, uint8_t sRef, uint8_t sMask);
void cellGcmSetZcullLimitUnsafe(CellGcmContextData * context, uint16_t moveforwardlimit, uint16_t pushbacklimit);
void cellGcmSetZcullStatsEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetZcullControlUnsafe(CellGcmContextData * context, uint8_t zculldir, uint8_t zcullformat);
void cellGcmSetClearZcullSurfaceUnsafe(CellGcmContextData * context, uint32_t depth, uint32_t stencil);
void cellGcmSetZcullEnableUnsafe(CellGcmContextData * context, uint32_t depth, uint32_t stencil);
void cellGcmSetInvalidateZcullUnsafe(CellGcmContextData * context);
void cellGcmSetPolySmoothEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetLineSmoothEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetDitherEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetUpdateFragmentProgramParameterUnsafe(CellGcmContextData *context, uint32_t offset);
void cellGcmSetVertexProgramParameterBlockUnsafe(CellGcmContextData *context, uint32_t baseConst, uint32_t constCount, const float *values);
void cellGcmSetVertexProgramConstantsUnsafe(CellGcmContextData * context, uint32_t start, uint32_t count, const float * data);
void cellGcmSetVertexAttribOutputMaskUnsafe(CellGcmContextData * context, uint32_t mask);
void cellGcmSetFragmentProgramGammaEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetDrawBeginUnsafe(CellGcmContextData *context, uint8_t mode);
void cellGcmSetDrawEndUnsafe(CellGcmContextData * context);
void cellGcmSetVertexData1fUnsafe(CellGcmContextData * context, uint8_t idx, float v);
void cellGcmSetVertexData2fUnsafe(CellGcmContextData * context, uint8_t idx, const float v[2]);
void cellGcmSetVertexData3fUnsafe(CellGcmContextData * context, uint8_t idx, const float v[3]);
void cellGcmSetVertexData4fUnsafe(CellGcmContextData * context, uint8_t idx, const float v[4]);
void cellGcmSetVertexData4sUnsafe(CellGcmContextData * context, uint8_t idx, const int16_t v[4]);
void cellGcmSetVertexDataScaled4sUnsafe(CellGcmContextData * context, uint8_t idx, const int16_t v[4]);
void cellGcmSetVertexData2sUnsafe(CellGcmContextData * context, uint8_t idx, const int16_t v[2]);
void cellGcmSetVertexData4ubUnsafe(CellGcmContextData * context, uint8_t idx, const uint8_t v[4]);
void cellGcmSetFrequencyDividerOperationUnsafe(CellGcmContextData * context, uint16_t operation);
void cellGcmSetInvalidateVertexCacheUnsafe(CellGcmContextData * context);
void cellGcmSetInvalidateTextureCacheUnsafe(CellGcmContextData * context, uint32_t type);
void cellGcmSetVertexTextureControlUnsafe(CellGcmContextData * context, uint8_t index, uint32_t enable, uint16_t minlod, uint16_t maxlod);
void cellGcmSetVertexTextureFilterUnsafe(CellGcmContextData * context, uint8_t index, uint16_t bias);
void cellGcmSetVertexTextureAddressUnsafe(CellGcmContextData * context, uint8_t index, uint8_t wraps, uint8_t wrapt);
void cellGcmSetTextureBorderColorUnsafe(CellGcmContextData * context, uint8_t index, uint32_t color);
void cellGcmSetVertexTextureBorderColorUnsafe(CellGcmContextData * context, uint8_t index, uint32_t color);
void cellGcmSetAnisoSpreadUnsafe(CellGcmContextData * context, uint8_t index, uint8_t reduceSamplesEnable, uint8_t hReduceSamplesEnable, uint8_t vReduceSamplesEnable, uint8_t spacingSelect, uint8_t hSpacingSelect, uint8_t vSpacingSelect);
void cellGcmSetZMinMaxControlUnsafe(CellGcmContextData *context, uint32_t cullNearFarEnable, uint32_t zclampEnable, uint32_t cullIgnoreW);
void cellGcmSetDepthBoundsTestEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetDepthBoundsUnsafe(CellGcmContextData * context, float zMin, float zMax);
void cellGcmSetVertexDataArrayUnsafe(CellGcmContextData *context, uint8_t index, uint16_t frequency, uint8_t stride, uint8_t size, uint8_t type, uint8_t location, uint32_t offset);
void cellGcmSetDrawArraysUnsafe(CellGcmContextData *context, uint8_t mode, uint32_t start, uint32_t count);
void cellGcmSetDrawInlineArrayUnsafe(CellGcmContextData * context, uint8_t type, uint32_t count, const void * data);
void cellGcmSetDrawIndexArrayUnsafe(CellGcmContextData *context, uint8_t mode, uint32_t count, uint8_t type, uint8_t location, uint32_t indicies);
void cellGcmSetDrawInlineIndexArray16Unsafe(CellGcmContextData * context, uint8_t type, uint32_t start, uint32_t count, const uint16_t * data);
void cellGcmSetDrawInlineIndexArray32Unsafe(CellGcmContextData * context, uint8_t type, uint32_t start, uint32_t count, const uint32_t * data);
void cellGcmSetRestartIndexEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetRestartIndexUnsafe(CellGcmContextData * context, uint32_t index);
void cellGcmSetScissorUnsafe(CellGcmContextData * context, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void cellGcmSetAntiAliasingControlUnsafe(CellGcmContextData * context, uint32_t enable, uint32_t alphaToCoverage, uint32_t alphaToOne, uint32_t sampleMask);
void cellGcmInlineTransferUnsafe(CellGcmContextData * context, uint32_t dstOffset, const void * srcAddress, uint32_t sizeInWords, uint8_t location);
void cellGcmSetAlphaFuncUnsafe(CellGcmContextData * context, uint32_t alphaFunc, uint32_t ref);
void cellGcmSetAlphaTestEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetBlendFuncUnsafe(CellGcmContextData * context, uint16_t sfcolor, uint16_t dfcolor, uint16_t sfalpha, uint16_t dfalpha);
void cellGcmSetBlendEquationUnsafe(CellGcmContextData * context, uint16_t color, uint16_t alpha);
void cellGcmSetBlendColorUnsafe(CellGcmContextData * context, uint32_t color0, uint32_t color1);
void cellGcmSetBlendEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetBlendOptimizationUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetBlendEnableMrtUnsafe(CellGcmContextData * context, uint32_t mrt1, uint32_t mrt2, uint32_t mrt3);
void cellGcmSetLogicOpUnsafe(CellGcmContextData * context, uint32_t op);
void cellGcmSetLogicOpEnableUnsafe(CellGcmContextData * context, uint32_t enable);
void cellGcmSetFogModeUnsafe(CellGcmContextData * context, uint32_t mode);
void cellGcmSetFogParamsUnsafe(CellGcmContextData * context, float p0, float p1);
void cellGcmSetTransformBranchBitsUnsafe(CellGcmContextData * context, uint32_t branchBits);
void cellGcmSetPointSpriteControlUnsafe(CellGcmContextData * context, uint32_t enable, uint32_t rmode, uint32_t texcoordMask);
void cellGcmSetPointSizeUnsafe(CellGcmContextData * context, float size);
void cellGcmSetTransferDataUnsafe(CellGcmContextData * context, uint8_t mode, uint32_t dst, uint32_t outpitch, uint32_t src, uint32_t inpitch, uint32_t linelength, uint32_t linecount);
void cellGcmSetTransferDataModeUnsafe(CellGcmContextData * context, uint8_t mode);
void cellGcmSetTransferDataOffsetUnsafe(CellGcmContextData * context, uint32_t dst, uint32_t src);
void cellGcmSetTransferDataFormatUnsafe(CellGcmContextData * context, int32_t inpitch, int32_t outpitch, uint32_t linelength, uint32_t linecount, uint8_t inbytes, uint8_t outbytes);
void cellGcmSetTransferImageUnsafe(CellGcmContextData * context, uint8_t mode, uint32_t dstOffset, uint32_t dstPitch, uint32_t dstX, uint32_t dstY, uint32_t srcOffset, uint32_t srcPitch, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height, uint32_t bytesPerPixel);
void cellGcmSetTransferScaleModeUnsafe(CellGcmContextData *context, uint8_t mode, uint8_t surface);
void cellGcmSetTransferScaleSurfaceUnsafe(CellGcmContextData *context, const CellGcmTransferScale *scale, const CellGcmTransferSurface *surface);
void cellGcmSetTransferScaleSwizzleUnsafe(CellGcmContextData *context, const CellGcmTransferScale *scale, const CellGcmTransferSwizzle *swizzle);
void cellGcmSetConvertSwizzleFormatUnsafe(CellGcmContextData *context, uint32_t dstOffset, uint32_t dstWidth, uint32_t dstHeight, uint32_t dstX, uint32_t dstY, uint32_t srcOffset, uint32_t srcPitch, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height, uint32_t bytesPerPixel, uint32_t mode);
void cellGcmSetWaitForIdleUnsafe(CellGcmContextData * context);
void cellGcmResetDefaultCommandBufferUnsafe(CellGcmContextData * context);

#ifdef __cplusplus
}
#endif

#endif /* __PS3TC_CELL_GCM_COMMAND_LINK_H__ */
