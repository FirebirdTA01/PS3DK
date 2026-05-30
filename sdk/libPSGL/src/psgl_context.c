#include "psgl_context.h"

#include <malloc.h>
#include <rsx/rsx.h>
#include <stdlib.h>
#include <sys/lv2_syscall.h>
#include <sys/timer.h>
#include <sysutil/video.h>

#define PSGL_DEFAULT_FIFO_SIZE (1024u * 1024u)
#define PSGL_DEFAULT_HOST_SIZE (32u * 1024u * 1024u)
#define PSGL_HOST_ALIGNMENT    (1024u * 1024u)
#define PSGL_LABEL_INDEX       255u
#define PSGL_BUFFER_ALIGNMENT  64u

typedef struct PSGLbufferObject {
    GLuint name;
    GLenum usage;
    uint32_t size;
    void *address;
    uint32_t offset;
    GLboolean mapped;
    struct PSGLbufferObject *next;
} PSGLbufferObject;

typedef struct PSGLsystemState {
    uint32_t initialized;
    void *host_memory;
    uint32_t fifo_size;
    uint32_t host_size;
    PSGLuint64 last_flip_time;
    PSGLcontext *current_context;
    PSGLdevice *current_device;
    void (*flip_handler)(const GLuint head);
    void (*vblank_handler)(const GLuint head);
} PSGLsystemState;

static PSGLsystemState g_psgl;
static PSGLbufferObject *g_psgl_buffers;
static GLuint g_psgl_next_buffer_name = 1u;

static void psgl_zero(void *ptr, uint32_t size)
{
    unsigned char *out = (unsigned char *)ptr;
    while (size--) {
        *out++ = 0;
    }
}

static void psgl_copy(void *dst, const void *src, uint32_t size)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    while (size--) {
        *out++ = *in++;
    }
}

static void psgl_flip_trampoline(uint32_t head)
{
    if (g_psgl.flip_handler) g_psgl.flip_handler((GLuint)head);
}

static void psgl_vblank_trampoline(uint32_t head)
{
    if (g_psgl.vblank_handler) g_psgl.vblank_handler((GLuint)head);
}

static int32_t psgl_get_current_time(uint64_t *sec, uint64_t *nsec)
{
    lv2syscall2(145, (uint64_t)(uintptr_t)sec, (uint64_t)(uintptr_t)nsec);
    return_to_user_prog(int32_t);
}

