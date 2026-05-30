#ifndef PS3DK_PSGL_H
#define PS3DK_PSGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <PSGL/export.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <Cg/cg.h>
#include <Cg/cgGL.h>
#include <PSGL/report.h>

#include <cell/cgb/cgb_struct.h>
#include <cell/resc.h>
#include <sdk_version.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PSGLdevice PSGLdevice;
typedef struct PSGLcontext PSGLcontext;

typedef enum PSGLtvStandard {
    PSGL_TV_STANDARD_NONE = 0,
    PSGL_TV_STANDARD_NTSC_M,
    PSGL_TV_STANDARD_NTSC_J,
    PSGL_TV_STANDARD_PAL_M,
    PSGL_TV_STANDARD_PAL_B,
    PSGL_TV_STANDARD_PAL_D,
    PSGL_TV_STANDARD_PAL_G,
    PSGL_TV_STANDARD_PAL_H,
    PSGL_TV_STANDARD_PAL_I,
    PSGL_TV_STANDARD_PAL_N,
    PSGL_TV_STANDARD_PAL_NC,
    PSGL_TV_STANDARD_HD480I,
    PSGL_TV_STANDARD_HD480P,
    PSGL_TV_STANDARD_HD576I,
    PSGL_TV_STANDARD_HD576P,
    PSGL_TV_STANDARD_HD720P,
    PSGL_TV_STANDARD_HD1080I,
    PSGL_TV_STANDARD_HD1080P,
    PSGL_TV_STANDARD_1280x720_ON_VESA_1280x768 = 128,
    PSGL_TV_STANDARD_1280x720_ON_VESA_1280x1024,
    PSGL_TV_STANDARD_1920x1080_ON_VESA_1920x1200
} PSGLtvStandard;

typedef enum PSGLbufferingMode {
    PSGL_BUFFERING_MODE_SINGLE = 1,
    PSGL_BUFFERING_MODE_DOUBLE = 2,
    PSGL_BUFFERING_MODE_TRIPLE = 3
} PSGLbufferingMode;

typedef enum PSGLdeviceConnector {
    PSGL_DEVICE_CONNECTOR_NONE = 0,
    PSGL_DEVICE_CONNECTOR_VGA,
    PSGL_DEVICE_CONNECTOR_DVI,
    PSGL_DEVICE_CONNECTOR_HDMI,
    PSGL_DEVICE_CONNECTOR_COMPOSITE,
    PSGL_DEVICE_CONNECTOR_SVIDEO,
    PSGL_DEVICE_CONNECTOR_COMPONENT
} PSGLdeviceConnector;

typedef enum RescRatioMode {
    RESC_RATIO_MODE_FULLSCREEN = 0,
    RESC_RATIO_MODE_LETTERBOX,
    RESC_RATIO_MODE_PANSCAN
} RescRatioMode;

typedef enum RescPalTemporalMode {
    RESC_PAL_TEMPORAL_MODE_50_NONE = 0,
    RESC_PAL_TEMPORAL_MODE_60_DROP,
    RESC_PAL_TEMPORAL_MODE_60_INTERPOLATE,
    RESC_PAL_TEMPORAL_MODE_60_INTERPOLATE_30_DROP,
    RESC_PAL_TEMPORAL_MODE_60_INTERPOLATE_DROP_FLEXIBLE
} RescPalTemporalMode;

typedef enum RescInterlaceMode {
    RESC_INTERLACE_MODE_NORMAL_BILINEAR = 0,
    RESC_INTERLACE_MODE_INTERLACE_FILTER
} RescInterlaceMode;

