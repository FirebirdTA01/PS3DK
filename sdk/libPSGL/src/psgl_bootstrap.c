/* PSGL device, context, swap, and low-level command entry points.
 *
 * Unimplemented public psgl* entry points return conservative defaults:
 *   pointer returns NULL
 *   GLboolean / int32_t return GL_FALSE / CELL_OK
 *   GLuint returns 0
 *   void functions are no-ops
 */
#include <PSGL/psgl.h>
#include <PSGL/psglu.h>
#include <PSGL/report.h>

#include "psgl_context.h"

static void psgl_bootstrap_zero(void *ptr, uint32_t size)
{
    unsigned char *out = (unsigned char *)ptr;
    while (size--) {
        *out++ = 0;
    }
}

static uint32_t psgl_bootstrap_halfword_swap(uint32_t value)
{
    return ((value >> 16) & 0xffffu) | ((value & 0xffffu) << 16);
}

static uint32_t psgl_bootstrap_fp_control(
    const CellCgbFragmentProgramConfiguration *conf)
{
    uint32_t output_from_h0 = (conf->fragmentControl >> 16) & 1u;
    uint32_t pixel_kill = (conf->fragmentControl >> 18) & 1u;
    uint32_t reg_count = conf->registerCount > 2u ? conf->registerCount : 2u;
    uint32_t low = output_from_h0 ? 0x0eu : 0x40u;
    if (pixel_kill) low |= (1u << 7);
    return low | (1u << 10) | (reg_count << 24);
}

/* lifecycle */

PSGL_EXPORT void psglInit(PSGLinitOptions *options)
{
    if (!psgl_context_init_system(options)) return;
    if (!psgl_context_current()) {
        PSGLcontext *context = psgl_context_create();
        if (context) psgl_context_make_current(context, NULL);
    }
}

PSGL_EXPORT void psglExit(void)
{
    PSGLcontext *context = psgl_context_current();
    PSGLdevice *device = psgl_context_current_device();
    if (context) psgl_context_destroy(context);
    if (device) psgl_device_destroy(device);
    psgl_context_shutdown_system();
}

PSGL_EXPORT PSGLdevice *psglCreateDeviceAuto(GLenum colorFormat,
                                             GLenum depthFormat,
                                             GLenum multisamplingMode)
{
    PSGLdeviceParameters parameters;
    psgl_bootstrap_zero(&parameters, sizeof(parameters));
    parameters.enable = PSGL_DEVICE_PARAMETERS_COLOR_FORMAT |
                        PSGL_DEVICE_PARAMETERS_DEPTH_FORMAT |
                        PSGL_DEVICE_PARAMETERS_MULTISAMPLING_MODE |
                        PSGL_DEVICE_PARAMETERS_BUFFERING_MODE;
    parameters.colorFormat = colorFormat;
    parameters.depthFormat = depthFormat;
    parameters.multisamplingMode = multisamplingMode;
    parameters.bufferingMode = PSGL_BUFFERING_MODE_TRIPLE;
    return psgl_device_create(&parameters);
}

PSGL_EXPORT PSGLdevice *psglCreateDeviceExtended(
    const PSGLdeviceParameters *parameters)
{
    return psgl_device_create(parameters);
}

PSGL_EXPORT GLfloat psglGetDeviceAspectRatio(const PSGLdevice *device)
{
    return device ? device->aspect_ratio : 1.0f;
}

PSGL_EXPORT void psglGetDeviceDimensions(const PSGLdevice *device,
                                         GLuint *width, GLuint *height)
{
    if (width)  *width  = device ? device->width : 0;
    if (height) *height = device ? device->height : 0;
}

PSGL_EXPORT void psglGetRenderBufferDimensions(const PSGLdevice *device,
                                               GLuint *width, GLuint *height)
{
    if (width)  *width  = device ? device->render_width : 0;
    if (height) *height = device ? device->render_height : 0;
}

PSGL_EXPORT void psglDestroyDevice(PSGLdevice *device)
{
    psgl_device_destroy(device);
}

/* context */