static void psgl_matrix_identity(GLfloat matrix[16])
{
    psgl_zero(matrix, sizeof(GLfloat) * 16u);
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

static float psgl_clampf(GLfloat value, GLfloat lo, GLfloat hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static uint8_t psgl_float_to_unorm8(GLfloat value)
{
    float clamped = psgl_clampf(value, 0.0f, 1.0f);
    return (uint8_t)(clamped * 255.0f + 0.5f);
}

static uint32_t psgl_pack_clear_color(const GLfloat color[4])
{
    uint32_t r = psgl_float_to_unorm8(color[0]);
    uint32_t g = psgl_float_to_unorm8(color[1]);
    uint32_t b = psgl_float_to_unorm8(color[2]);
    uint32_t a = psgl_float_to_unorm8(color[3]);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t psgl_pack_depth_stencil(GLfloat depth, GLint stencil)
{
    uint32_t z = (uint32_t)(psgl_clampf(depth, 0.0f, 1.0f) * 16777215.0f + 0.5f);
    return (z << 8) | ((uint32_t)stencil & 0xffu);
}

static void psgl_fill_surface(PSGLdevice *device, CellGcmSurface *surface)
{
    PSGLframeBuffer *frame = &device->frames[device->current_frame];
    psgl_zero(surface, sizeof(*surface));
    surface->colorFormat = GCM_SURFACE_X8R8G8B8;
    surface->colorTarget = GCM_SURFACE_TARGET_0;
    surface->colorLocation[0] = GCM_LOCATION_RSX;
    surface->colorOffset[0] = frame->offset;
    surface->colorPitch[0] = frame->pitch;
    surface->colorLocation[1] = GCM_LOCATION_RSX;
    surface->colorLocation[2] = GCM_LOCATION_RSX;
    surface->colorLocation[3] = GCM_LOCATION_RSX;
    surface->colorPitch[1] = 64u;
    surface->colorPitch[2] = 64u;
    surface->colorPitch[3] = 64u;
    surface->depthFormat = GCM_SURFACE_ZETA_Z16;
    surface->depthLocation = GCM_LOCATION_RSX;
    surface->depthOffset = device->depth_offset;
    surface->depthPitch = device->depth_pitch;
    surface->type = GCM_TEXTURE_LINEAR;
    surface->antiAlias = GCM_SURFACE_CENTER_1;
    surface->width = frame->width;
    surface->height = frame->height;
}

static void psgl_bind_render_target(PSGLcontext *context)
{
    if (!context || !context->device || !context->gcm) return;
    CellGcmSurface surface;
    psgl_fill_surface(context->device, &surface);
    cellGcmSetSurface(context->gcm, &surface);
    context->dirty &= ~PSGL_DIRTY_FRAMEBUFFER;
}

static void psgl_emit_viewport(PSGLcontext *context)
{
    if (!context || !context->gcm) return;
    float scale[4];
    float offset[4];
    GLint *vp = context->viewport;

    scale[0] = (float)vp[2] * 0.5f;
    scale[1] = (float)vp[3] * -0.5f;
    scale[2] = 0.5f;
    scale[3] = 0.0f;
    offset[0] = (float)vp[0] + (float)vp[2] * 0.5f;
    offset[1] = (float)vp[1] + (float)vp[3] * 0.5f;
    offset[2] = 0.5f;
    offset[3] = 0.0f;

    cellGcmSetViewport(context->gcm, (uint16_t)vp[0], (uint16_t)vp[1],
                       (uint16_t)vp[2], (uint16_t)vp[3],
                       0.0f, 1.0f, scale, offset);
    context->dirty &= ~PSGL_DIRTY_VIEWPORT;
}

static void psgl_wait_rsx_idle(CellGcmContextData *gcm)
{
    uint32_t label = 1u;
    cellGcmSetWriteBackEndLabel(gcm, PSGL_LABEL_INDEX, label);
    cellGcmSetWaitLabel(gcm, PSGL_LABEL_INDEX, label);
    label++;
    cellGcmSetWriteBackEndLabel(gcm, PSGL_LABEL_INDEX, label);
    cellGcmFlush(gcm);
    while (*(volatile uint32_t *)cellGcmGetLabelAddress(PSGL_LABEL_INDEX) != label)
        sys_timer_usleep(30);
}

static void psgl_wait_flip(void)
{
    while (cellGcmGetFlipStatus() != 0u)
        sys_timer_usleep(200);
    cellGcmResetFlipStatus();
}

static int psgl_make_frame_buffer(PSGLdevice *device, uint32_t id)
{
    PSGLframeBuffer *frame = &device->frames[id];
    uint32_t size = device->pitch * (uint32_t)device->height;
    frame->address = (uint32_t *)rsxMemalign(64u, size);
    if (!frame->address) return 0;
    if (cellGcmAddressToOffset(frame->address, &frame->offset) != 0) return 0;
    if (cellGcmSetDisplayBuffer((uint8_t)id, frame->offset, frame->pitch,
                                frame->width, frame->height) != 0) {
        return 0;
    }
    return 1;
}

static PSGLbufferObject *psgl_find_buffer(GLuint name)
{
    PSGLbufferObject *buffer = g_psgl_buffers;
    while (buffer) {
        if (buffer->name == name) return buffer;
        buffer = buffer->next;
    }
    return NULL;
}

static PSGLbufferObject *psgl_create_buffer(GLuint name)
{
    PSGLbufferObject *buffer = psgl_find_buffer(name);
    if (buffer) return buffer;
    for (buffer = g_psgl_buffers; buffer; buffer = buffer->next) {
        if (buffer->name == 0u) {
            PSGLbufferObject *next = buffer->next;
            psgl_zero(buffer, sizeof(*buffer));
            buffer->name = name;
            buffer->next = next;
            return buffer;
        }
    }
    buffer = (PSGLbufferObject *)calloc(1u, sizeof(*buffer));
    if (!buffer) return NULL;
    buffer->name = name;
    buffer->next = g_psgl_buffers;
    g_psgl_buffers = buffer;
    return buffer;
}

static void psgl_release_buffer_storage(PSGLbufferObject *buffer)
{
    if (!buffer) return;
    buffer->address = NULL;
    buffer->size = 0u;
    buffer->offset = 0u;
    buffer->mapped = GL_FALSE;
}

static PSGLbufferObject *psgl_bound_buffer(PSGLcontext *context, GLenum target)
{
    if (!context) return NULL;
    if (target == GL_ARRAY_BUFFER)
        return psgl_find_buffer(context->bound_array_buffer);
    if (target == GL_ELEMENT_ARRAY_BUFFER)
        return psgl_find_buffer(context->bound_element_array_buffer);
    return NULL;
}

static void psgl_clear_deleted_buffer_bindings(GLuint name)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    if (context->bound_array_buffer == name) context->bound_array_buffer = 0u;
    if (context->bound_element_array_buffer == name) context->bound_element_array_buffer = 0u;
    for (uint32_t i = 0; i < PSGL_MAX_VERTEX_ATTRIBS; i++) {
        if (context->attribs[i].buffer_name == name) {
            context->attribs[i].buffer_name = 0u;
            context->attribs[i].buffer_offset = 0u;
        }
    }
}

int psgl_context_init_system(const PSGLinitOptions *options)
{
    if (g_psgl.initialized) return 1;

    uint32_t fifo_size = PSGL_DEFAULT_FIFO_SIZE;
    uint32_t host_size = PSGL_DEFAULT_HOST_SIZE;
    if (options) {
        if ((options->enable & PSGL_INIT_FIFO_SIZE) && options->fifoSize)
            fifo_size = options->fifoSize;
        if ((options->enable & PSGL_INIT_HOST_MEMORY_SIZE) && options->hostMemorySize)
            host_size = options->hostMemorySize;
    }

    void *host = memalign(PSGL_HOST_ALIGNMENT, host_size);
    if (!host) return 0;
    psgl_zero(host, host_size);

    if (cellGcmInit(fifo_size, host_size, host) != 0) {
        free(host);
        return 0;
    }

    g_psgl.host_memory = host;
    g_psgl.fifo_size = fifo_size;
    g_psgl.host_size = host_size;
    g_psgl.initialized = 1u;
    cellGcmSetFlipMode(GCM_FLIP_VSYNC);
    cellGcmResetFlipStatus();
    return 1;
}

void psgl_context_shutdown_system(void)
{
    if (!g_psgl.initialized) return;
    if (CELL_GCM_CURRENT && g_psgl.current_device &&
        g_psgl.current_device->submitted_flips) {
        cellGcmSetWaitFlip(CELL_GCM_CURRENT);
        cellGcmFinish(CELL_GCM_CURRENT, 1u);
    }
    g_psgl.current_context = NULL;
    g_psgl.current_device = NULL;
    g_psgl.host_memory = NULL;
    g_psgl.initialized = 0u;
}

PSGLcontext *psgl_context_create(void)
{
    PSGLcontext *context = (PSGLcontext *)calloc(1u, sizeof(*context));
    if (!context) return NULL;
    context->gcm = CELL_GCM_CURRENT;
    context->matrix_mode = GL_MODELVIEW;
    psgl_matrix_identity(context->modelview);
    psgl_matrix_identity(context->projection);
    psgl_matrix_identity(context->texture);
    context->clear_color[3] = 1.0f;
    context->clear_depth = 1.0f;
    context->attribs[PSGL_ATTRIB_VERTEX].size = 4;
    context->attribs[PSGL_ATTRIB_NORMAL].size = 3;
    context->attribs[PSGL_ATTRIB_COLOR].size = 4;
    context->attribs[PSGL_ATTRIB_TEXCOORD].size = 4;
    context->dirty = PSGL_DIRTY_ALL;
    return context;
}

void psgl_context_destroy(PSGLcontext *context)
{
    if (!context) return;
    if (g_psgl.current_context == context) psgl_context_reset_current();
}

PSGLdevice *psgl_device_create(const PSGLdeviceParameters *parameters)
{
    if (!psgl_context_init_system(NULL)) return NULL;

    videoState state;
    videoResolution resolution;
    videoConfiguration config;
    psgl_zero(&state, sizeof(state));
    psgl_zero(&resolution, sizeof(resolution));
    if (videoGetState(VIDEO_PRIMARY, 0, &state) != 0 ||
        state.state != VIDEO_STATE_DISABLED) {
        return NULL;
    }
    if (videoGetResolution(state.displayMode.resolution, &resolution) != 0)
        return NULL;

    PSGLdevice *device = (PSGLdevice *)calloc(1u, sizeof(*device));
    if (!device) return NULL;

    device->width = resolution.width;
    device->height = resolution.height;
    device->render_width = resolution.width;
    device->render_height = resolution.height;
    if (parameters) {
        if ((parameters->enable & PSGL_DEVICE_PARAMETERS_WIDTH_HEIGHT) &&
            parameters->width && parameters->height) {
            device->width = (uint16_t)parameters->width;
            device->height = (uint16_t)parameters->height;
        }
        if ((parameters->enable & PSGL_DEVICE_PARAMETERS_RESC_RENDER_WIDTH_HEIGHT) &&
            parameters->renderWidth && parameters->renderHeight) {
            device->render_width = (uint16_t)parameters->renderWidth;
            device->render_height = (uint16_t)parameters->renderHeight;
        }
        device->color_format = parameters->colorFormat;
        device->depth_format = parameters->depthFormat;
        device->multisampling_mode = parameters->multisamplingMode;
    }
    device->pitch = (uint32_t)device->width * sizeof(uint32_t);
    device->depth_pitch = device->pitch;
    device->frame_count = PSGL_MAX_FRAME_BUFFERS;
    device->aspect_ratio = device->height ? ((float)device->width / (float)device->height) : 1.0f;

    psgl_zero(&config, sizeof(config));
    config.resolution = state.displayMode.resolution;
    config.format = VIDEO_BUFFER_FORMAT_XRGB;
    config.pitch = device->pitch;
    config.aspect = state.displayMode.aspect;

    psgl_wait_rsx_idle(CELL_GCM_CURRENT);
    if (videoConfigure(VIDEO_PRIMARY, &config, NULL, 0) != 0) {
        free(device);
        return NULL;
    }
    cellGcmResetFlipStatus();

    for (uint32_t i = 0; i < device->frame_count; i++) {
        device->frames[i].pitch = device->pitch;
        device->frames[i].width = device->width;
        device->frames[i].height = device->height;
        device->frames[i].id = (uint8_t)i;
        if (!psgl_make_frame_buffer(device, i)) {
            psgl_device_destroy(device);
            return NULL;
        }
    }

    uint32_t depth_size = device->depth_pitch * (uint32_t)device->height * 2u;
    device->depth_address = (uint32_t *)rsxMemalign(64u, depth_size);
    if (!device->depth_address ||
        cellGcmAddressToOffset(device->depth_address, &device->depth_offset) != 0) {
        psgl_device_destroy(device);
        return NULL;
    }

    device->initialized = 1u;
    return device;
}

void psgl_device_destroy(PSGLdevice *device)
{
    if (!device) return;
    if (g_psgl.current_device == device) psgl_context_reset_current();
    if (CELL_GCM_CURRENT) {
        cellGcmFinish(CELL_GCM_CURRENT, 1u);
        for (uint32_t i = 0; i < PSGL_MAX_FRAME_BUFFERS; i++) {
            if (device->frames[i].address)
                cellGcmSetDisplayBuffer((uint8_t)i, 0u, 0u, 0u, 0u);
        }
        cellGcmFlush(CELL_GCM_CURRENT);
        cellGcmSetWaitFlip(CELL_GCM_CURRENT);
        cellGcmFinish(CELL_GCM_CURRENT, 1u);
        cellGcmResetFlipStatus();
    }
    for (uint32_t i = 0; i < PSGL_MAX_FRAME_BUFFERS; i++) {
        device->frames[i].address = NULL;
    }
    device->depth_address = NULL;
}

void psgl_context_make_current(PSGLcontext *context, PSGLdevice *device)
{
    if (!context) {
        psgl_context_reset_current();
        return;
    }
    context->device = device;
    context->gcm = CELL_GCM_CURRENT;
    g_psgl.current_context = context;
    g_psgl.current_device = device;
    if (!device) return;
    context->viewport[0] = 0;
    context->viewport[1] = 0;
    context->viewport[2] = device->render_width;
    context->viewport[3] = device->render_height;
    context->scissor[0] = 0;
    context->scissor[1] = 0;
    context->scissor[2] = device->render_width;
    context->scissor[3] = device->render_height;
    context->dirty |= PSGL_DIRTY_FRAMEBUFFER | PSGL_DIRTY_VIEWPORT;
    psgl_bind_render_target(context);
    psgl_emit_viewport(context);
}

void psgl_context_reset_current(void)
{
    if (g_psgl.current_context) g_psgl.current_context->device = NULL;
    g_psgl.current_context = NULL;
    g_psgl.current_device = NULL;
}

PSGLcontext *psgl_context_current(void)
{
    return g_psgl.current_context;
}

PSGLdevice *psgl_context_current_device(void)
{
    return g_psgl.current_device;
}

void psgl_context_swap(void)
{
    PSGLcontext *context = g_psgl.current_context;
    PSGLdevice *device = g_psgl.current_device;
    if (!context || !device || !context->gcm) return;

    if (device->submitted_flips) {
        psgl_wait_flip();
    } else {
        cellGcmResetFlipStatus();
    }
    PSGLframeBuffer *frame = &device->frames[device->current_frame];
    if (cellGcmSetFlip(context->gcm, frame->id) == 0) {
        cellGcmFlush(context->gcm);
        cellGcmSetWaitFlip(context->gcm);
        g_psgl.last_flip_time = psglGetSystemTime();
        device->submitted_flips++;
        device->current_frame = (device->current_frame + 1u) % device->frame_count;
        context->dirty |= PSGL_DIRTY_FRAMEBUFFER;
        psgl_bind_render_target(context);
        psgl_emit_viewport(context);
        if (g_psgl.flip_handler) g_psgl.flip_handler(0u);
    }
}

void psgl_context_set_clear_color(GLfloat red, GLfloat green,
                                  GLfloat blue, GLfloat alpha)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    context->clear_color[0] = psgl_clampf(red, 0.0f, 1.0f);
    context->clear_color[1] = psgl_clampf(green, 0.0f, 1.0f);
    context->clear_color[2] = psgl_clampf(blue, 0.0f, 1.0f);
    context->clear_color[3] = psgl_clampf(alpha, 0.0f, 1.0f);
    context->dirty |= PSGL_DIRTY_CLEAR;
}

void psgl_context_set_clear_depth(GLfloat depth)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    context->clear_depth = psgl_clampf(depth, 0.0f, 1.0f);
    context->dirty |= PSGL_DIRTY_CLEAR;
}