#define PSGL_DEVICE_PARAMETERS_COLOR_FORMAT             0x0001u
#define PSGL_DEVICE_PARAMETERS_DEPTH_FORMAT             0x0002u
#define PSGL_DEVICE_PARAMETERS_MULTISAMPLING_MODE       0x0004u
#define PSGL_DEVICE_PARAMETERS_TV_STANDARD              0x0008u
#define PSGL_DEVICE_PARAMETERS_CONNECTOR                0x0010u
#define PSGL_DEVICE_PARAMETERS_BUFFERING_MODE           0x0020u
#define PSGL_DEVICE_PARAMETERS_WIDTH_HEIGHT             0x0040u
#define PSGL_DEVICE_PARAMETERS_RESC_RENDER_WIDTH_HEIGHT 0x0080u
#define PSGL_DEVICE_PARAMETERS_RESC_RATIO_MODE          0x0100u
#define PSGL_DEVICE_PARAMETERS_RESC_PAL_TEMPORAL_MODE   0x0200u
#define PSGL_DEVICE_PARAMETERS_RESC_INTERLACE_MODE      0x0400u
#define PSGL_DEVICE_PARAMETERS_RESC_ADJUST_ASPECT_RATIO 0x0800u

#define PSGL_VALIDATE_NONE                      0x00000000u
#define PSGL_VALIDATE_FRAMEBUFFER               0x00000001u
#define PSGL_VALIDATE_TEXTURES_USED             0x00000002u
#define PSGL_VALIDATE_VERTEX_PROGRAM            0x00000004u
#define PSGL_VALIDATE_VERTEX_CONSTANTS          0x00000008u
#define PSGL_VALIDATE_VERTEX_TEXTURES_USED      0x00000010u
#define PSGL_VALIDATE_FFX_VERTEX_PROGRAM        0x00000020u
#define PSGL_VALIDATE_FRAGMENT_PROGRAM          0x00000040u
#define PSGL_VALIDATE_FFX_FRAGMENT_PROGRAM      0x00000080u
#define PSGL_VALIDATE_FRAGMENT_SHARED_CONSTANTS 0x00000100u
#define PSGL_VALIDATE_VIEWPORT                  0x00000200u
#define PSGL_VALIDATE_ALPHA_TEST                0x00000400u
#define PSGL_VALIDATE_DEPTH_TEST                0x00000800u
#define PSGL_VALIDATE_WRITE_MASK                0x00001000u
#define PSGL_VALIDATE_STENCIL_TEST              0x00002000u
#define PSGL_VALIDATE_STENCIL_OP_AND_MASK       0x00004000u
#define PSGL_VALIDATE_SCISSOR_BOX               0x00008000u
#define PSGL_VALIDATE_FACE_CULL                 0x00010000u
#define PSGL_VALIDATE_BLENDING                  0x00020000u
#define PSGL_VALIDATE_POINT_RASTER              0x00040000u
#define PSGL_VALIDATE_LINE_RASTER               0x00080000u
#define PSGL_VALIDATE_POLYGON_OFFSET            0x00100000u
#define PSGL_VALIDATE_SHADE_MODEL               0x00200000u
#define PSGL_VALIDATE_LOGIC_OP                  0x00400000u
#define PSGL_VALIDATE_MULTISAMPLING             0x00800000u
#define PSGL_VALIDATE_POLYGON_MODE              0x01000000u
#define PSGL_VALIDATE_PRIMITIVE_RESTART         0x02000000u
#define PSGL_VALIDATE_CLIP_PLANES               0x04000000u
#define PSGL_VALIDATE_SHADER_SRGB_REMAP         0x08000000u
#define PSGL_VALIDATE_POINT_SPRITE              0x10000000u
#define PSGL_VALIDATE_TWO_SIDE_COLOR            0x20000000u
#define PSGL_VALIDATE_ALL                       0x3fffffffu

typedef struct PSGLdeviceParameters {
    GLuint enable;
    GLenum colorFormat;
    GLenum depthFormat;
    GLenum multisamplingMode;
    PSGLtvStandard TVStandard;
    PSGLdeviceConnector connector;
    PSGLbufferingMode bufferingMode;
    GLuint width;
    GLuint height;
    GLuint renderWidth;
    GLuint renderHeight;
    RescRatioMode rescRatioMode;
    RescPalTemporalMode rescPalTemporalMode;
    RescInterlaceMode rescInterlaceMode;
    GLfloat horizontalScale;
    GLfloat verticalScale;
} PSGLdeviceParameters;

