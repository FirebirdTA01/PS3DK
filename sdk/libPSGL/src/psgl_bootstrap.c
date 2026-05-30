/* psgl_bootstrap.c — PSGL device/context/swap stubs (Slice 1d link gate).
 *
 * Every public psgl* entry point returns a spec-correct default:
 *   pointer returns → NULL
 *   GLboolean / int32_t → GL_FALSE / CELL_OK
 *   GLuint → 0
 *   void → no-op
 */
#include <PSGL/psgl.h>
#include <PSGL/psglu.h>
#include <PSGL/report.h>

/* ── lifecycle ──────────────────────────────────────────────────── */

PSGL_EXPORT void psglInit(PSGLinitOptions *options)
{
    (void)options;
}

PSGL_EXPORT void psglExit(void) {}

PSGL_EXPORT PSGLdevice *psglCreateDeviceAuto(GLenum colorFormat,
                                             GLenum depthFormat,
                                             GLenum multisamplingMode)
{
    (void)colorFormat;
    (void)depthFormat;
    (void)multisamplingMode;
    return NULL;
}

PSGL_EXPORT PSGLdevice *psglCreateDeviceExtended(
    const PSGLdeviceParameters *parameters)
{
    (void)parameters;
    return NULL;
}

PSGL_EXPORT GLfloat psglGetDeviceAspectRatio(const PSGLdevice *device)
{
    (void)device;
    return 1.0f;
}

PSGL_EXPORT void psglGetDeviceDimensions(const PSGLdevice *device,
                                         GLuint *width, GLuint *height)
{
    (void)device;
    if (width)  *width  = 0;
    if (height) *height = 0;
}

PSGL_EXPORT void psglGetRenderBufferDimensions(const PSGLdevice *device,
                                               GLuint *width, GLuint *height)
{
    (void)device;
    if (width)  *width  = 0;
    if (height) *height = 0;
}

PSGL_EXPORT void psglDestroyDevice(PSGLdevice *device)
{
    (void)device;
}

/* ── context ─────────────────────────────────────────────────────── */

PSGL_EXPORT void psglMakeCurrent(PSGLcontext *context, PSGLdevice *device)
{
    (void)context;
    (void)device;
}

PSGL_EXPORT PSGLcontext *psglCreateContext(void)
{
    return NULL;
}

PSGL_EXPORT void psglDestroyContext(PSGLcontext *context)
{
    (void)context;
}

PSGL_EXPORT void psglResetCurrentContext(void) {}

PSGL_EXPORT PSGLcontext *psglGetCurrentContext(void)
{
    return NULL;
}

PSGL_EXPORT PSGLdevice *psglGetCurrentDevice(void)
{
    return NULL;
}

/* ── frame control ───────────────────────────────────────────────── */

PSGL_EXPORT void psglSwap(void) {}

PSGL_EXPORT PSGLuint64 psglGetSystemTime(void)
{
    return 0ULL;
}

PSGL_EXPORT PSGLuint64 psglGetLastFlipTime(void)
{
    return 0ULL;
}

PSGL_EXPORT void psglSetFlipHandler(void (*handler)(const GLuint head))
{
    (void)handler;
}

PSGL_EXPORT void psglSetVBlankHandler(void (*handler)(const GLuint head))
{
    (void)handler;
}

/* ── shader library ──────────────────────────────────────────────── */

PSGL_EXPORT void psglLoadShaderLibrary(const char *filename)
{
    (void)filename;
}

PSGL_EXPORT void *psglGetSPUInitData(void)
{
    return NULL;
}

/* ── allocators ──────────────────────────────────────────────────── */

PSGL_EXPORT GLboolean psglSetAllocatorFuncs(PSGLmallocFunc mallocFunc,
                                            PSGLmemalignFunc memalignFunc,
                                            PSGLreallocFunc reallocFunc,
                                            PSGLfreeFunc freeFunc)
{
    (void)mallocFunc;
    (void)memalignFunc;
    (void)reallocFunc;
    (void)freeFunc;
    return GL_TRUE;
}

PSGL_EXPORT void psglSetBounceBufferSize(GLsizei size)
{
    (void)size;
}

PSGL_EXPORT GLsizei psglGetBounceBufferSize(void)
{
    return 0;
}

/* ── low-level RSX ───────────────────────────────────────────────── */

PSGL_EXPORT void psglAddressToOffset(const void *address, GLuint *offset)
{
    (void)address;
    if (offset) *offset = 0;
}

PSGL_EXPORT void psglSetVertexProgramRegister(
    GLuint reg, const void *__restrict value)
{
    (void)reg;
    (void)value;
}

PSGL_EXPORT void psglSetVertexProgramRegisterBlock(
    GLuint reg, GLuint count, const void *__restrict value)
{
    (void)reg;
    (void)count;
    (void)value;
}

PSGL_EXPORT void psglSetVertexProgramTransformBranchBits(GLuint values)
{
    (void)values;
}

PSGL_EXPORT void psglSetVertexProgramConfiguration(
    const CellCgbVertexProgramConfiguration *conf,
    const void *ucodeStorageAddress)
{
    (void)conf;
    (void)ucodeStorageAddress;
}

PSGL_EXPORT void psglSetFragmentProgramEmbeddedConstant(
    const GLuint offset, const GLfloat *value, const GLuint sizeInWords)
{
    (void)offset;
    (void)value;
    (void)sizeInWords;
}