void psgl_context_set_clear_stencil(GLint stencil)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    context->clear_stencil = stencil;
    context->dirty |= PSGL_DIRTY_CLEAR;
}

void psgl_context_clear(GLbitfield mask)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !context->device || !context->gcm) return;
    if (context->dirty & PSGL_DIRTY_FRAMEBUFFER) psgl_bind_render_target(context);

    uint32_t clear_mask = 0u;
    if (mask & GL_COLOR_BUFFER_BIT) {
        cellGcmSetClearColor(context->gcm, psgl_pack_clear_color(context->clear_color));
        clear_mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
    }
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        cellGcmSetClearDepthStencil(context->gcm,
                                    psgl_pack_depth_stencil(context->clear_depth,
                                                           context->clear_stencil));
        if (mask & GL_DEPTH_BUFFER_BIT) clear_mask |= GCM_CLEAR_Z;
        if (mask & GL_STENCIL_BUFFER_BIT) clear_mask |= GCM_CLEAR_S;
    }
    if (clear_mask) cellGcmSetClearSurface(context->gcm, clear_mask);
    context->dirty &= ~PSGL_DIRTY_CLEAR;
}

void psgl_context_set_viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    if (width < 0 || height < 0) return;
    context->viewport[0] = x < 0 ? 0 : x;
    context->viewport[1] = y < 0 ? 0 : y;
    context->viewport[2] = width;
    context->viewport[3] = height;
    context->dirty |= PSGL_DIRTY_VIEWPORT;
    psgl_emit_viewport(context);
}