#define PSGL_INIT_MAX_SPUS               0x0001u
#define PSGL_INIT_INITIALIZE_SPUS        0x0002u
#define PSGL_INIT_PERSISTENT_MEMORY_SIZE 0x0004u
#define PSGL_INIT_TRANSIENT_MEMORY_SIZE  0x0008u
#define PSGL_INIT_ERROR_CONSOLE          0x0010u
#define PSGL_INIT_FIFO_SIZE              0x0020u
#define PSGL_INIT_HOST_MEMORY_SIZE       0x0040u
#define PSGL_INIT_USE_PMQUERIES          0x0080u

typedef struct PSGLinitOptions {
    GLuint enable;
    GLuint maxSPUs;
    GLboolean initializeSPUs;
    GLuint persistentMemorySize;
    GLuint transientMemorySize;
    int errorConsole;
    GLuint fifoSize;
    GLuint hostMemorySize;
} PSGLinitOptions;

typedef unsigned long long PSGLuint64;
typedef void *(*PSGLmallocFunc)(size_t size);
typedef void *(*PSGLmemalignFunc)(size_t alignment, size_t size);
typedef void *(*PSGLreallocFunc)(void *block, size_t size);
typedef void (*PSGLfreeFunc)(void *block);

PSGL_EXPORT void psglInit(PSGLinitOptions *options);
PSGL_EXPORT void psglExit(void);
PSGL_EXPORT PSGLdevice *psglCreateDeviceAuto(GLenum colorFormat, GLenum depthFormat, GLenum multisamplingMode);
PSGL_EXPORT PSGLdevice *psglCreateDeviceExtended(const PSGLdeviceParameters *parameters);
PSGL_EXPORT GLfloat psglGetDeviceAspectRatio(const PSGLdevice *device);
PSGL_EXPORT void psglGetDeviceDimensions(const PSGLdevice *device, GLuint *width, GLuint *height);
PSGL_EXPORT void psglGetRenderBufferDimensions(const PSGLdevice *device, GLuint *width, GLuint *height);
PSGL_EXPORT void psglDestroyDevice(PSGLdevice *device);
PSGL_EXPORT void psglMakeCurrent(PSGLcontext *context, PSGLdevice *device);
PSGL_EXPORT PSGLcontext *psglCreateContext(void);
PSGL_EXPORT void psglDestroyContext(PSGLcontext *context);
PSGL_EXPORT void psglResetCurrentContext(void);
PSGL_EXPORT PSGLcontext *psglGetCurrentContext(void);
PSGL_EXPORT PSGLdevice *psglGetCurrentDevice(void);
PSGL_EXPORT void psglSwap(void);
PSGL_EXPORT void psglLoadShaderLibrary(const char *filename);
PSGL_EXPORT PSGLuint64 psglGetSystemTime(void);
PSGL_EXPORT PSGLuint64 psglGetLastFlipTime(void);
PSGL_EXPORT void *psglGetSPUInitData(void);
PSGL_EXPORT void psglSetFlipHandler(void (*handler)(const GLuint head));
PSGL_EXPORT void psglSetVBlankHandler(void (*handler)(const GLuint head));
PSGL_EXPORT GLboolean psglSetAllocatorFuncs(PSGLmallocFunc mallocFunc, PSGLmemalignFunc memalignFunc, PSGLreallocFunc reallocFunc, PSGLfreeFunc freeFunc);
PSGL_EXPORT void psglSetBounceBufferSize(GLsizei size);
PSGL_EXPORT GLsizei psglGetBounceBufferSize(void);
PSGL_EXPORT void psglAddressToOffset(const void *address, GLuint *offset);
PSGL_EXPORT void psglSetVertexProgramRegister(GLuint reg, const void *__restrict value);
PSGL_EXPORT void psglSetVertexProgramRegisterBlock(GLuint reg, GLuint count, const void *__restrict value);
PSGL_EXPORT void psglSetVertexProgramTransformBranchBits(GLuint values);
PSGL_EXPORT void psglSetVertexProgramConfiguration(const CellCgbVertexProgramConfiguration *conf, const void *ucodeStorageAddress);
PSGL_EXPORT void psglSetFragmentProgramEmbeddedConstant(const GLuint offset, const GLfloat *value, const GLuint sizeInWords);
PSGL_EXPORT void psglSetFragmentProgramConfiguration(const CellCgbFragmentProgramConfiguration *conf);
PSGL_EXPORT void psglSetFragmentProgramEmbeddedConstantMemoryLocation(const GLuint offset, const GLfloat *value, const GLuint sizeInWords, bool inLocalMemory);
PSGL_EXPORT void psglSetFragmentProgramConfigurationMemoryLocation(const CellCgbFragmentProgramConfiguration *conf, bool inLocalMemory);
PSGL_EXPORT void psglSetUserClipPlanes(const GLuint userClipControlMask, const GLuint vertexOutputAttributeMask);