PSGL_EXPORT void psglMakeCurrent(PSGLcontext *context, PSGLdevice *device)
{
    psgl_context_make_current(context, device);
}

PSGL_EXPORT PSGLcontext *psglCreateContext(void)
{
    if (!psgl_context_init_system(NULL)) return NULL;
    return psgl_context_create();
}

PSGL_EXPORT void psglDestroyContext(PSGLcontext *context)
{
    psgl_context_destroy(context);
}

PSGL_EXPORT void psglResetCurrentContext(void)
{
    psgl_context_reset_current();
}

PSGL_EXPORT PSGLcontext *psglGetCurrentContext(void)
{
    return psgl_context_current();
}

PSGL_EXPORT PSGLdevice *psglGetCurrentDevice(void)
{
    return psgl_context_current_device();
}

/* frame control */

PSGL_EXPORT void psglSwap(void)
{
    psgl_context_swap();
}

/* shader library */

PSGL_EXPORT void psglLoadShaderLibrary(const char *filename)
{
    psgl_context_load_shader_library(filename);
}

PSGL_EXPORT void *psglGetSPUInitData(void)
{
    static uint64_t config[34] __attribute__((aligned(16)));
    CellGcmConfig gcm_config;
    uintptr_t local_base;
    uint32_t blocks;

    cellGcmGetConfiguration(&gcm_config);
    if (!gcm_config.localAddress || !gcm_config.localSize) return NULL;

    local_base = (uintptr_t)gcm_config.localAddress;
    blocks = (gcm_config.localSize + 0x03ffffffu) >> 26;
    if (blocks > 8u) blocks = 8u;

    for (uint32_t i = 0u; i < 34u; i++)
        config[i] = 0u;
    for (uint32_t block = 0u; block < blocks; block++) {
        uint64_t block_base = (uint64_t)(local_base + (block << 26));
        for (uint32_t window = 0u; window < 4u; window++)
            config[block * 4u + window] = block_base;
    }
    config[32] = (uint64_t)local_base;
    return config;
}

/* allocators */

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

/* low-level RSX */

PSGL_EXPORT void psglAddressToOffset(const void *address, GLuint *offset)
{
    if (!offset) return;
    *offset = 0;
    if (address) {
        uint32_t gcm_offset = 0;
        if (cellGcmAddressToOffset(address, &gcm_offset) == 0)
            *offset = (GLuint)gcm_offset;
    }
}

PSGL_EXPORT void psglSetVertexProgramRegister(
    GLuint reg, const void *__restrict value)
{
    PSGLcontext *context = psgl_context_current();
    if (!context || !context->gcm || !value) return;
    cellGcmSetVertexProgramParameterBlock(context->gcm, reg, 1u,
                                          (const GLfloat *)value);
}

PSGL_EXPORT void psglSetVertexProgramRegisterBlock(
    GLuint reg, GLuint count, const void *__restrict value)
{
    PSGLcontext *context = psgl_context_current();
    if (!context || !context->gcm || !value || count == 0u) return;
    cellGcmSetVertexProgramParameterBlock(context->gcm, reg, count,
                                          (const GLfloat *)value);
}

PSGL_EXPORT void psglSetVertexProgramTransformBranchBits(GLuint values)
{
    PSGLcontext *context = psgl_context_current();
    uint32_t *w;
    if (!context || !context->gcm) return;
    w = ps3tc_gcm_reserve(context->gcm, 2u);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_TRANSFORM_BRANCH_BITS, 1);
    *w++ = values;
}

PSGL_EXPORT void psglSetVertexProgramConfiguration(
    const CellCgbVertexProgramConfiguration *conf,
    const void *ucodeStorageAddress)
{
    PSGLcontext *context = psgl_context_current();
    const uint32_t *src = (const uint32_t *)ucodeStorageAddress;
    uint32_t inst_count;
    uint32_t reserve;
    uint32_t *w;
    if (!context || !context->gcm || !conf || !src) return;
    inst_count = conf->instructionCount;
    if (inst_count == 0u || inst_count > 512u) return;
    reserve = 3u + inst_count * 5u + 2u + 2u;
    w = ps3tc_gcm_reserve(context->gcm, reserve);
    if (!w) return;

    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_FROM_ID, 2);
    *w++ = conf->instructionSlot;
    *w++ = conf->instructionSlot;
    for (uint32_t i = 0u; i < inst_count; i++) {
        *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_INST(0), 4);
        *w++ = src[0];
        *w++ = src[1];
        *w++ = src[2];
        *w++ = src[3];
        src += 4;
    }
    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_ATTRIB_EN, 1);
    *w++ = conf->attributeInputMask;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_TRANSFORM_TIMEOUT, 1);
    *w++ = (conf->registerCount <= 32u) ? 0x0020ffffu : 0x0030ffffu;
}