void psgl_context_gen_buffers(GLsizei n, GLuint *buffers)
{
    if (n < 0 || !buffers) return;
    for (GLsizei i = 0; i < n; i++) {
        GLuint name = g_psgl_next_buffer_name++;
        if (name == 0u) name = g_psgl_next_buffer_name++;
        buffers[i] = psgl_create_buffer(name) ? name : 0u;
    }
}

void psgl_context_delete_buffers(GLsizei n, const GLuint *buffers)
{
    if (n < 0 || !buffers) return;
    for (GLsizei i = 0; i < n; i++) {
        GLuint name = buffers[i];
        if (name == 0u) continue;
        PSGLbufferObject *buffer = g_psgl_buffers;
        while (buffer) {
            if (buffer->name == name) {
                psgl_clear_deleted_buffer_bindings(name);
                psgl_release_buffer_storage(buffer);
                buffer->name = 0u;
                break;
            }
            buffer = buffer->next;
        }
    }
}

void psgl_context_bind_buffer(GLenum target, GLuint name)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    if (name != 0u && !psgl_create_buffer(name)) return;
    if (target == GL_ARRAY_BUFFER) {
        context->bound_array_buffer = name;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        context->bound_element_array_buffer = name;
    }
}

void psgl_context_buffer_data(GLenum target, GLsizeiptr size,
                              const GLvoid *data, GLenum usage)
{
    PSGLbufferObject *buffer = psgl_bound_buffer(g_psgl.current_context, target);
    if (!buffer || size < 0) return;

    if (size == 0) {
        psgl_release_buffer_storage(buffer);
        buffer->usage = usage;
        return;
    }

    void *address = rsxMemalign(PSGL_BUFFER_ALIGNMENT, (uint32_t)size);
    uint32_t offset = 0u;
    if (!address) return;
    if (cellGcmAddressToOffset(address, &offset) != 0) {
        return;
    }
    if (data) psgl_copy(address, data, (uint32_t)size);

    psgl_release_buffer_storage(buffer);
    buffer->address = address;
    buffer->offset = offset;
    buffer->size = (uint32_t)size;
    buffer->usage = usage;
}

