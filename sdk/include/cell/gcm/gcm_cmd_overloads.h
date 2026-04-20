/*
 * PS3 Custom Toolchain — <cell/gcm/gcm_cmd_overloads.h>
 *
 * Auto-generated C++ overloads: for every ctx-taking cellGcm*
 * command emitter shipped under gcm_command_c.h, add a sibling
 * overload without the leading CellGcmContextData* argument that
 * forwards the call through gCellGcmCurrentContext.  Matches
 * the cell-SDK no-context convention so unmodified cell-SDK sample
 * code compiles against our SDK.
 *
 * Regenerate with the sdk build system if gcm_command_c.h's
 * signature list changes.
 */

#ifndef PS3TC_CELL_GCM_CMD_OVERLOADS_H
#define PS3TC_CELL_GCM_CMD_OVERLOADS_H

#ifdef __cplusplus

#include <cell/gcm/gcm_command_c.h>

static inline void cellGcmFlush(void)
{ cellGcmFlush(gCellGcmCurrentContext); }

static inline void cellGcmFlushUnsafe(void)
{ cellGcmFlushUnsafe(gCellGcmCurrentContext); }

static inline void cellGcmFinish(uint32_t ref)
{ cellGcmFinish(gCellGcmCurrentContext, ref); }

static inline void cellGcmFinishUnsafe(uint32_t ref)
{ cellGcmFinishUnsafe(gCellGcmCurrentContext, ref); }

static inline void cellGcmSetReferenceCommand(uint32_t ref)
{ cellGcmSetReferenceCommand(gCellGcmCurrentContext, ref); }

static inline void cellGcmSetSurface(const CellGcmSurface *surface)
{ cellGcmSetSurface(gCellGcmCurrentContext, surface); }

static inline void cellGcmSetClearColor(uint32_t color)
{ cellGcmSetClearColor(gCellGcmCurrentContext, color); }

static inline void cellGcmSetClearDepthStencil(uint32_t value)
{ cellGcmSetClearDepthStencil(gCellGcmCurrentContext, value); }

static inline void cellGcmSetClearSurface(uint32_t mask)
{ cellGcmSetClearSurface(gCellGcmCurrentContext, mask); }

static inline void cellGcmSetWaitLabel(uint8_t index, uint32_t value)
{ cellGcmSetWaitLabel(gCellGcmCurrentContext, index, value); }

static inline void cellGcmSetWriteCommandLabel(uint8_t index, uint32_t value)
{ cellGcmSetWriteCommandLabel(gCellGcmCurrentContext, index, value); }

static inline void cellGcmSetWriteBackEndLabel(uint8_t index, uint32_t value)
{ cellGcmSetWriteBackEndLabel(gCellGcmCurrentContext, index, value); }

static inline void cellGcmSetWriteTextureLabel(uint8_t index, uint32_t value)
{ cellGcmSetWriteTextureLabel(gCellGcmCurrentContext, index, value); }

static inline void cellGcmSetJumpCommand(uint32_t offset)
{ cellGcmSetJumpCommand(gCellGcmCurrentContext, offset); }

static inline void cellGcmSetCallCommand(uint32_t offset)
{ cellGcmSetCallCommand(gCellGcmCurrentContext, offset); }

static inline void cellGcmSetReturnCommand(void)
{ cellGcmSetReturnCommand(gCellGcmCurrentContext); }

static inline void cellGcmSetNopCommand(uint32_t count)
{ cellGcmSetNopCommand(gCellGcmCurrentContext, count); }

static inline void cellGcmSetFrontPolygonMode(uint32_t mode)
{ cellGcmSetFrontPolygonMode(gCellGcmCurrentContext, mode); }

static inline void cellGcmSetBackPolygonMode(uint32_t mode)
{ cellGcmSetBackPolygonMode(gCellGcmCurrentContext, mode); }

static inline void cellGcmSetColorMask(uint32_t mask)
{ cellGcmSetColorMask(gCellGcmCurrentContext, mask); }

static inline void cellGcmSetColorMaskMrt(uint32_t mask)
{ cellGcmSetColorMaskMrt(gCellGcmCurrentContext, mask); }

static inline void cellGcmSetDepthFunc(uint32_t func)
{ cellGcmSetDepthFunc(gCellGcmCurrentContext, func); }

static inline void cellGcmSetDepthTestEnable(uint32_t enable)
{ cellGcmSetDepthTestEnable(gCellGcmCurrentContext, enable); }

static inline void cellGcmSetDepthMask(uint32_t mask)
{ cellGcmSetDepthMask(gCellGcmCurrentContext, mask); }

static inline void cellGcmSetBlendEnable(uint32_t enable)
{ cellGcmSetBlendEnable(gCellGcmCurrentContext, enable); }

static inline void cellGcmSetShadeModel(uint32_t model)
{ cellGcmSetShadeModel(gCellGcmCurrentContext, model); }

static inline void cellGcmSetViewport(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      float min_z, float max_z,
                                      const float scale[4], const float offset[4])
{ cellGcmSetViewport(gCellGcmCurrentContext, x, y, w, h, min_z, max_z, scale, offset); }