PSGL_EXPORT void psglSetFragmentProgramConfiguration(
    const CellCgbFragmentProgramConfiguration *conf)
{
    (void)conf;
}

PSGL_EXPORT void psglSetFragmentProgramEmbeddedConstantMemoryLocation(
    const GLuint offset, const GLfloat *value, const GLuint sizeInWords,
    bool inLocalMemory)
{
    (void)offset;
    (void)value;
    (void)sizeInWords;
    (void)inLocalMemory;
}

PSGL_EXPORT void psglSetFragmentProgramConfigurationMemoryLocation(
    const CellCgbFragmentProgramConfiguration *conf, bool inLocalMemory)
{
    (void)conf;
    (void)inLocalMemory;
}

/* ── user clip planes ────────────────────────────────────────────── */

PSGL_EXPORT void psglSetUserClipPlanes(
    const GLuint userClipControlMask,
    const GLuint vertexOutputAttributeMask)
{
    (void)userClipControlMask;
    (void)vertexOutputAttributeMask;
}

/* ── rescaler (inline in header, but needs symbol for ABI) ────────── */

#if !defined(CELL_SDK_VERSION) || (CELL_SDK_VERSION < 0x180000)
PSGL_EXPORT void psglRescAdjustAspectRatio(
    const float horizontalScale, const float verticalScale)
{
    (void)horizontalScale;
    (void)verticalScale;
}
#endif

/* ── command buffer recording ────────────────────────────────────── */

PSGL_EXPORT void psglBeginCommandRecord(void *commandBuffer,
                                        GLuint sizeInBytes)
{
    (void)commandBuffer;
    (void)sizeInBytes;
}

PSGL_EXPORT void *psglGetCommandRecordCurrent(void)
{
    return NULL;
}

PSGL_EXPORT void *psglEndCommandRecord(bool addReturn)
{
    (void)addReturn;
    return NULL;
}

PSGL_EXPORT void *psglCallCommandBuffer(void *commandBuffer)
{
    (void)commandBuffer;
    return NULL;
}

PSGL_EXPORT void psglPushCommandBuffer(void *commandBuffer,
                                       GLuint sizeInBytes)
{
    (void)commandBuffer;
    (void)sizeInBytes;
}

PSGL_EXPORT void *psglAllocateCommandBuffer(GLuint sizeInBytes,
                                            GLuint *offset)
{
    (void)sizeInBytes;
    if (offset) *offset = 0;
    return NULL;
}

PSGL_EXPORT void psglFreeCommandBuffer(void *commandBuffer)
{
    (void)commandBuffer;
}

PSGL_EXPORT void psglSetRecordOutOfSpaceCallback(
    psglStaticCommandBufferCallBack callback)
{
    (void)callback;
}

PSGL_EXPORT void psglSetStallCallback(psglStallCallBack callBack)
{
    (void)callBack;
}

/* ── command buffer holes ────────────────────────────────────────── */

PSGL_EXPORT int psglDrawCommandBufferHole(
    GLenum mode, GLsizei count, GLenum type,
    const GLvoid *indices, uint32_t *indexOffset,
    uint32_t *holeEA, uint32_t *holeSizeInWord)
{
    (void)mode;
    (void)count;
    (void)type;
    (void)indices;
    if (indexOffset)    *indexOffset    = 0;
    if (holeEA)         *holeEA         = 0;
    if (holeSizeInWord) *holeSizeInWord = 0;
    return -1;
}

PSGL_EXPORT int psglGenerateCommandBufferHole(uint32_t holeSizeInWord,
                                              uint32_t *holeEA)
{
    (void)holeSizeInWord;
    if (holeEA) *holeEA = 0;
    return -1;
}

/* ── state validation ────────────────────────────────────────────── */

PSGL_EXPORT GLuint psglValidateStates(GLuint mask)
{
    (void)mask;
    return 0;
}

PSGL_EXPORT void psglInvalidateStates(GLuint mask)
{
    (void)mask;
}

PSGL_EXPORT GLuint psglValidateAttributes(const GLvoid *indices,
                                          GLboolean *isMain)
{
    (void)indices;
    if (isMain) *isMain = GL_FALSE;
    return 0;
}

PSGL_EXPORT void psglInvalidateAttributes(void) {}

/* ── hardware cursor ─────────────────────────────────────────────── */

PSGL_EXPORT int32_t psglInitCursor(void)
{
    return PSGL_HW_CURSOR_ERROR_FAILURE;
}

PSGL_EXPORT int32_t psglSetCursorEnable(void)
{
    return PSGL_HW_CURSOR_ERROR_FAILURE;
}

PSGL_EXPORT int32_t psglSetCursorDisable(void)
{
    return PSGL_HW_CURSOR_ERROR_FAILURE;
}

PSGL_EXPORT int32_t psglSetCursorImageOffset(const uint32_t offset)
{
    (void)offset;
    return PSGL_HW_CURSOR_ERROR_FAILURE;
}

PSGL_EXPORT int32_t psglSetCursorPosition(const int32_t x, const int32_t y)
{
    (void)x;
    (void)y;
    return PSGL_HW_CURSOR_ERROR_FAILURE;
}

PSGL_EXPORT int32_t psglUpdateCursor(void)
{
    return PSGL_HW_CURSOR_ERROR_FAILURE;
}