void psgl_context_buffer_sub_data(GLenum target, GLintptr offset,
                                  GLsizeiptr size, const GLvoid *data)
{
    PSGLbufferObject *buffer = psgl_bound_buffer(g_psgl.current_context, target);
    if (!buffer || !data || offset < 0 || size < 0) return;
    uint32_t begin = (uint32_t)offset;
    uint32_t count = (uint32_t)size;
    if (begin > buffer->size || count > buffer->size - begin) return;
    psgl_copy((unsigned char *)buffer->address + begin, data, count);
}

void psgl_context_get_buffer_parameteriv(GLenum target, GLenum pname,
                                         GLint *params)
{
    if (!params) return;
    PSGLbufferObject *buffer = psgl_bound_buffer(g_psgl.current_context, target);
    if (!buffer) {
        *params = 0;
        return;
    }
    switch (pname) {
    case GL_BUFFER_SIZE:
        *params = (GLint)buffer->size;
        break;
    case GL_BUFFER_USAGE:
        *params = (GLint)buffer->usage;
        break;
    case GL_BUFFER_ACCESS:
        *params = buffer->mapped ? GL_READ_WRITE : GL_WRITE_ONLY;
        break;
    case GL_BUFFER_MAPPED:
        *params = buffer->mapped;
        break;
    default:
        *params = 0;
        break;
    }
}