static inline void cellGcmSetScissor(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{ cellGcmSetScissor(gCellGcmCurrentContext, x, y, w, h); }

static inline void cellGcmSetTexture(uint8_t index, const CellGcmTexture *texture)
{ cellGcmSetTexture(gCellGcmCurrentContext, index, texture); }

static inline void cellGcmSetTextureControl(uint8_t index, uint32_t enable,
                                            uint16_t minlod, uint16_t maxlod,
                                            uint8_t maxaniso)
{ cellGcmSetTextureControl(gCellGcmCurrentContext, index, enable, minlod, maxlod, maxaniso); }

static inline void cellGcmSetTextureFilter(uint8_t index, uint16_t bias,
                                           uint8_t min, uint8_t mag, uint8_t conv)
{ cellGcmSetTextureFilter(gCellGcmCurrentContext, index, bias, min, mag, conv); }

static inline void cellGcmSetTextureAddress(uint8_t index,
                                            uint8_t wraps, uint8_t wrapt, uint8_t wrapr,
                                            uint8_t unsignedRemap, uint8_t zfunc,
                                            uint8_t gamma)
{ cellGcmSetTextureAddress(gCellGcmCurrentContext, index, wraps, wrapt, wrapr, unsignedRemap, zfunc, gamma); }

static inline void cellGcmSetTimeStamp(uint32_t index)
{ cellGcmSetTimeStamp(gCellGcmCurrentContext, index); }

static inline void cellGcmSetCullFace(uint32_t cfm)
{ cellGcmSetCullFace(gCellGcmCurrentContext, cfm); }

static inline void cellGcmSetFrontFace(uint32_t dir)
{ cellGcmSetFrontFace(gCellGcmCurrentContext, dir); }

static inline void cellGcmSetCullFaceEnable(uint32_t enable)
{ cellGcmSetCullFaceEnable(gCellGcmCurrentContext, enable); }

static inline void cellGcmSetUserClipPlaneControl(uint32_t plane0, uint32_t plane1,
                                                  uint32_t plane2, uint32_t plane3,
                                                  uint32_t plane4, uint32_t plane5)
{ cellGcmSetUserClipPlaneControl(gCellGcmCurrentContext, plane0, plane1, plane2, plane3, plane4, plane5); }

static inline void cellGcmSetDrawArrays(uint32_t type, uint32_t start, uint32_t count)
{ cellGcmSetDrawArrays(gCellGcmCurrentContext, type, start, count); }

static inline void cellGcmSetDrawIndexArray(uint8_t type, uint32_t offset, uint32_t count,
                                            uint8_t data_type, uint8_t location)
{ cellGcmSetDrawIndexArray(gCellGcmCurrentContext, type, offset, count, data_type, location); }

static inline void cellGcmSetDrawBegin(uint32_t type)
{ cellGcmSetDrawBegin(gCellGcmCurrentContext, type); }

static inline void cellGcmSetDrawEnd(void)
{ cellGcmSetDrawEnd(gCellGcmCurrentContext); }

static inline void cellGcmSetVertexData4f(uint8_t index, const float v[4])
{ cellGcmSetVertexData4f(gCellGcmCurrentContext, index, v); }

static inline void cellGcmSetVertexDataArray(uint8_t index, uint16_t frequency,
                                             uint8_t stride, uint8_t size,
                                             uint8_t type, uint8_t location,
                                             uint32_t offset)
{ cellGcmSetVertexDataArray(gCellGcmCurrentContext, index, frequency, stride, size, type, location, offset); }

static inline void cellGcmSetVertexAttribInputMask(uint16_t mask)
{ cellGcmSetVertexAttribInputMask(gCellGcmCurrentContext, mask); }

static inline void cellGcmSetVertexAttribOutputMask(uint32_t mask)
{ cellGcmSetVertexAttribOutputMask(gCellGcmCurrentContext, mask); }

static inline void cellGcmSetVertexProgramConstants(uint32_t start, uint32_t count,
                                                    const float *data)
{ cellGcmSetVertexProgramConstants(gCellGcmCurrentContext, start, count, data); }

static inline void cellGcmSetFragmentProgramGammaEnable(uint32_t enable)
{ cellGcmSetFragmentProgramGammaEnable(gCellGcmCurrentContext, enable); }

static inline void cellGcmSetInvalidateVertexCache(void)
{ cellGcmSetInvalidateVertexCache(gCellGcmCurrentContext); }

static inline void cellGcmSetInvalidateTextureCache(uint32_t type)
{ cellGcmSetInvalidateTextureCache(gCellGcmCurrentContext, type); }

static inline void cellGcmSetTransferDataMode(uint8_t mode)
{ cellGcmSetTransferDataMode(gCellGcmCurrentContext, mode); }

static inline void cellGcmSetTransferDataOffset(uint32_t dst, uint32_t src)
{ cellGcmSetTransferDataOffset(gCellGcmCurrentContext, dst, src); }

static inline void cellGcmSetTransferDataFormat(int32_t inpitch, int32_t outpitch,
                                                uint32_t linelength, uint32_t linecount,
                                                uint8_t inbytes, uint8_t outbytes)
{ cellGcmSetTransferDataFormat(gCellGcmCurrentContext, inpitch, outpitch, linelength, linecount, inbytes, outbytes); }

static inline void cellGcmSetTransferData(uint8_t mode, uint32_t dst, uint32_t outpitch,
                                          uint32_t src, uint32_t inpitch,
                                          uint32_t linelength, uint32_t linecount)
{ cellGcmSetTransferData(gCellGcmCurrentContext, mode, dst, outpitch, src, inpitch, linelength, linecount); }

static inline void cellGcmSetTransferImage(uint8_t mode,
                                           uint32_t dstOffset, uint32_t dstPitch,
                                           uint32_t dstX, uint32_t dstY,
                                           uint32_t srcOffset, uint32_t srcPitch,
                                           uint32_t srcX, uint32_t srcY,
                                           uint32_t width, uint32_t height,
                                           uint32_t bytesPerPixel)
{ cellGcmSetTransferImage(gCellGcmCurrentContext, mode, dstOffset, dstPitch, dstX, dstY, srcOffset, srcPitch, srcX, srcY, width, height, bytesPerPixel); }

#endif  /* __cplusplus */

#endif  /* PS3TC_CELL_GCM_CMD_OVERLOADS_H */