PSGL_EXPORT void psglSetFragmentProgramEmbeddedConstant(
    const GLuint offset, const GLfloat *value, const GLuint sizeInWords)
{
    psglSetFragmentProgramEmbeddedConstantMemoryLocation(offset, value,
                                                         sizeInWords,
                                                         true);
}

PSGL_EXPORT void psglSetFragmentProgramConfiguration(
    const CellCgbFragmentProgramConfiguration *conf)
{
    psglSetFragmentProgramConfigurationMemoryLocation(conf, true);
}

PSGL_EXPORT void psglSetFragmentProgramEmbeddedConstantMemoryLocation(
    const GLuint offset, const GLfloat *value, const GLuint sizeInWords,
    bool inLocalMemory)
{
    void *address = NULL;
    uint32_t *dst;
    const uint32_t *src = (const uint32_t *)value;
    if (!value || sizeInWords == 0u) return;
    if (inLocalMemory) {
        CellGcmConfig config;
        cellGcmGetConfiguration(&config);
        address = (unsigned char *)config.localAddress + offset;
    } else {
        address = (void *)(uintptr_t)offset;
    }
    if (!address) return;
    dst = (uint32_t *)address;
    for (GLuint i = 0u; i < sizeInWords; i++)
        dst[i] = psgl_bootstrap_halfword_swap(src[i]);
}

PSGL_EXPORT void psglSetFragmentProgramConfigurationMemoryLocation(
    const CellCgbFragmentProgramConfiguration *conf, bool inLocalMemory)
{
    PSGLcontext *context = psgl_context_current();
    uint32_t location;
    uint32_t texcoords;
    uint32_t texcoords_2d;
    uint32_t fp_control;
    uint32_t *w;
    if (!context || !context->gcm || !conf) return;
    location = inLocalMemory ? GCM_LOCATION_RSX : GCM_LOCATION_CELL;

    w = ps3tc_gcm_reserve(context->gcm, 2u);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_FP_ADDRESS, 1);
    *w++ = (location + 1u) | (conf->offset & 0x1fffffffu);

    texcoords = conf->texCoordsInputMask;
    texcoords_2d = conf->texCoords2D;
    for (uint32_t i = 0u; texcoords; i++) {
        if (texcoords & 1u) {
            w = ps3tc_gcm_reserve(context->gcm, 2u);
            if (!w) return;
            *w++ = PS3TC_GCM_METHOD(NV40TCL_TEX_COORD_CONTROL(i), 1);
            *w++ = texcoords_2d & 1u;
        }
        texcoords >>= 1;
        texcoords_2d >>= 1;
    }

    fp_control = psgl_bootstrap_fp_control(conf);
    w = ps3tc_gcm_reserve(context->gcm, 2u);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_FP_CONTROL, 1);
    *w++ = fp_control;
}

/* user clip planes */

PSGL_EXPORT void psglSetUserClipPlanes(
    const GLuint userClipControlMask,
    const GLuint vertexOutputAttributeMask)
{
    (void)userClipControlMask;
    (void)vertexOutputAttributeMask;
}

/* rescaler (inline in header, but needs symbol for ABI) */

#if !defined(CELL_SDK_VERSION) || (CELL_SDK_VERSION < 0x180000)
PSGL_EXPORT void psglRescAdjustAspectRatio(
    const float horizontalScale, const float verticalScale)
{
    (void)horizontalScale;
    (void)verticalScale;
}
#endif

/* state validation */

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

/* hardware cursor */

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