GLvoid *psgl_context_map_buffer(GLenum target, GLenum access)
{
    (void)access;
    PSGLbufferObject *buffer = psgl_bound_buffer(g_psgl.current_context, target);
    if (!buffer || !buffer->address) return NULL;
    buffer->mapped = GL_TRUE;
    return buffer->address;
}

GLboolean psgl_context_unmap_buffer(GLenum target)
{
    PSGLbufferObject *buffer = psgl_bound_buffer(g_psgl.current_context, target);
    if (!buffer) return GL_FALSE;
    buffer->mapped = GL_FALSE;
    return GL_TRUE;
}

void psgl_context_set_client_state(GLenum array, GLboolean enabled)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    switch (array) {
    case GL_VERTEX_ARRAY:
        context->attribs[PSGL_ATTRIB_VERTEX].enabled = enabled;
        break;
    case GL_NORMAL_ARRAY:
        context->attribs[PSGL_ATTRIB_NORMAL].enabled = enabled;
        break;
    case GL_COLOR_ARRAY:
        context->attribs[PSGL_ATTRIB_COLOR].enabled = enabled;
        break;
    case GL_TEXTURE_COORD_ARRAY:
        context->attribs[PSGL_ATTRIB_TEXCOORD].enabled = enabled;
        break;
    default:
        break;
    }
}