#if defined(CELL_SDK_VERSION) && (CELL_SDK_VERSION >= 0x180000)
static inline PSGL_EXPORT void psglRescAdjustAspectRatio(const float horizontalScale, const float verticalScale)
{
    cellRescAdjustAspectRatio(horizontalScale, verticalScale);
}
#endif

PSGL_EXPORT void psglBeginCommandRecord(void *commandBuffer, GLuint sizeInBytes);
PSGL_EXPORT void *psglGetCommandRecordCurrent(void);
PSGL_EXPORT void *psglEndCommandRecord(bool addReturn);
PSGL_EXPORT void *psglCallCommandBuffer(void *commandBuffer);
PSGL_EXPORT void psglPushCommandBuffer(void *commandBuffer, GLuint sizeInBytes);
PSGL_EXPORT void *psglAllocateCommandBuffer(GLuint sizeInBytes, GLuint *offset);
PSGL_EXPORT void psglFreeCommandBuffer(void *commandBuffer);

typedef int32_t (*psglStaticCommandBufferCallBack)(CellGcmContextData *context, uint32_t size);
PSGL_EXPORT void psglSetRecordOutOfSpaceCallback(psglStaticCommandBufferCallBack callback);
typedef bool (*psglStallCallBack)(void *get, void *nextBegin, void *nextEnd);
PSGL_EXPORT void psglSetStallCallback(psglStallCallBack callBack);

#define PSGL_ADD_RETURN        true
#define PSGL_DO_NOT_ADD_RETURN false
#define PSGL_STALL             true
#define PSGL_DO_NOT_STALL      false

PSGL_EXPORT int psglDrawCommandBufferHole(GLenum mode, GLsizei count, GLenum type,
                                          const GLvoid *indices, uint32_t *indexOffset,
                                          uint32_t *holeEA, uint32_t *holeSizeInWord);
PSGL_EXPORT int psglGenerateCommandBufferHole(uint32_t holeSizeInWord, uint32_t *holeEA);
PSGL_EXPORT GLuint psglValidateStates(GLuint mask);
PSGL_EXPORT void psglInvalidateStates(GLuint mask);
PSGL_EXPORT GLuint psglValidateAttributes(const GLvoid *indices, GLboolean *isMain);
PSGL_EXPORT void psglInvalidateAttributes(void);

#define PSGL_HW_CURSOR_OK                  CELL_OK
#define PSGL_HW_CURSOR_ERROR_FAILURE       CELL_GCM_ERROR_FAILURE
#define PSGL_HW_CURSOR_ERROR_INVALID_VALUE CELL_GCM_ERROR_INVALID_VALUE

PSGL_EXPORT int32_t psglInitCursor(void);
PSGL_EXPORT int32_t psglSetCursorEnable(void);
PSGL_EXPORT int32_t psglSetCursorDisable(void);
PSGL_EXPORT int32_t psglSetCursorImageOffset(const uint32_t offset);
PSGL_EXPORT int32_t psglSetCursorPosition(const int32_t x, const int32_t y);
PSGL_EXPORT int32_t psglUpdateCursor(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_PSGL_H */