void psgl_context_set_attrib_pointer(PSGLvertexAttribSlot slot, GLint size,
                                     GLenum type, GLsizei stride,
                                     const GLvoid *pointer)
{
    if (slot >= PSGL_MAX_VERTEX_ATTRIBS || stride < 0) return;
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    PSGLvertexAttribState *attrib = &context->attribs[slot];
    attrib->size = size;
    attrib->type = type;
    attrib->stride = stride;
    attrib->pointer = pointer;
    attrib->buffer_name = context->bound_array_buffer;
    attrib->buffer_offset = context->bound_array_buffer ? (uint32_t)(uintptr_t)pointer : 0u;
}

PSGLuint64 psglGetSystemTime(void)
{
    uint64_t sec = 0;
    uint64_t nsec = 0;
    if (psgl_get_current_time(&sec, &nsec) != 0) return 0;
    return (PSGLuint64)(sec * 1000000ull + nsec / 1000ull);
}

PSGLuint64 psglGetLastFlipTime(void)
{
    return g_psgl.last_flip_time;
}

void psglSetFlipHandler(void (*handler)(const GLuint head))
{
    g_psgl.flip_handler = handler;
    cellGcmSetFlipHandler(handler ? psgl_flip_trampoline : NULL);
}

void psglSetVBlankHandler(void (*handler)(const GLuint head))
{
    g_psgl.vblank_handler = handler;
    cellGcmSetVBlankHandler(handler ? psgl_vblank_trampoline : NULL);
}
