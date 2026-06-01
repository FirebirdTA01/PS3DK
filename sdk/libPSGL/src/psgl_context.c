#include "psgl_context.h"
#include "cg_internal.h"
#include "ffp_emit.h"
#include "ffp_lit_bootstrap_fpo.h"
#include "ffp_lit_bootstrap_vpo.h"
#include "ffp_minimal_fpo.h"
#include "ffp_minimal_vpo.h"

#include <malloc.h>
#include <math.h>
#include <ppu-types.h>
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
#define PSGL_LABEL_PREPARED_BUFFER 0x41u
#define PSGL_LABEL_BUFFER_STATUS   0x42u
#define PSGL_BUFFER_IDLE           0u
#define PSGL_BUFFER_BUSY           1u
#define PSGL_MAX_QUEUE_FRAMES      0u
#define PSGL_TEXTURE_ALIGNMENT     128u
#define PSGL_CELL_FS_O_RDONLY      000000

extern s32 cellFsOpen(const char *path, s32 flags, s32 *fd,
                      const void *arg, u64 argsize);
extern s32 cellFsClose(s32 fd);
extern s32 cellFsRead(s32 fd, void *ptr, u64 len, u64 *read);

typedef struct PSGLbufferObject {
    GLuint name;
    GLenum usage;
    uint32_t size;
    void *address;
    uint32_t offset;
    GLboolean mapped;
    struct PSGLbufferObject *next;
} PSGLbufferObject;

typedef struct PSGLtextureObject {
    GLuint name;
    GLenum target;
    GLenum min_filter;
    GLenum mag_filter;
    GLenum wrap_s;
    GLenum wrap_t;
    GLenum allocation_hint;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint8_t levels;
    uint8_t rsx_format;
    uint8_t location;
    uint8_t linear;
    uint32_t size;
    void *address;
    uint32_t offset;
    CellGcmTexture gcm_texture;
    struct PSGLtextureObject *next;
} PSGLtextureObject;

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
static PSGLtextureObject *g_psgl_textures;
static GLuint g_psgl_next_buffer_name = 1u;
static GLuint g_psgl_next_texture_name = 1u;

static PSGLcgParameter g_psgl_ffp_vp_params[1];
static PSGLcgProgram g_psgl_ffp_vp;
static PSGLcgProgram g_psgl_ffp_fp;
static uint32_t g_psgl_ffp_initialized;
static PSGLcgParameter g_psgl_ffp_lit_vp_params[1];
static PSGLcgParameter g_psgl_ffp_lit_fp_params[1];
static PSGLcgProgram g_psgl_ffp_lit_vp;
static PSGLcgProgram g_psgl_ffp_lit_fp;
static uint32_t g_psgl_ffp_lit_initialized;
static PSGLcgParameter g_psgl_ffp_library_vp_params[1];
static PSGLcgProgram g_psgl_ffp_library_vp;
static PSGLcgProgram g_psgl_ffp_library_fp;
static uint32_t g_psgl_ffp_library_valid;

static uint32_t psgl_texture_unit_index(GLenum texture);

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

static uint32_t psgl_texture_remap_argb(void)
{
    return GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY,
                                  GCM_TEXTURE_REMAP_COLOR_A,
                                  GCM_TEXTURE_REMAP_COLOR_R,
                                  GCM_TEXTURE_REMAP_COLOR_G,
                                  GCM_TEXTURE_REMAP_COLOR_B,
                                  GCM_TEXTURE_REMAP_TYPE_REMAP,
                                  GCM_TEXTURE_REMAP_TYPE_REMAP,
                                  GCM_TEXTURE_REMAP_TYPE_REMAP,
                                  GCM_TEXTURE_REMAP_TYPE_REMAP);
}

static uint32_t psgl_morton2(uint32_t x, uint32_t y)
{
    uint32_t out = 0u;
    for (uint32_t bit = 0u; bit < 16u; bit++) {
        out |= ((x >> bit) & 1u) << (bit * 2u);
        out |= ((y >> bit) & 1u) << (bit * 2u + 1u);
    }
    return out;
}

static int psgl_is_power_of_two(uint32_t value)
{
    return value && ((value & (value - 1u)) == 0u);
}

static void psgl_flip_trampoline(uint32_t head)
{
    PSGLdevice *device = g_psgl.current_device;
    if (device) {
        uint32_t target = device->flip_target_frame;
        for (uint32_t i = device->frame_on_display;
             i != target && device->frame_count;
             i = (i + 1u) % device->frame_count) {
            volatile uint32_t *label =
                (volatile uint32_t *)cellGcmGetLabelAddress(
                    (uint8_t)(PSGL_LABEL_BUFFER_STATUS + i));
            if (label) *label = PSGL_BUFFER_IDLE;
        }
        device->frame_on_display = target;
        device->flip_pending = 0u;
    }
    if (g_psgl.flip_handler) g_psgl.flip_handler((GLuint)head);
}

static void psgl_vblank_trampoline(uint32_t head)
{
    PSGLdevice *device = g_psgl.current_device;
    volatile uint32_t *prepared =
        (volatile uint32_t *)cellGcmGetLabelAddress(PSGL_LABEL_PREPARED_BUFFER);
    if (device && prepared) {
        uint32_t data = *prepared;
        uint32_t buffer = data >> 8;
        uint32_t queue_id = data & 0x7u;
        if (!device->flip_pending && buffer != device->frame_on_display) {
            device->flip_pending = 1u;
            if (cellGcmSetFlipImmediate((uint8_t)queue_id) == 0) {
                device->flip_target_frame = buffer;
            } else {
                device->flip_pending = 0u;
            }
        }
    }
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

static void psgl_matrix_multiply(GLfloat out[16], const GLfloat a[16],
                                 const GLfloat b[16])
{
    GLfloat tmp[16];
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t col = 0u; col < 4u; col++) {
            tmp[row * 4u + col] =
                a[row * 4u + 0u] * b[0u * 4u + col] +
                a[row * 4u + 1u] * b[1u * 4u + col] +
                a[row * 4u + 2u] * b[2u * 4u + col] +
                a[row * 4u + 3u] * b[3u * 4u + col];
        }
    }
    psgl_copy(out, tmp, sizeof(tmp));
}

static void psgl_matrix_transpose(GLfloat out[16], const GLfloat in[16])
{
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t col = 0u; col < 4u; col++)
            out[row * 4u + col] = in[col * 4u + row];
    }
}

static void psgl_matrix_transform4(const GLfloat matrix[16],
                                   const GLfloat in[4], GLfloat out[4])
{
    GLfloat tmp[4];
    for (uint32_t row = 0u; row < 4u; row++) {
        tmp[row] = matrix[row * 4u + 0u] * in[0] +
                   matrix[row * 4u + 1u] * in[1] +
                   matrix[row * 4u + 2u] * in[2] +
                   matrix[row * 4u + 3u] * in[3];
    }
    psgl_copy(out, tmp, sizeof(tmp));
}

static GLfloat *psgl_current_matrix(PSGLcontext *context)
{
    uint32_t unit;
    if (!context) return NULL;
    switch (context->matrix_mode) {
    case GL_MODELVIEW:  return context->modelview;
    case GL_PROJECTION: return context->projection;
    case GL_TEXTURE:
        unit = psgl_texture_unit_index(context->active_texture);
        if (unit >= PSGL_MAX_TEXTURE_UNITS) unit = 0u;
        return context->texture_matrix[unit];
    default:            return context->modelview;
    }
}

static void psgl_matrix_mark_dirty(PSGLcontext *context)
{
    if (!context) return;
    context->dirty |= PSGL_DIRTY_MATRICES | PSGL_DIRTY_CG;
}

static void psgl_mark_lighting_dirty(PSGLcontext *context)
{
    if (!context) return;
    context->dirty |= PSGL_DIRTY_LIGHTING | PSGL_DIRTY_CG;
}

static void psgl_set_float4(GLfloat dst[4], const GLfloat *src)
{
    if (!dst || !src) return;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

static void psgl_set_float3(GLfloat dst[3], const GLfloat *src)
{
    if (!dst || !src) return;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

static void psgl_get_float4(const GLfloat src[4], GLfloat *dst)
{
    if (!src || !dst) return;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

static void psgl_get_float3(const GLfloat src[3], GLfloat *dst)
{
    if (!src || !dst) return;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

static uint32_t psgl_light_index(GLenum light)
{
    if (light < GL_LIGHT0) return PSGL_MAX_LIGHTS;
    light -= GL_LIGHT0;
    return light < PSGL_MAX_LIGHTS ? (uint32_t)light : PSGL_MAX_LIGHTS;
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

static uint32_t psgl_align_u32(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static int psgl_msaa_mode_valid(GLenum mode)
{
    switch (mode) {
    case 0:
    case GL_MULTISAMPLING_NONE_SCE:
    case GL_MULTISAMPLING_2X_DIAGONAL_CENTERED_SCE:
    case GL_MULTISAMPLING_4X_SQUARE_CENTERED_SCE:
    case GL_MULTISAMPLING_4X_SQUARE_ROTATED_SCE:
        return 1;
    default:
        return 0;
    }
}

static uint8_t psgl_msaa_antialias(GLenum mode)
{
    switch (mode) {
    case GL_MULTISAMPLING_2X_DIAGONAL_CENTERED_SCE:
        return GCM_SURFACE_DIAGONAL_CENTERED_2;
    case GL_MULTISAMPLING_4X_SQUARE_CENTERED_SCE:
        return GCM_SURFACE_SQUARE_CENTERED_4;
    case GL_MULTISAMPLING_4X_SQUARE_ROTATED_SCE:
        return GCM_SURFACE_SQUARE_ROTATED_4;
    case 0:
    case GL_MULTISAMPLING_NONE_SCE:
    default:
        return GCM_SURFACE_CENTER_1;
    }
}

static uint32_t psgl_msaa_samples_x(GLenum mode)
{
    switch (mode) {
    case GL_MULTISAMPLING_2X_DIAGONAL_CENTERED_SCE:
    case GL_MULTISAMPLING_4X_SQUARE_CENTERED_SCE:
    case GL_MULTISAMPLING_4X_SQUARE_ROTATED_SCE:
        return 2u;
    default:
        return 1u;
    }
}

static uint32_t psgl_msaa_samples_y(GLenum mode)
{
    switch (mode) {
    case GL_MULTISAMPLING_4X_SQUARE_CENTERED_SCE:
    case GL_MULTISAMPLING_4X_SQUARE_ROTATED_SCE:
        return 2u;
    default:
        return 1u;
    }
}

static int psgl_device_has_msaa_storage(const PSGLdevice *device)
{
    if (!device) return 0;
    return psgl_msaa_samples_x(device->multisampling_mode) > 1u ||
           psgl_msaa_samples_y(device->multisampling_mode) > 1u;
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

static uint32_t psgl_halfword_swap_u32(uint32_t value)
{
    return ((value >> 16) & 0xffffu) | ((value & 0xffffu) << 16);
}

static void psgl_patch_fragment_slot(uint32_t *slot, const uint32_t value[4])
{
    slot[0] = psgl_halfword_swap_u32(value[0]);
    slot[1] = psgl_halfword_swap_u32(value[1]);
    slot[2] = psgl_halfword_swap_u32(value[2]);
    slot[3] = psgl_halfword_swap_u32(value[3]);
}

static void psgl_copy_name(char *dst, const char *src)
{
    uint32_t i = 0u;
    if (!dst) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (; i + 1u < PSGL_CG_NAME_MAX && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static uint32_t psgl_read_be32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static int psgl_bytes_equal(const unsigned char *data, const char *text,
                            uint32_t count)
{
    for (uint32_t i = 0u; i < count; i++) {
        if (data[i] != (unsigned char)text[i]) return 0;
    }
    return 1;
}

static int psgl_ffp_init_pair(PSGLcgProgram *vp, PSGLcgProgram *fp,
                              PSGLcgParameter *vp_params,
                              unsigned char *vpo, uint32_t vpo_size,
                              unsigned char *fpo, uint32_t fpo_size)
{
    const float *defaults = NULL;
    uint32_t mvp_index;

    if (!vp || !fp || !vp_params || !vpo || !vpo_size || !fpo || !fpo_size)
        return 0;
    psgl_zero(vp, sizeof(*vp));
    psgl_zero(fp, sizeof(*fp));
    psgl_zero(vp_params, sizeof(*vp_params));

    if (cellCgbRead(vpo, vpo_size, &vp->cgb_program) != 0)
        return 0;
    if (cellCgbRead(fpo, fpo_size, &fp->cgb_program) != 0)
        return 0;

    vp->magic = PSGL_CG_PROGRAM_MAGIC;
    vp->profile = CG_PROFILE_SCE_VP_RSX;
    vp->cgb_profile = cellCgbGetProfile(&vp->cgb_program);
    vp->binary = vpo;
    vp->binary_size = vpo_size;
    vp->parameters = vp_params;
    vp->parameter_count = 1u;
    vp->loaded = CG_TRUE;

    fp->magic = PSGL_CG_PROGRAM_MAGIC;
    fp->profile = CG_PROFILE_SCE_FP_RSX;
    fp->cgb_profile = cellCgbGetProfile(&fp->cgb_program);
    fp->binary = fpo;
    fp->binary_size = fpo_size;
    fp->loaded = CG_TRUE;

    mvp_index = cellCgbMapLookup(&vp->cgb_program, "modelViewProj");
    if (mvp_index == (uint32_t)CELL_CGB_ERROR_FAILED) return 0;

    vp_params[0].magic = PSGL_CG_PARAMETER_MAGIC;
    vp_params[0].program = vp;
    vp_params[0].index = mvp_index;
    psgl_copy_name(vp_params[0].name, "modelViewProj");
    vp_params[0].type = CG_FLOAT4x4;
    vp_params[0].resource = CG_C;
    vp_params[0].variability = CG_UNIFORM;
    vp_params[0].direction = CG_IN;
    vp_params[0].vertex_register = 0xffffu;
    vp_params[0].fragment_register = 0xffffu;
    vp_params[0].value_count = 16u;
    vp_params[0].dirty = CG_TRUE;
    cellCgbMapGetVertexUniformRegister(&vp->cgb_program, mvp_index,
                                       &vp_params[0].vertex_register,
                                       &defaults);
    if (defaults)
        psgl_copy(vp_params[0].value, defaults, sizeof(vp_params[0].value));

    return 1;
}

static int psgl_ffp_init_minimal(void)
{
    if (g_psgl_ffp_initialized) return 1;
    if (!psgl_ffp_init_pair(&g_psgl_ffp_vp, &g_psgl_ffp_fp,
                            g_psgl_ffp_vp_params,
                            psgl_ffp_minimal_vpo,
                            psgl_ffp_minimal_vpo_len,
                            psgl_ffp_minimal_fpo,
                            psgl_ffp_minimal_fpo_len))
        return 0;
    g_psgl_ffp_initialized = 1u;
    return 1;
}

static int psgl_ffp_init_lit_bootstrap(void)
{
    uint32_t light_index;
    PSGLcgParameter *parameter = &g_psgl_ffp_lit_fp_params[0];
    if (g_psgl_ffp_lit_initialized) return 1;
    if (!psgl_ffp_init_pair(&g_psgl_ffp_lit_vp, &g_psgl_ffp_lit_fp,
                            g_psgl_ffp_lit_vp_params,
                            psgl_ffp_lit_bootstrap_vpo,
                            psgl_ffp_lit_bootstrap_vpo_len,
                            psgl_ffp_lit_bootstrap_fpo,
                            psgl_ffp_lit_bootstrap_fpo_len))
        return 0;

    light_index = cellCgbMapLookup(&g_psgl_ffp_lit_fp.cgb_program,
                                   "lightDirection");
    if (light_index == (uint32_t)CELL_CGB_ERROR_FAILED) return 0;

    psgl_zero(g_psgl_ffp_lit_fp_params, sizeof(g_psgl_ffp_lit_fp_params));
    parameter->magic = PSGL_CG_PARAMETER_MAGIC;
    parameter->program = &g_psgl_ffp_lit_fp;
    parameter->index = light_index;
    psgl_copy_name(parameter->name, "lightDirection");
    parameter->type = CG_FLOAT3;
    parameter->resource = CG_C;
    parameter->variability = CG_UNIFORM;
    parameter->direction = CG_IN;
    parameter->vertex_register = 0xffffu;
    parameter->fragment_register = 0xffffu;
    parameter->value[2] = 1.0f;
    parameter->value_count = 4u;
    parameter->dirty = CG_TRUE;
    g_psgl_ffp_lit_fp.parameters = g_psgl_ffp_lit_fp_params;
    g_psgl_ffp_lit_fp.parameter_count = 1u;
    g_psgl_ffp_lit_initialized = 1u;
    return 1;
}

static uint32_t psgl_ffp_state_mask(const PSGLcontext *context)
{
    uint32_t mask = PSGL_FFP_MINIMAL_MASK;
    if (!context) return 0u;
    if (context->bound_vertex_program || context->bound_fragment_program)
        return 0u;
    if (!context->attribs[PSGL_ATTRIB_VERTEX].enabled)
        return 0u;
    if (context->lighting_enabled) {
        if (!context->attribs[PSGL_ATTRIB_NORMAL].enabled)
            return 0u;
        for (uint32_t i = 0u; i < PSGL_MAX_TEXTURE_UNITS; i++) {
            if (context->textures[i].texture_2d_enabled)
                return 0u;
        }
        if (context->fog_enabled) return 0u;
        mask |= PSGL_FFP_LIGHTING_MASK;
        for (uint32_t i = 0u; i < PSGL_MAX_LIGHTS; i++) {
            if (context->lights[i].enabled)
                mask |= (1u << (8u + i));
        }
        return mask;
    }
    if (!context->attribs[PSGL_ATTRIB_TEXCOORD].enabled)
        return 0u;
    if (!context->textures[0].texture_2d_enabled ||
        !context->textures[0].texture_2d)
        return 0u;
    for (uint32_t i = 1u; i < PSGL_MAX_TEXTURE_UNITS; i++) {
        if (context->textures[i].texture_2d_enabled)
            return 0u;
    }
    if (context->fog_enabled) mask |= PSGL_FFP_FOG_MASK;
    return mask;
}

static int psgl_ffp_state_mask_to_programs(uint32_t mask, PSGLcgProgram **vp,
                                           PSGLcgProgram **fp)
{
    if (vp) *vp = NULL;
    if (fp) *fp = NULL;
    if ((mask & PSGL_FFP_LIGHTING_MASK) != 0u) {
        if ((mask & PSGL_FFP_FOG_MASK) != 0u) return 0;
        if (!psgl_ffp_init_lit_bootstrap()) return 0;
        if (vp) *vp = &g_psgl_ffp_lit_vp;
        if (fp) *fp = &g_psgl_ffp_lit_fp;
        return 1;
    }
    if (mask != PSGL_FFP_MINIMAL_MASK) return 0;
    if (g_psgl_ffp_library_valid) {
        if (vp) *vp = &g_psgl_ffp_library_vp;
        if (fp) *fp = &g_psgl_ffp_library_fp;
        return 1;
    }
    if (!psgl_ffp_init_minimal()) return 0;
    if (vp) *vp = &g_psgl_ffp_vp;
    if (fp) *fp = &g_psgl_ffp_fp;
    return 1;
}

static void psgl_ffp_update_matrices(PSGLcontext *context, PSGLcgProgram *vp)
{
    GLfloat mvp[16];
    PSGLcgParameter *parameter;
    if (!context || !vp || !vp->parameters || vp->parameter_count == 0u)
        return;
    parameter = &vp->parameters[0];
    psgl_matrix_multiply(mvp, context->projection, context->modelview);
    psgl_copy(parameter->value, mvp, sizeof(mvp));
    parameter->value_count = 16u;
    parameter->dirty = CG_TRUE;
    context->dirty &= ~PSGL_DIRTY_MATRICES;
}

static void psgl_ffp_update_lighting(PSGLcontext *context, PSGLcgProgram *fp)
{
    GLfloat value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GLfloat length;
    uint32_t light = 0u;
    uint32_t found = 0u;
    PSGLcgParameter *parameter;
    if (!context || !fp || !fp->parameters || fp->parameter_count == 0u)
        return;
    for (uint32_t i = 0u; i < PSGL_MAX_LIGHTS; i++) {
        if (context->lights[i].enabled) {
            light = i;
            found = 1u;
            break;
        }
    }
    if (found) {
        value[0] = context->lights[light].position[0];
        value[1] = context->lights[light].position[1];
        value[2] = context->lights[light].position[2];
        length = sqrtf(value[0] * value[0] + value[1] * value[1] +
                       value[2] * value[2]);
        if (length > 0.00001f) {
            value[0] /= length;
            value[1] /= length;
            value[2] /= length;
        } else {
            value[0] = 0.0f;
            value[1] = 0.0f;
            value[2] = 0.0f;
        }
    }
    parameter = &fp->parameters[0];
    psgl_copy(parameter->value, value, sizeof(value));
    parameter->value_count = 4u;
    parameter->dirty = CG_TRUE;
    context->dirty &= ~PSGL_DIRTY_LIGHTING;
}

static uint32_t psgl_bool(GLboolean value)
{
    return value ? CELL_GCM_TRUE : CELL_GCM_FALSE;
}

static int psgl_translate_compare(GLenum func, uint32_t *out)
{
    switch (func) {
    case GL_NEVER:    *out = CELL_GCM_NEVER; break;
    case GL_LESS:     *out = CELL_GCM_LESS; break;
    case GL_EQUAL:    *out = CELL_GCM_EQUAL; break;
    case GL_LEQUAL:   *out = CELL_GCM_LEQUAL; break;
    case GL_GREATER:  *out = CELL_GCM_GREATER; break;
    case GL_NOTEQUAL: *out = CELL_GCM_NOTEQUAL; break;
    case GL_GEQUAL:   *out = CELL_GCM_GEQUAL; break;
    case GL_ALWAYS:   *out = CELL_GCM_ALWAYS; break;
    default: return 0;
    }
    return 1;
}

static int psgl_translate_blend_factor(GLenum factor, uint16_t *out)
{
    switch (factor) {
    case GL_ZERO:                     *out = CELL_GCM_ZERO; break;
    case GL_ONE:                      *out = CELL_GCM_ONE; break;
    case GL_SRC_COLOR:                *out = CELL_GCM_SRC_COLOR; break;
    case GL_ONE_MINUS_SRC_COLOR:      *out = CELL_GCM_ONE_MINUS_SRC_COLOR; break;
    case GL_SRC_ALPHA:                *out = CELL_GCM_SRC_ALPHA; break;
    case GL_ONE_MINUS_SRC_ALPHA:      *out = CELL_GCM_ONE_MINUS_SRC_ALPHA; break;
    case GL_DST_ALPHA:                *out = CELL_GCM_DST_ALPHA; break;
    case GL_ONE_MINUS_DST_ALPHA:      *out = CELL_GCM_ONE_MINUS_DST_ALPHA; break;
    case GL_DST_COLOR:                *out = CELL_GCM_DST_COLOR; break;
    case GL_ONE_MINUS_DST_COLOR:      *out = CELL_GCM_ONE_MINUS_DST_COLOR; break;
    case GL_SRC_ALPHA_SATURATE:       *out = CELL_GCM_SRC_ALPHA_SATURATE; break;
    case GL_CONSTANT_COLOR:           *out = CELL_GCM_CONSTANT_COLOR; break;
    case GL_ONE_MINUS_CONSTANT_COLOR: *out = CELL_GCM_ONE_MINUS_CONSTANT_COLOR; break;
    case GL_CONSTANT_ALPHA:           *out = CELL_GCM_CONSTANT_ALPHA; break;
    case GL_ONE_MINUS_CONSTANT_ALPHA: *out = CELL_GCM_ONE_MINUS_CONSTANT_ALPHA; break;
    default: return 0;
    }
    return 1;
}

static int psgl_translate_blend_equation(GLenum mode, uint16_t *out)
{
    switch (mode) {
    case GL_FUNC_ADD:              *out = CELL_GCM_FUNC_ADD; break;
    case GL_MIN:                   *out = CELL_GCM_MIN; break;
    case GL_MAX:                   *out = CELL_GCM_MAX; break;
    case GL_FUNC_SUBTRACT:         *out = CELL_GCM_FUNC_SUBTRACT; break;
    case GL_FUNC_REVERSE_SUBTRACT: *out = CELL_GCM_FUNC_REVERSE_SUBTRACT; break;
    default: return 0;
    }
    return 1;
}

static int psgl_translate_stencil_op(GLenum op, uint32_t *out)
{
    switch (op) {
    case GL_KEEP:      *out = CELL_GCM_KEEP; break;
    case GL_ZERO:      *out = CELL_GCM_ZERO; break;
    case GL_REPLACE:   *out = CELL_GCM_REPLACE; break;
    case GL_INCR:      *out = CELL_GCM_INCR; break;
    case GL_DECR:      *out = CELL_GCM_DECR; break;
    case GL_INVERT:    *out = CELL_GCM_INVERT; break;
    case GL_INCR_WRAP: *out = CELL_GCM_INCR_WRAP; break;
    case GL_DECR_WRAP: *out = CELL_GCM_DECR_WRAP; break;
    default: return 0;
    }
    return 1;
}

static int psgl_translate_cull_face(GLenum mode, uint32_t *out)
{
    switch (mode) {
    case GL_FRONT:          *out = CELL_GCM_FRONT; break;
    case GL_BACK:           *out = CELL_GCM_BACK; break;
    case GL_FRONT_AND_BACK: *out = CELL_GCM_FRONT_AND_BACK; break;
    default: return 0;
    }
    return 1;
}

static int psgl_translate_front_face(GLenum mode, uint32_t *out)
{
    switch (mode) {
    case GL_CW:  *out = CELL_GCM_CW; break;
    case GL_CCW: *out = CELL_GCM_CCW; break;
    default: return 0;
    }
    return 1;
}

static int psgl_translate_logic_op(GLenum op, uint32_t *out)
{
    switch (op) {
    case GL_CLEAR:         *out = CELL_GCM_CLEAR; break;
    case GL_AND:           *out = CELL_GCM_AND; break;
    case GL_AND_REVERSE:   *out = CELL_GCM_AND_REVERSE; break;
    case GL_COPY:          *out = CELL_GCM_COPY; break;
    case GL_AND_INVERTED:  *out = CELL_GCM_AND_INVERTED; break;
    case GL_NOOP:          *out = CELL_GCM_NOOP; break;
    case GL_XOR:           *out = CELL_GCM_XOR; break;
    case GL_OR:            *out = CELL_GCM_OR; break;
    case GL_NOR:           *out = CELL_GCM_NOR; break;
    case GL_EQUIV:         *out = CELL_GCM_EQUIV; break;
    case GL_INVERT:        *out = CELL_GCM_INVERT; break;
    case GL_OR_REVERSE:    *out = CELL_GCM_OR_REVERSE; break;
    case GL_COPY_INVERTED: *out = CELL_GCM_COPY_INVERTED; break;
    case GL_OR_INVERTED:   *out = CELL_GCM_OR_INVERTED; break;
    case GL_NAND:          *out = CELL_GCM_NAND; break;
    case GL_SET:           *out = CELL_GCM_SET; break;
    default: return 0;
    }
    return 1;
}

static void psgl_fill_surface(PSGLcontext *context, CellGcmSurface *surface)
{
    PSGLdevice *device = context->device;
    PSGLframeBuffer *frame = &device->frames[device->current_frame];
    GLenum msaa = context->multisample_enabled ?
        device->multisampling_mode : GL_MULTISAMPLING_NONE_SCE;
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
    surface->antiAlias = psgl_msaa_antialias(msaa);
    surface->width = frame->width;
    surface->height = frame->height;
}

static void psgl_bind_render_target(PSGLcontext *context)
{
    if (!context || !context->device || !context->gcm) return;
    CellGcmSurface surface;
    psgl_fill_surface(context, &surface);
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

static void psgl_emit_scissor(PSGLcontext *context)
{
    if (!context || !context->gcm || !context->device) return;
    if (context->scissor_test_enabled) {
        cellGcmSetScissor(context->gcm, (uint16_t)context->scissor[0],
                          (uint16_t)context->scissor[1],
                          (uint16_t)context->scissor[2],
                          (uint16_t)context->scissor[3]);
    } else {
        cellGcmSetScissor(context->gcm, 0u, 0u, context->device->render_width,
                          context->device->render_height);
    }
    context->dirty &= ~PSGL_DIRTY_SCISSOR;
}

static void psgl_emit_blend(PSGLcontext *context)
{
    uint16_t src_rgb;
    uint16_t dst_rgb;
    uint16_t src_alpha;
    uint16_t dst_alpha;
    uint16_t equation_rgb;
    uint16_t equation_alpha;
    if (!context || !context->gcm) return;
    if (!psgl_translate_blend_factor(context->blend_src_rgb, &src_rgb) ||
        !psgl_translate_blend_factor(context->blend_dst_rgb, &dst_rgb) ||
        !psgl_translate_blend_factor(context->blend_src_alpha, &src_alpha) ||
        !psgl_translate_blend_factor(context->blend_dst_alpha, &dst_alpha) ||
        !psgl_translate_blend_equation(context->blend_equation_rgb, &equation_rgb) ||
        !psgl_translate_blend_equation(context->blend_equation_alpha, &equation_alpha))
        return;
    cellGcmSetBlendEnable(context->gcm, psgl_bool(context->blend_enabled));
    cellGcmSetBlendFunc(context->gcm, src_rgb, dst_rgb, src_alpha, dst_alpha);
    cellGcmSetBlendEquation(context->gcm, equation_rgb, equation_alpha);
    context->dirty &= ~PSGL_DIRTY_BLEND;
}

static void psgl_emit_depth(PSGLcontext *context)
{
    uint32_t func;
    if (!context || !context->gcm) return;
    if (!psgl_translate_compare(context->depth_func, &func)) return;
    cellGcmSetDepthTestEnable(context->gcm, psgl_bool(context->depth_test_enabled));
    cellGcmSetDepthFunc(context->gcm, func);
    cellGcmSetDepthMask(context->gcm, psgl_bool(context->depth_mask));
    context->dirty &= ~PSGL_DIRTY_DEPTH;
}

static void psgl_emit_stencil(PSGLcontext *context)
{
    uint32_t func;
    uint32_t fail;
    uint32_t zfail;
    uint32_t zpass;
    if (!context || !context->gcm) return;
    if (!psgl_translate_compare(context->stencil_func, &func) ||
        !psgl_translate_stencil_op(context->stencil_fail, &fail) ||
        !psgl_translate_stencil_op(context->stencil_depth_fail, &zfail) ||
        !psgl_translate_stencil_op(context->stencil_depth_pass, &zpass))
        return;
    cellGcmSetStencilTestEnable(context->gcm, psgl_bool(context->stencil_test_enabled));
    cellGcmSetStencilFunc(context->gcm, func, context->stencil_ref,
                          context->stencil_value_mask);
    cellGcmSetStencilMask(context->gcm, context->stencil_write_mask);
    cellGcmSetStencilOp(context->gcm, fail, zfail, zpass);
    context->dirty &= ~PSGL_DIRTY_STENCIL;
}

static void psgl_emit_alpha(PSGLcontext *context)
{
    uint32_t func;
    uint32_t ref;
    if (!context || !context->gcm) return;
    if (!psgl_translate_compare(context->alpha_func, &func)) return;
    ref = psgl_float_to_unorm8(context->alpha_ref);
    cellGcmSetAlphaTestEnable(context->gcm, psgl_bool(context->alpha_test_enabled));
    cellGcmSetAlphaFunc(context->gcm, func, ref);
    context->dirty &= ~PSGL_DIRTY_ALPHA;
}

static void psgl_emit_raster(PSGLcontext *context)
{
    uint32_t cull;
    uint32_t front;
    uint32_t logic;
    uint32_t mask = 0u;
    if (!context || !context->gcm) return;
    if (!psgl_translate_cull_face(context->cull_face, &cull) ||
        !psgl_translate_front_face(context->front_face, &front) ||
        !psgl_translate_logic_op(context->logic_op, &logic))
        return;
    if (context->color_mask[0]) mask |= CELL_GCM_COLOR_MASK_R;
    if (context->color_mask[1]) mask |= CELL_GCM_COLOR_MASK_G;
    if (context->color_mask[2]) mask |= CELL_GCM_COLOR_MASK_B;
    if (context->color_mask[3]) mask |= CELL_GCM_COLOR_MASK_A;
    cellGcmSetCullFaceEnable(context->gcm, psgl_bool(context->cull_face_enabled));
    cellGcmSetCullFace(context->gcm, cull);
    cellGcmSetFrontFace(context->gcm, front);
    cellGcmSetDitherEnable(context->gcm, psgl_bool(context->dither_enabled));
    cellGcmSetColorMask(context->gcm, mask);
    cellGcmSetLogicOpEnable(context->gcm, psgl_bool(context->logic_op_enabled));
    cellGcmSetLogicOp(context->gcm, logic);
    context->dirty &= ~PSGL_DIRTY_RASTER;
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

static void psgl_init_flip_labels(PSGLdevice *device)
{
    if (!device) return;
    for (uint32_t i = 0; i < device->frame_count; i++) {
        volatile uint32_t *label =
            (volatile uint32_t *)cellGcmGetLabelAddress(
                (uint8_t)(PSGL_LABEL_BUFFER_STATUS + i));
        if (label) *label = PSGL_BUFFER_IDLE;
    }
    {
        volatile uint32_t *prepared =
            (volatile uint32_t *)cellGcmGetLabelAddress(PSGL_LABEL_PREPARED_BUFFER);
        volatile uint32_t *displayed =
            (volatile uint32_t *)cellGcmGetLabelAddress(
                (uint8_t)(PSGL_LABEL_BUFFER_STATUS + device->frame_on_display));
        if (prepared) *prepared = device->frame_on_display << 8;
        if (displayed) *displayed = PSGL_BUFFER_BUSY;
    }
}

static int psgl_ppu_too_far_ahead(const PSGLdevice *device)
{
    volatile uint32_t *prepared =
        (volatile uint32_t *)cellGcmGetLabelAddress(PSGL_LABEL_PREPARED_BUFFER);
    uint32_t gpu;
    if (!device || !prepared || !device->frame_count) return 0;
    gpu = *prepared >> 8;
    return (((device->current_frame + device->frame_count - gpu) %
             device->frame_count) > PSGL_MAX_QUEUE_FRAMES);
}

static int psgl_make_frame_buffer(PSGLdevice *device, uint32_t id)
{
    PSGLframeBuffer *frame = &device->frames[id];
    uint32_t size = device->pitch * (uint32_t)device->storage_height;
    uint32_t display_offset;
    uint32_t display_pitch;
    frame->address = (uint32_t *)rsxMemalign(64u, size);
    if (!frame->address) return 0;
    if (cellGcmAddressToOffset(frame->address, &frame->offset) != 0) return 0;
    display_offset = frame->offset;
    display_pitch = frame->pitch;
    if (psgl_device_has_msaa_storage(device)) {
        uint32_t scanout_size;
        frame->scanout_pitch =
            psgl_align_u32((uint32_t)device->render_width * sizeof(uint32_t),
                           64u);
        scanout_size = frame->scanout_pitch * (uint32_t)device->render_height;
        frame->scanout_address = (uint32_t *)rsxMemalign(64u, scanout_size);
        if (!frame->scanout_address) return 0;
        if (cellGcmAddressToOffset(frame->scanout_address,
                                   &frame->scanout_offset) != 0)
            return 0;
        display_offset = frame->scanout_offset;
        display_pitch = frame->scanout_pitch;
    }
    if (cellGcmSetDisplayBuffer((uint8_t)id, display_offset, display_pitch,
                                device->width, device->height) != 0) {
        return 0;
    }
    return 1;
}

static void psgl_resolve_frame_for_display(PSGLcontext *context)
{
    PSGLdevice *device;
    PSGLframeBuffer *frame;
    GLenum mode;
    uint32_t samples_x;
    uint32_t samples_y;
    uint32_t block_x;
    uint32_t block_y;
    gcmTransferScale scale;
    gcmTransferSurface surface;
    if (!context || !context->device || !context->gcm) return;
    device = context->device;
    frame = &device->frames[device->current_frame];
    if (!frame->scanout_address) return;

    mode = context->multisample_enabled ?
        device->multisampling_mode : GL_MULTISAMPLING_NONE_SCE;
    samples_x = psgl_msaa_samples_x(mode);
    samples_y = psgl_msaa_samples_y(mode);

    psgl_zero(&scale, sizeof(scale));
    psgl_zero(&surface, sizeof(surface));
    scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
    scale.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
    scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
    scale.ratioX = rsxGetFixedSint32((float)samples_x);
    scale.ratioY = rsxGetFixedSint32((float)samples_y);
    scale.pitch = frame->pitch;
    scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
    scale.interp = GCM_TRANSFER_INTERPOLATOR_LINEAR;
    scale.inX = 0u;
    scale.inY = 0u;

    surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
    surface.pitch = (uint16_t)frame->scanout_pitch;

    cellGcmSetTransferScaleMode(context->gcm, GCM_TRANSFER_LOCAL_TO_LOCAL,
                                GCM_TRANSFER_SURFACE);
    for (block_y = 0u; block_y < device->render_height; block_y += 512u) {
        uint32_t block_h = (uint32_t)device->render_height - block_y;
        if (block_h > 512u) block_h = 512u;
        for (block_x = 0u; block_x < device->render_width; block_x += 512u) {
            uint32_t block_w = (uint32_t)device->render_width - block_x;
            if (block_w > 512u) block_w = 512u;
            scale.clipX = 0;
            scale.clipY = 0;
            scale.clipW = (uint16_t)block_w;
            scale.clipH = (uint16_t)block_h;
            scale.outX = 0;
            scale.outY = 0;
            scale.outW = (uint16_t)block_w;
            scale.outH = (uint16_t)block_h;
            scale.inW = (uint16_t)(block_w * samples_x);
            scale.inH = (uint16_t)(block_h * samples_y);
            scale.offset = frame->offset +
                (block_x * samples_x * sizeof(uint32_t)) +
                (block_y * samples_y * frame->pitch);
            surface.offset = frame->scanout_offset +
                (block_x * sizeof(uint32_t)) +
                (block_y * frame->scanout_pitch);
            cellGcmSetTransferScaleSurface(context->gcm, &scale, &surface);
        }
    }
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
    if (target == GL_TEXTURE_REFERENCE_BUFFER_SCE)
        return psgl_find_buffer(context->bound_texture_reference_buffer);
    return NULL;
}

static PSGLtextureObject *psgl_find_texture(GLuint name)
{
    PSGLtextureObject *texture = g_psgl_textures;
    while (texture) {
        if (texture->name == name) return texture;
        texture = texture->next;
    }
    return NULL;
}

static void psgl_init_texture_defaults(PSGLtextureObject *texture)
{
    texture->target = GL_TEXTURE_2D;
    texture->min_filter = GL_NEAREST;
    texture->mag_filter = GL_NEAREST;
    texture->wrap_s = GL_REPEAT;
    texture->wrap_t = GL_REPEAT;
    texture->allocation_hint = GL_TEXTURE_LINEAR_GPU_SCE;
    texture->location = CELL_GCM_LOCATION_LOCAL;
    texture->linear = 1u;
    texture->levels = 1u;
    texture->gcm_texture.remap = psgl_texture_remap_argb();
}

static PSGLtextureObject *psgl_create_texture(GLuint name)
{
    PSGLtextureObject *texture = psgl_find_texture(name);
    if (texture) return texture;
    for (texture = g_psgl_textures; texture; texture = texture->next) {
        if (texture->name == 0u) {
            PSGLtextureObject *next = texture->next;
            psgl_zero(texture, sizeof(*texture));
            texture->name = name;
            texture->next = next;
            psgl_init_texture_defaults(texture);
            return texture;
        }
    }
    texture = (PSGLtextureObject *)calloc(1u, sizeof(*texture));
    if (!texture) return NULL;
    texture->name = name;
    psgl_init_texture_defaults(texture);
    texture->next = g_psgl_textures;
    g_psgl_textures = texture;
    return texture;
}

static void psgl_release_texture_storage(PSGLtextureObject *texture)
{
    if (!texture) return;
    texture->address = NULL;
    texture->offset = 0u;
    texture->size = 0u;
    texture->width = 0u;
    texture->height = 0u;
    texture->pitch = 0u;
    psgl_zero(&texture->gcm_texture, sizeof(texture->gcm_texture));
    texture->gcm_texture.remap = psgl_texture_remap_argb();
}

static uint32_t psgl_texture_unit_index(GLenum texture)
{
    if (texture < GL_TEXTURE0) return PSGL_MAX_TEXTURE_UNITS;
    texture -= GL_TEXTURE0;
    return texture < PSGL_MAX_TEXTURE_UNITS ? (uint32_t)texture : PSGL_MAX_TEXTURE_UNITS;
}

static PSGLtextureObject *psgl_bound_texture(PSGLcontext *context, GLenum target)
{
    uint32_t unit;
    GLuint name;
    if (!context || target != GL_TEXTURE_2D) return NULL;
    unit = psgl_texture_unit_index(context->active_texture);
    if (unit >= PSGL_MAX_TEXTURE_UNITS) return NULL;
    name = context->textures[unit].texture_2d;
    return name ? psgl_find_texture(name) : NULL;
}

static uint8_t psgl_translate_texture_filter(GLenum filter)
{
    switch (filter) {
    case GL_NEAREST: return CELL_GCM_TEXTURE_NEAREST;
    case GL_LINEAR: return CELL_GCM_TEXTURE_LINEAR;
    case GL_NEAREST_MIPMAP_NEAREST: return CELL_GCM_TEXTURE_NEAREST_NEAREST;
    case GL_LINEAR_MIPMAP_NEAREST: return CELL_GCM_TEXTURE_LINEAR_NEAREST;
    case GL_NEAREST_MIPMAP_LINEAR: return CELL_GCM_TEXTURE_NEAREST_LINEAR;
    case GL_LINEAR_MIPMAP_LINEAR: return CELL_GCM_TEXTURE_LINEAR_LINEAR;
    default: return 0u;
    }
}

static uint8_t psgl_translate_texture_wrap(GLenum wrap)
{
    switch (wrap) {
    case GL_REPEAT: return CELL_GCM_TEXTURE_WRAP;
    case GL_CLAMP: return CELL_GCM_TEXTURE_CLAMP;
    case GL_CLAMP_TO_EDGE: return CELL_GCM_TEXTURE_CLAMP_TO_EDGE;
    case GL_MIRRORED_REPEAT: return CELL_GCM_TEXTURE_MIRROR;
    default: return 0u;
    }
}

static uint16_t psgl_align_u16(uint32_t value, uint32_t align)
{
    return (uint16_t)((value + align - 1u) & ~(align - 1u));
}

static uint32_t psgl_unpack_row_stride(GLsizei width, GLenum format,
                                       GLenum type, GLint alignment)
{
    uint32_t row = 0u;
    uint32_t align = alignment > 0 ? (uint32_t)alignment : 1u;
    if (format == GL_RGBA || format == GL_BGRA) {
        row = (type == GL_UNSIGNED_BYTE) ? (uint32_t)width * 4u : (uint32_t)width * 2u;
    } else if (format == GL_RGB) {
        row = (type == GL_UNSIGNED_BYTE) ? (uint32_t)width * 3u : (uint32_t)width * 2u;
    }
    if (align > 1u) row = (row + align - 1u) & ~(align - 1u);
    return row;
}

static int psgl_texture_supported(GLenum format, GLenum type)
{
    if ((format == GL_RGBA || format == GL_BGRA || format == GL_RGB) &&
        type == GL_UNSIGNED_BYTE)
        return 1;
    if (format == GL_RGBA &&
        (type == GL_UNSIGNED_SHORT_5_5_5_1 || type == GL_UNSIGNED_SHORT_4_4_4_4))
        return 1;
    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5)
        return 1;
    return 0;
}

static uint32_t psgl_read_u16_be(const unsigned char *src)
{
    return ((uint32_t)src[0] << 8) | (uint32_t)src[1];
}

static uint32_t psgl_pack_argb8(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t psgl_convert_texel_argb8(const unsigned char *src,
                                         GLenum format, GLenum type)
{
    if (type == GL_UNSIGNED_BYTE) {
        if (format == GL_RGBA)
            return psgl_pack_argb8(src[0], src[1], src[2], src[3]);
        if (format == GL_BGRA)
            return psgl_pack_argb8(src[2], src[1], src[0], src[3]);
        if (format == GL_RGB)
            return psgl_pack_argb8(src[0], src[1], src[2], 255u);
    } else if (type == GL_UNSIGNED_SHORT_5_6_5) {
        uint32_t v = psgl_read_u16_be(src);
        uint8_t r = (uint8_t)(((v >> 11) & 0x1fu) * 255u / 31u);
        uint8_t g = (uint8_t)(((v >> 5) & 0x3fu) * 255u / 63u);
        uint8_t b = (uint8_t)((v & 0x1fu) * 255u / 31u);
        return psgl_pack_argb8(r, g, b, 255u);
    } else if (type == GL_UNSIGNED_SHORT_5_5_5_1) {
        uint32_t v = psgl_read_u16_be(src);
        uint8_t r = (uint8_t)(((v >> 11) & 0x1fu) * 255u / 31u);
        uint8_t g = (uint8_t)(((v >> 6) & 0x1fu) * 255u / 31u);
        uint8_t b = (uint8_t)(((v >> 1) & 0x1fu) * 255u / 31u);
        uint8_t a = (v & 1u) ? 255u : 0u;
        return psgl_pack_argb8(r, g, b, a);
    } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
        uint32_t v = psgl_read_u16_be(src);
        uint8_t r = (uint8_t)(((v >> 12) & 0x0fu) * 17u);
        uint8_t g = (uint8_t)(((v >> 8) & 0x0fu) * 17u);
        uint8_t b = (uint8_t)(((v >> 4) & 0x0fu) * 17u);
        uint8_t a = (uint8_t)((v & 0x0fu) * 17u);
        return psgl_pack_argb8(r, g, b, a);
    }
    return 0u;
}

static uint32_t psgl_texel_size(GLenum format, GLenum type)
{
    if (type == GL_UNSIGNED_BYTE)
        return format == GL_RGB ? 3u : 4u;
    return 2u;
}

static void psgl_write_texture_image(PSGLtextureObject *texture,
                                     GLint xoffset, GLint yoffset,
                                     GLsizei width, GLsizei height,
                                     GLenum format, GLenum type,
                                     const GLvoid *pixels,
                                     GLint unpack_alignment)
{
    const unsigned char *src = (const unsigned char *)pixels;
    uint32_t source_stride = psgl_unpack_row_stride(width, format, type, unpack_alignment);
    uint32_t texel_size = psgl_texel_size(format, type);
    if (!texture || !texture->address || !pixels || !source_stride) return;
    for (GLsizei y = 0; y < height; y++) {
        for (GLsizei x = 0; x < width; x++) {
            uint32_t value = psgl_convert_texel_argb8(src + (uint32_t)y * source_stride +
                                                      (uint32_t)x * texel_size,
                                                      format, type);
            uint32_t dst_x = (uint32_t)xoffset + (uint32_t)x;
            uint32_t dst_y = (uint32_t)yoffset + (uint32_t)y;
            uint32_t index = texture->linear
                ? dst_y * (texture->pitch / 4u) + dst_x
                : psgl_morton2(dst_x, dst_y);
            ((uint32_t *)texture->address)[index] = value;
        }
    }
}

static void psgl_fill_gcm_texture(PSGLtextureObject *texture)
{
    if (!texture) return;
    psgl_zero(&texture->gcm_texture, sizeof(texture->gcm_texture));
    texture->gcm_texture.format = texture->rsx_format;
    texture->gcm_texture.mipmap = texture->levels ? texture->levels : 1u;
    texture->gcm_texture.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
    texture->gcm_texture.cubemap = 0u;
    texture->gcm_texture.remap = psgl_texture_remap_argb();
    texture->gcm_texture.width = texture->width;
    texture->gcm_texture.height = texture->height;
    texture->gcm_texture.depth = 1u;
    texture->gcm_texture.location = texture->location;
    texture->gcm_texture.pitch = texture->pitch;
    texture->gcm_texture.offset = texture->offset;
}

static uint32_t psgl_gl_primitive(GLenum mode)
{
    switch (mode) {
    case GL_POINTS: return CELL_GCM_PRIMITIVE_POINTS;
    case GL_LINES: return CELL_GCM_PRIMITIVE_LINES;
    case GL_LINE_LOOP: return CELL_GCM_PRIMITIVE_LINE_LOOP;
    case GL_LINE_STRIP: return CELL_GCM_PRIMITIVE_LINE_STRIP;
    case GL_TRIANGLES: return CELL_GCM_PRIMITIVE_TRIANGLES;
    case GL_TRIANGLE_STRIP: return CELL_GCM_PRIMITIVE_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN: return CELL_GCM_PRIMITIVE_TRIANGLE_FAN;
    default: return 0u;
    }
}

static uint8_t psgl_gl_vertex_type(GLenum type)
{
    switch (type) {
    case GL_FLOAT: return CELL_GCM_VERTEX_F;
    case GL_UNSIGNED_BYTE: return CELL_GCM_VERTEX_UB;
    case GL_UNSIGNED_SHORT: return CELL_GCM_VERTEX_S32K;
    default: return 0u;
    }
}

static uint8_t psgl_gl_index_type(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_SHORT: return CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16;
    case GL_UNSIGNED_INT: return CELL_GCM_DRAW_INDEX_ARRAY_TYPE_32;
    default: return 0xffu;
    }
}

static uint8_t psgl_attrib_hw_index(uint32_t slot)
{
    switch (slot) {
    case PSGL_ATTRIB_VERTEX: return GCM_VERTEX_ATTRIB_POS;
    case PSGL_ATTRIB_NORMAL: return GCM_VERTEX_ATTRIB_NORMAL;
    case PSGL_ATTRIB_COLOR: return GCM_VERTEX_ATTRIB_COLOR0;
    case PSGL_ATTRIB_TEXCOORD: return GCM_VERTEX_ATTRIB_TEX0;
    default: return 0u;
    }
}

static uint32_t psgl_attrib_stride(const PSGLvertexAttribState *attrib)
{
    if (attrib->stride > 0) return (uint32_t)attrib->stride;
    if (attrib->type == GL_FLOAT) return (uint32_t)attrib->size * 4u;
    if (attrib->type == GL_UNSIGNED_SHORT) return (uint32_t)attrib->size * 2u;
    return (uint32_t)attrib->size;
}

static void psgl_emit_vertex_arrays(PSGLcontext *context)
{
    if (!context || !context->gcm) return;
    for (uint32_t i = 0; i < PSGL_MAX_VERTEX_ATTRIBS; i++) {
        PSGLvertexAttribState *attrib = &context->attribs[i];
        PSGLbufferObject *buffer;
        uint8_t type;
        uint32_t offset;
        if (!attrib->enabled || attrib->size <= 0) continue;
        if (!attrib->buffer_name) continue;
        buffer = psgl_find_buffer(attrib->buffer_name);
        if (!buffer || !buffer->address) continue;
        type = psgl_gl_vertex_type(attrib->type);
        if (!type) continue;
        offset = buffer->offset + attrib->buffer_offset;
        cellGcmSetVertexDataArray(context->gcm, psgl_attrib_hw_index(i),
                                  0, (uint8_t)psgl_attrib_stride(attrib),
                                  (uint8_t)attrib->size, type,
                                  CELL_GCM_LOCATION_LOCAL, offset);
    }
}

static void psgl_emit_textures(PSGLcontext *context)
{
    if (!context || !context->gcm) return;
    for (uint32_t i = 0; i < PSGL_MAX_TEXTURE_UNITS; i++) {
        GLuint name = context->textures[i].texture_2d;
        PSGLtextureObject *texture = name ? psgl_find_texture(name) : NULL;
        if (!texture || !texture->address) {
            cellGcmSetTextureControl(context->gcm, (uint8_t)i, 0u,
                                     0u, 0u,
                                     CELL_GCM_TEXTURE_MAX_ANISO_1);
            continue;
        }
        uint8_t min_filter = psgl_translate_texture_filter(texture->min_filter);
        uint8_t mag_filter = psgl_translate_texture_filter(texture->mag_filter);
        uint8_t wrap_s = psgl_translate_texture_wrap(texture->wrap_s);
        uint8_t wrap_t = psgl_translate_texture_wrap(texture->wrap_t);
        if (!min_filter) min_filter = CELL_GCM_TEXTURE_NEAREST;
        if (!mag_filter) mag_filter = CELL_GCM_TEXTURE_NEAREST;
        if (!wrap_s) wrap_s = CELL_GCM_TEXTURE_WRAP;
        if (!wrap_t) wrap_t = CELL_GCM_TEXTURE_WRAP;
        cellGcmSetTexture(context->gcm, (uint8_t)i, &texture->gcm_texture);
        cellGcmSetTextureControl(context->gcm, (uint8_t)i, 1u,
                                 0u, 0u,
                                 CELL_GCM_TEXTURE_MAX_ANISO_1);
        cellGcmSetTextureFilter(context->gcm, (uint8_t)i, 0u,
                                min_filter, mag_filter,
                                CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
        cellGcmSetTextureAddress(context->gcm, (uint8_t)i,
                                 wrap_s, wrap_t, CELL_GCM_TEXTURE_WRAP,
                                 CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                                 CELL_GCM_TEXTURE_ZFUNC_LESS,
                                 CELL_GCM_TEXTURE_GAMMA_NONE);
        cellGcmSetTextureBorderColor(context->gcm, (uint8_t)i, 0u);
    }
    cellGcmSetInvalidateTextureCache(context->gcm, CELL_GCM_INVALIDATE_TEXTURE);
    context->dirty &= ~PSGL_DIRTY_TEXTURES;
}

static const void *psgl_cg_ucode(const PSGLcgProgram *program)
{
    return program ? cellCgbGetUCode(&program->cgb_program) : NULL;
}

static uint32_t psgl_cg_const_count(const PSGLcgParameter *parameter)
{
    uint32_t count = parameter->value_count;
    if (count == 0u) count = 4u;
    return (count + 3u) / 4u;
}

static int psgl_prepare_fragment_ucode(PSGLcgProgram *program)
{
    const void *ucode;
    uint32_t size;
    if (!program) return 0;
    if (program->fragment_ucode_address) return 1;
    ucode = cellCgbGetUCode(&program->cgb_program);
    size = cellCgbGetUCodeSize(&program->cgb_program);
    if (!ucode || !size) return 0;
    program->fragment_ucode_address = rsxMemalign(64u, size);
    if (!program->fragment_ucode_address) return 0;
    psgl_copy(program->fragment_ucode_address, ucode, size);
    if (cellGcmAddressToOffset(program->fragment_ucode_address,
                               &program->fragment_ucode_offset) != 0) {
        program->fragment_ucode_address = NULL;
        program->fragment_ucode_offset = 0u;
        return 0;
    }
    return 1;
}

static void psgl_patch_fragment_parameter(PSGLcgProgram *program,
                                          PSGLcgParameter *parameter)
{
    uint32_t count = 0u;
    uint16_t offsets[64];
    uint32_t size;
    if (!program || !parameter || !program->fragment_ucode_address) return;
    if (!parameter->value_count) return;
    cellCgbMapGetFragmentUniformOffsets(&program->cgb_program,
                                        parameter->index, NULL, &count);
    size = cellCgbGetUCodeSize(&program->cgb_program);
    if (!count || count > 64u) return;
    cellCgbMapGetFragmentUniformOffsets(&program->cgb_program,
                                        parameter->index, offsets, &count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = offsets[i];
        uint32_t *slot;
        uint32_t *host_slot;
        const uint32_t *value = (const uint32_t *)parameter->value;
        const uint8_t *host_ucode = (const uint8_t *)
            cellCgbGetUCode(&program->cgb_program);
        if (off + 16u > size) continue;
        slot = (uint32_t *)((uint8_t *)program->fragment_ucode_address + off);
        psgl_patch_fragment_slot(slot, value);
        if (host_ucode) {
            host_slot = (uint32_t *)(void *)(host_ucode + off);
            psgl_patch_fragment_slot(host_slot, value);
        }
    }
    parameter->dirty = CG_FALSE;
}

static void psgl_emit_vertex_program(PSGLcontext *context,
                                     PSGLcgProgram *program)
{
    const void *ucode;
    if (!context || !context->gcm || !program || !program->loaded) return;
    ucode = psgl_cg_ucode(program);
    if (!ucode) return;
    cellGcmSetVertexProgram(context->gcm, (CGprogram)program->binary,
                            (void *)ucode);
    cellGcmCgUploadInternalConsts(context->gcm, (CGprogram)program->binary);
    for (uint32_t i = 0; i < program->parameter_count; i++) {
        PSGLcgParameter *parameter = &program->parameters[i];
        if (parameter->resource != CG_C || parameter->vertex_register == 0xffffu)
            continue;
        if (!parameter->value_count) continue;
        cellGcmSetVertexProgramParameterBlock(context->gcm,
                                             parameter->vertex_register,
                                             psgl_cg_const_count(parameter),
                                             parameter->value);
        parameter->dirty = CG_FALSE;
    }
}

static void psgl_emit_fragment_program(PSGLcontext *context,
                                       PSGLcgProgram *program)
{
    if (!context || !context->gcm || !program || !program->loaded) return;
    if (!psgl_prepare_fragment_ucode(program)) return;
    for (uint32_t i = 0; i < program->parameter_count; i++) {
        if (program->parameters[i].dirty)
            psgl_patch_fragment_parameter(program, &program->parameters[i]);
    }
    cellGcmSetFragmentProgram(context->gcm, (CGprogram)program->binary,
                              program->fragment_ucode_offset);
}

static void psgl_validate_draw_state(PSGLcontext *context)
{
    PSGLcgProgram *vp;
    PSGLcgProgram *fp;
    uint32_t ffp_mask;
    if (!context) return;
    if (context->dirty & PSGL_DIRTY_FRAMEBUFFER) psgl_bind_render_target(context);
    if (context->dirty & PSGL_DIRTY_VIEWPORT) psgl_emit_viewport(context);
    if (context->dirty & PSGL_DIRTY_SCISSOR) psgl_emit_scissor(context);
    if (context->dirty & PSGL_DIRTY_BLEND) psgl_emit_blend(context);
    if (context->dirty & PSGL_DIRTY_DEPTH) psgl_emit_depth(context);
    if (context->dirty & PSGL_DIRTY_STENCIL) psgl_emit_stencil(context);
    if (context->dirty & PSGL_DIRTY_ALPHA) psgl_emit_alpha(context);
    if (context->dirty & PSGL_DIRTY_RASTER) psgl_emit_raster(context);
    if (context->dirty & PSGL_DIRTY_TEXTURES) psgl_emit_textures(context);
    if (context->dirty & PSGL_DIRTY_CG) {
        vp = psgl_cg_program(context->bound_vertex_program);
        fp = psgl_cg_program(context->bound_fragment_program);
        ffp_mask = psgl_ffp_state_mask(context);
        if ((!vp || !fp) && ffp_mask) {
            PSGLcgProgram *ffp_vp = NULL;
            PSGLcgProgram *ffp_fp = NULL;
            if (psgl_ffp_state_mask_to_programs(ffp_mask, &ffp_vp, &ffp_fp)) {
                if (!vp) vp = ffp_vp;
                if (!fp) fp = ffp_fp;
                psgl_ffp_update_matrices(context, ffp_vp);
                if ((ffp_mask & PSGL_FFP_LIGHTING_MASK) != 0u)
                    psgl_ffp_update_lighting(context, ffp_fp);
            }
        }
        psgl_emit_vertex_program(context, vp);
        psgl_emit_fragment_program(context, fp);
        psgl_emit_vertex_arrays(context);
        context->dirty &= ~PSGL_DIRTY_CG;
    }
}

static void psgl_clear_deleted_buffer_bindings(GLuint name)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    if (context->bound_array_buffer == name) context->bound_array_buffer = 0u;
    if (context->bound_element_array_buffer == name) context->bound_element_array_buffer = 0u;
    if (context->bound_texture_reference_buffer == name) context->bound_texture_reference_buffer = 0u;
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
    cellGcmSetVBlankHandler(psgl_vblank_trampoline);
    cellGcmSetFlipHandler(psgl_flip_trampoline);
    cellGcmResetFlipStatus();
    return 1;
}

void psgl_context_shutdown_system(void)
{
    if (!g_psgl.initialized) return;
    if (CELL_GCM_CURRENT && g_psgl.current_device &&
        g_psgl.current_device->submitted_flips) {
        cellGcmFinish(CELL_GCM_CURRENT, 1u);
    }
    cellGcmSetVBlankHandler(NULL);
    cellGcmSetFlipHandler(NULL);
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
    for (uint32_t unit = 0u; unit < PSGL_MAX_TEXTURE_UNITS; unit++)
        psgl_matrix_identity(context->texture_matrix[unit]);
    context->clear_color[3] = 1.0f;
    context->clear_depth = 1.0f;
    context->dither_enabled = GL_TRUE;
    context->blend_src_rgb = GL_ONE;
    context->blend_dst_rgb = GL_ZERO;
    context->blend_src_alpha = GL_ONE;
    context->blend_dst_alpha = GL_ZERO;
    context->blend_equation_rgb = GL_FUNC_ADD;
    context->blend_equation_alpha = GL_FUNC_ADD;
    context->depth_func = GL_LESS;
    context->depth_mask = GL_TRUE;
    context->alpha_func = GL_ALWAYS;
    context->alpha_ref = 0.0f;
    context->stencil_func = GL_ALWAYS;
    context->stencil_ref = 0;
    context->stencil_value_mask = 0xffffffffu;
    context->stencil_write_mask = 0xffffffffu;
    context->stencil_fail = GL_KEEP;
    context->stencil_depth_fail = GL_KEEP;
    context->stencil_depth_pass = GL_KEEP;
    context->color_mask[0] = GL_TRUE;
    context->color_mask[1] = GL_TRUE;
    context->color_mask[2] = GL_TRUE;
    context->color_mask[3] = GL_TRUE;
    context->logic_op = GL_COPY;
    context->cull_face = GL_BACK;
    context->front_face = GL_CCW;
    context->shade_model = GL_SMOOTH;
    context->multisample_enabled = GL_TRUE;
    context->color_material_face = GL_FRONT_AND_BACK;
    context->color_material_parameter = GL_AMBIENT_AND_DIFFUSE;
    context->current_color[0] = 1.0f;
    context->current_color[1] = 1.0f;
    context->current_color[2] = 1.0f;
    context->current_color[3] = 1.0f;
    context->light_model_ambient[0] = 0.2f;
    context->light_model_ambient[1] = 0.2f;
    context->light_model_ambient[2] = 0.2f;
    context->light_model_ambient[3] = 1.0f;
    context->front_material.ambient[0] = 0.2f;
    context->front_material.ambient[1] = 0.2f;
    context->front_material.ambient[2] = 0.2f;
    context->front_material.ambient[3] = 1.0f;
    context->front_material.diffuse[0] = 0.8f;
    context->front_material.diffuse[1] = 0.8f;
    context->front_material.diffuse[2] = 0.8f;
    context->front_material.diffuse[3] = 1.0f;
    context->front_material.specular[3] = 1.0f;
    context->front_material.emission[3] = 1.0f;
    context->back_material = context->front_material;
    for (uint32_t light = 0u; light < PSGL_MAX_LIGHTS; light++) {
        context->lights[light].ambient[3] = 1.0f;
        context->lights[light].position[2] = 1.0f;
        context->lights[light].spot_direction[2] = -1.0f;
        context->lights[light].spot_cutoff = 180.0f;
        context->lights[light].constant_attenuation = 1.0f;
        if (light == 0u) {
            context->lights[light].diffuse[0] = 1.0f;
            context->lights[light].diffuse[1] = 1.0f;
            context->lights[light].diffuse[2] = 1.0f;
            context->lights[light].diffuse[3] = 1.0f;
            context->lights[light].specular[0] = 1.0f;
            context->lights[light].specular[1] = 1.0f;
            context->lights[light].specular[2] = 1.0f;
            context->lights[light].specular[3] = 1.0f;
        } else {
            context->lights[light].diffuse[3] = 1.0f;
            context->lights[light].specular[3] = 1.0f;
        }
    }
    context->active_texture = GL_TEXTURE0;
    context->client_active_texture = GL_TEXTURE0;
    context->unpack_alignment = 4;
    context->attribs[PSGL_ATTRIB_VERTEX].size = 4;
    context->attribs[PSGL_ATTRIB_NORMAL].size = 3;
    context->attribs[PSGL_ATTRIB_COLOR].size = 4;
    context->attribs[PSGL_ATTRIB_TEXCOORD].size = 4;
    context->fog_enabled = GL_FALSE;
    context->fog_mode = GL_EXP;
    context->fog_density = 1.0f;
    context->fog_start = 0.0f;
    context->fog_end = 1.0f;
    context->fog_color[0] = 0.0f;
    context->fog_color[1] = 0.0f;
    context->fog_color[2] = 0.0f;
    context->fog_color[3] = 0.0f;
    context->dirty = PSGL_DIRTY_ALL;
    return context;
}

void psgl_context_destroy(PSGLcontext *context)
{
    if (!context) return;
    if (g_psgl.current_context == context) psgl_context_unbind_current();
}

PSGLdevice *psgl_device_create(const PSGLdeviceParameters *parameters)
{
    uint32_t samples_x;
    uint32_t samples_y;
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
        if (psgl_msaa_mode_valid(parameters->multisamplingMode))
            device->multisampling_mode = parameters->multisamplingMode;
    }
    if (!device->multisampling_mode)
        device->multisampling_mode = GL_MULTISAMPLING_NONE_SCE;
    samples_x = psgl_msaa_samples_x(device->multisampling_mode);
    samples_y = psgl_msaa_samples_y(device->multisampling_mode);
    device->storage_height = (uint16_t)((uint32_t)device->render_height * samples_y);
    device->pitch = psgl_align_u32((uint32_t)device->render_width *
                                   samples_x * sizeof(uint32_t), 64u);
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
        device->frames[i].width = device->render_width;
        device->frames[i].height = device->render_height;
        device->frames[i].id = (uint8_t)i;
        if (!psgl_make_frame_buffer(device, i)) {
            psgl_device_destroy(device);
            return NULL;
        }
    }

    uint32_t depth_size = device->depth_pitch * (uint32_t)device->storage_height;
    device->depth_address = (uint32_t *)rsxMemalign(64u, depth_size);
    if (!device->depth_address ||
        cellGcmAddressToOffset(device->depth_address, &device->depth_offset) != 0) {
        psgl_device_destroy(device);
        return NULL;
    }

    device->frame_on_display = 0u;
    device->flip_target_frame = 0u;
    device->flip_pending = 0u;
    device->current_frame = (device->frame_on_display + 1u) % device->frame_count;
    psgl_init_flip_labels(device);
    cellGcmSetVBlankHandler(psgl_vblank_trampoline);
    cellGcmSetFlipHandler(psgl_flip_trampoline);
    device->initialized = 1u;
    return device;
}

void psgl_device_destroy(PSGLdevice *device)
{
    if (!device) return;
    if (g_psgl.current_device == device) psgl_context_unbind_current();
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
        device->frames[i].scanout_address = NULL;
    }
    device->depth_address = NULL;
}

void psgl_context_make_current(PSGLcontext *context, PSGLdevice *device)
{
    if (!context) {
        psgl_context_unbind_current();
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
    context->dirty |= PSGL_DIRTY_FRAMEBUFFER | PSGL_DIRTY_VIEWPORT |
                      PSGL_DIRTY_SCISSOR;
    psgl_bind_render_target(context);
    psgl_emit_viewport(context);
    psgl_emit_scissor(context);
}

void psgl_context_unbind_current(void)
{
    if (g_psgl.current_context) g_psgl.current_context->device = NULL;
    g_psgl.current_context = NULL;
    g_psgl.current_device = NULL;
}

void psgl_context_reset_current(void)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    context->dirty |= PSGL_DIRTY_ALL;
}

PSGLcontext *psgl_context_current(void)
{
    return g_psgl.current_context;
}

PSGLdevice *psgl_context_current_device(void)
{
    return g_psgl.current_device;
}

void psgl_context_load_shader_library(const char *filename)
{
    s32 fd = -1;
    u64 got = 0u;
    unsigned char header[16];
    uint32_t count;
    if (!filename) return;
    if (cellFsOpen(filename, PSGL_CELL_FS_O_RDONLY, &fd, NULL, 0) != 0)
        return;
    if (cellFsRead(fd, header, sizeof(header), &got) != 0 ||
        got != sizeof(header) ||
        !psgl_bytes_equal(header, "PSGLSHDR", 8u) ||
        psgl_read_be32(header + 8u) != PSGL_SHADER_LIBRARY_VERSION) {
        cellFsClose(fd);
        return;
    }

    count = psgl_read_be32(header + 12u);
    for (uint32_t i = 0u; i < count; i++) {
        unsigned char rec[12];
        uint32_t mask;
        uint32_t vp_size;
        uint32_t fp_size;
        unsigned char *vp_blob;
        unsigned char *fp_blob;
        if (cellFsRead(fd, rec, sizeof(rec), &got) != 0 ||
            got != sizeof(rec))
            break;
        mask = psgl_read_be32(rec);
        vp_size = psgl_read_be32(rec + 4u);
        fp_size = psgl_read_be32(rec + 8u);
        if (!vp_size || !fp_size || vp_size > (1024u * 1024u) ||
            fp_size > (1024u * 1024u))
            break;
        vp_blob = (unsigned char *)malloc(vp_size);
        fp_blob = (unsigned char *)malloc(fp_size);
        if (!vp_blob || !fp_blob) {
            if (vp_blob) free(vp_blob);
            if (fp_blob) free(fp_blob);
            break;
        }
        if (cellFsRead(fd, vp_blob, vp_size, &got) != 0 || got != vp_size ||
            cellFsRead(fd, fp_blob, fp_size, &got) != 0 || got != fp_size) {
            free(vp_blob);
            free(fp_blob);
            break;
        }
        if (mask == PSGL_FFP_MINIMAL_MASK &&
            psgl_ffp_init_pair(&g_psgl_ffp_library_vp,
                               &g_psgl_ffp_library_fp,
                               g_psgl_ffp_library_vp_params,
                               vp_blob, vp_size, fp_blob, fp_size)) {
            g_psgl_ffp_library_valid = 1u;
        } else {
            free(vp_blob);
            free(fp_blob);
        }
    }
    cellFsClose(fd);
    if (g_psgl.current_context)
        g_psgl.current_context->dirty |= PSGL_DIRTY_CG;
}

void psgl_context_bind_cg_program(CGprogram program, CGprofile profile)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    if (profile == CG_PROFILE_SCE_VP_RSX ||
        profile == CG_PROFILE_VPRSX ||
        profile == CG_PROFILE_VP40 ||
        profile == CG_PROFILE_ARBVP1 ||
        profile == CG_PROFILE_VP30 ||
        profile == CG_PROFILE_VP20) {
        context->bound_vertex_program = program;
        context->dirty |= PSGL_DIRTY_CG;
    } else if (profile == CG_PROFILE_SCE_FP_RSX ||
               profile == CG_PROFILE_FPRSX ||
               profile == CG_PROFILE_FP40 ||
               profile == CG_PROFILE_ARBFP1 ||
               profile == CG_PROFILE_FP30 ||
               profile == CG_PROFILE_FP20) {
        context->bound_fragment_program = program;
        context->dirty |= PSGL_DIRTY_CG;
    }
}

void psgl_context_swap(void)
{
    PSGLcontext *context = g_psgl.current_context;
    PSGLdevice *device = g_psgl.current_device;
    int32_t queue_id;
    if (!context || !device || !context->gcm) return;

    psgl_resolve_frame_for_display(context);

    queue_id = (int32_t)cellGcmSetPrepareFlip(context->gcm,
                                              (uint8_t)device->current_frame);
    while (queue_id < 0) {
        sys_timer_usleep(1000);
        queue_id = (int32_t)cellGcmSetPrepareFlip(context->gcm,
                                                  (uint8_t)device->current_frame);
    }

    cellGcmSetWriteBackEndLabel(context->gcm, PSGL_LABEL_PREPARED_BUFFER,
                                (device->current_frame << 8) |
                                ((uint32_t)queue_id & 0x7u));
    cellGcmFlush(context->gcm);
    while (psgl_ppu_too_far_ahead(device))
        sys_timer_usleep(3000);

    g_psgl.last_flip_time = psglGetSystemTime();
    device->submitted_flips++;
    device->current_frame = (device->current_frame + 1u) % device->frame_count;
    cellGcmSetWaitLabel(context->gcm,
                        (uint8_t)(PSGL_LABEL_BUFFER_STATUS +
                                  device->current_frame),
                        PSGL_BUFFER_IDLE);
    cellGcmSetWriteCommandLabel(context->gcm,
                                (uint8_t)(PSGL_LABEL_BUFFER_STATUS +
                                          device->current_frame),
                                PSGL_BUFFER_BUSY);
    context->dirty |= PSGL_DIRTY_FRAMEBUFFER;
    psgl_bind_render_target(context);
    psgl_emit_viewport(context);
    context->dirty |= PSGL_DIRTY_CG | PSGL_DIRTY_TEXTURES;
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

void psgl_context_matrix_mode(GLenum mode)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE)
        return;
    context->matrix_mode = mode;
}

void psgl_context_load_identity(void)
{
    PSGLcontext *context = g_psgl.current_context;
    GLfloat *matrix = psgl_current_matrix(context);
    if (!matrix) return;
    psgl_matrix_identity(matrix);
    psgl_matrix_mark_dirty(context);
}

void psgl_context_push_matrix(void)
{
    PSGLcontext *context = g_psgl.current_context;
    GLfloat *matrix = psgl_current_matrix(context);
    uint32_t unit;
    if (!context || !matrix) return;
    switch (context->matrix_mode) {
    case GL_MODELVIEW:
        if (context->modelview_stack_depth >= PSGL_MODELVIEW_STACK_DEPTH)
            return;
        psgl_copy(context->modelview_stack[context->modelview_stack_depth],
                  matrix, sizeof(GLfloat) * 16u);
        context->modelview_stack_depth++;
        break;
    case GL_PROJECTION:
        if (context->projection_stack_depth >= PSGL_PROJECTION_STACK_DEPTH)
            return;
        psgl_copy(context->projection_stack[context->projection_stack_depth],
                  matrix, sizeof(GLfloat) * 16u);
        context->projection_stack_depth++;
        break;
    case GL_TEXTURE:
        unit = psgl_texture_unit_index(context->active_texture);
        if (unit >= PSGL_MAX_TEXTURE_UNITS) return;
        if (context->texture_stack_depth[unit] >= PSGL_TEXTURE_STACK_DEPTH)
            return;
        psgl_copy(context->texture_stack[unit][context->texture_stack_depth[unit]],
                  matrix, sizeof(GLfloat) * 16u);
        context->texture_stack_depth[unit]++;
        break;
    default:
        break;
    }
}

void psgl_context_pop_matrix(void)
{
    PSGLcontext *context = g_psgl.current_context;
    GLfloat *matrix = psgl_current_matrix(context);
    uint32_t unit;
    if (!context || !matrix) return;
    switch (context->matrix_mode) {
    case GL_MODELVIEW:
        if (context->modelview_stack_depth == 0u) return;
        context->modelview_stack_depth--;
        psgl_copy(matrix, context->modelview_stack[context->modelview_stack_depth],
                  sizeof(GLfloat) * 16u);
        break;
    case GL_PROJECTION:
        if (context->projection_stack_depth == 0u) return;
        context->projection_stack_depth--;
        psgl_copy(matrix,
                  context->projection_stack[context->projection_stack_depth],
                  sizeof(GLfloat) * 16u);
        break;
    case GL_TEXTURE:
        unit = psgl_texture_unit_index(context->active_texture);
        if (unit >= PSGL_MAX_TEXTURE_UNITS) return;
        if (context->texture_stack_depth[unit] == 0u) return;
        context->texture_stack_depth[unit]--;
        psgl_copy(matrix,
                  context->texture_stack[unit][context->texture_stack_depth[unit]],
                  sizeof(GLfloat) * 16u);
        break;
    default:
        return;
    }
    psgl_matrix_mark_dirty(context);
}

void psgl_context_load_matrix(const GLfloat *matrix)
{
    PSGLcontext *context = g_psgl.current_context;
    GLfloat *dst = psgl_current_matrix(context);
    if (!dst || !matrix) return;
    psgl_matrix_transpose(dst, matrix);
    psgl_matrix_mark_dirty(context);
}

static void psgl_context_mult_matrix_raw(const GLfloat *matrix)
{
    PSGLcontext *context = g_psgl.current_context;
    GLfloat *dst = psgl_current_matrix(context);
    if (!dst || !matrix) return;
    psgl_matrix_multiply(dst, dst, matrix);
    psgl_matrix_mark_dirty(context);
}

void psgl_context_mult_matrix(const GLfloat *matrix)
{
    GLfloat temp[16];
    if (!matrix) return;
    psgl_matrix_transpose(temp, matrix);
    psgl_context_mult_matrix_raw(temp);
}

void psgl_context_rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat matrix[16];
    GLfloat radians;
    GLfloat length = sqrtf(x * x + y * y + z * z);
    GLfloat c;
    GLfloat s;
    GLfloat ic;
    if (length == 0.0f) return;
    x /= length;
    y /= length;
    z /= length;
    radians = angle * 0.01745329251994329577f;
    c = cosf(radians);
    s = sinf(radians);
    ic = 1.0f - c;
    psgl_matrix_identity(matrix);
    matrix[0] = x * x * ic + c;
    matrix[1] = x * y * ic - z * s;
    matrix[2] = x * z * ic + y * s;
    matrix[4] = y * x * ic + z * s;
    matrix[5] = y * y * ic + c;
    matrix[6] = y * z * ic - x * s;
    matrix[8] = z * x * ic - y * s;
    matrix[9] = z * y * ic + x * s;
    matrix[10] = z * z * ic + c;
    psgl_context_mult_matrix_raw(matrix);
}

void psgl_context_scalef(GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat matrix[16];
    psgl_matrix_identity(matrix);
    matrix[0] = x;
    matrix[5] = y;
    matrix[10] = z;
    psgl_context_mult_matrix_raw(matrix);
}

void psgl_context_translatef(GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat matrix[16];
    psgl_matrix_identity(matrix);
    matrix[3] = x;
    matrix[7] = y;
    matrix[11] = z;
    psgl_context_mult_matrix_raw(matrix);
}

void psgl_context_frustumf(GLfloat left, GLfloat right, GLfloat bottom,
                           GLfloat top, GLfloat znear, GLfloat zfar)
{
    GLfloat matrix[16];
    GLfloat rl = right - left;
    GLfloat tb = top - bottom;
    GLfloat fn = zfar - znear;
    if (rl == 0.0f || tb == 0.0f || fn == 0.0f || znear <= 0.0f || zfar <= 0.0f)
        return;
    psgl_zero(matrix, sizeof(matrix));
    matrix[0] = (2.0f * znear) / rl;
    matrix[2] = (right + left) / rl;
    matrix[5] = (2.0f * znear) / tb;
    matrix[6] = (top + bottom) / tb;
    matrix[10] = -(zfar + znear) / fn;
    matrix[11] = -(2.0f * zfar * znear) / fn;
    matrix[14] = -1.0f;
    psgl_context_mult_matrix_raw(matrix);
}

void psgl_context_orthof(GLfloat left, GLfloat right, GLfloat bottom,
                         GLfloat top, GLfloat znear, GLfloat zfar)
{
    GLfloat matrix[16];
    GLfloat rl = right - left;
    GLfloat tb = top - bottom;
    GLfloat fn = zfar - znear;
    if (rl == 0.0f || tb == 0.0f || fn == 0.0f) return;
    psgl_matrix_identity(matrix);
    matrix[0] = 2.0f / rl;
    matrix[5] = 2.0f / tb;
    matrix[10] = -2.0f / fn;
    matrix[3] = -(right + left) / rl;
    matrix[7] = -(top + bottom) / tb;
    matrix[11] = -(zfar + znear) / fn;
    psgl_context_mult_matrix_raw(matrix);
}

void psgl_context_set_enable(GLenum cap, GLboolean enabled)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    enabled = enabled ? GL_TRUE : GL_FALSE;
    switch (cap) {
    case GL_BLEND:
        context->blend_enabled = enabled;
        context->dirty |= PSGL_DIRTY_BLEND;
        break;
    case GL_DEPTH_TEST:
        context->depth_test_enabled = enabled;
        context->dirty |= PSGL_DIRTY_DEPTH;
        break;
    case GL_STENCIL_TEST:
        context->stencil_test_enabled = enabled;
        context->dirty |= PSGL_DIRTY_STENCIL;
        break;
    case GL_ALPHA_TEST:
        context->alpha_test_enabled = enabled;
        context->dirty |= PSGL_DIRTY_ALPHA;
        break;
    case GL_SCISSOR_TEST:
        context->scissor_test_enabled = enabled;
        context->dirty |= PSGL_DIRTY_SCISSOR;
        break;
    case GL_CULL_FACE:
        context->cull_face_enabled = enabled;
        context->dirty |= PSGL_DIRTY_RASTER;
        break;
    case GL_DITHER:
        context->dither_enabled = enabled;
        context->dirty |= PSGL_DIRTY_RASTER;
        break;
    case GL_COLOR_LOGIC_OP:
        context->logic_op_enabled = enabled;
        context->dirty |= PSGL_DIRTY_RASTER;
        break;
    case GL_MULTISAMPLE:
        context->multisample_enabled = enabled;
        context->dirty |= PSGL_DIRTY_FRAMEBUFFER | PSGL_DIRTY_CG;
        break;
    case GL_LIGHTING:
        context->lighting_enabled = enabled;
        psgl_mark_lighting_dirty(context);
        break;
    case GL_COLOR_MATERIAL:
        context->color_material_enabled = enabled;
        psgl_mark_lighting_dirty(context);
        break;
    case GL_FOG:
        context->fog_enabled = enabled;
        context->dirty |= PSGL_DIRTY_FOG | PSGL_DIRTY_CG;
        break;
    case GL_TEXTURE_2D: {
        uint32_t unit = psgl_texture_unit_index(context->active_texture);
        context->textures[unit].texture_2d_enabled = enabled;
        context->dirty |= PSGL_DIRTY_TEXTURES | PSGL_DIRTY_CG;
        break;
    }
    default:
        if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + PSGL_MAX_LIGHTS) {
            context->lights[cap - GL_LIGHT0].enabled = enabled;
            psgl_mark_lighting_dirty(context);
        }
        break;
    }
}

void psgl_context_set_alpha_func(GLenum func, GLclampf ref)
{
    uint32_t translated;
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !psgl_translate_compare(func, &translated)) return;
    context->alpha_func = func;
    context->alpha_ref = psgl_clampf(ref, 0.0f, 1.0f);
    context->dirty |= PSGL_DIRTY_ALPHA;
}

void psgl_context_set_blend_func(GLenum sfactor, GLenum dfactor)
{
    psgl_context_set_blend_func_separate(sfactor, dfactor, sfactor, dfactor);
}

void psgl_context_set_blend_func_separate(GLenum sfactor_rgb, GLenum dfactor_rgb,
                                          GLenum sfactor_alpha,
                                          GLenum dfactor_alpha)
{
    uint16_t translated;
    PSGLcontext *context = g_psgl.current_context;
    if (!context ||
        !psgl_translate_blend_factor(sfactor_rgb, &translated) ||
        !psgl_translate_blend_factor(dfactor_rgb, &translated) ||
        !psgl_translate_blend_factor(sfactor_alpha, &translated) ||
        !psgl_translate_blend_factor(dfactor_alpha, &translated))
        return;
    context->blend_src_rgb = sfactor_rgb;
    context->blend_dst_rgb = dfactor_rgb;
    context->blend_src_alpha = sfactor_alpha;
    context->blend_dst_alpha = dfactor_alpha;
    context->dirty |= PSGL_DIRTY_BLEND;
}

void psgl_context_set_blend_equation(GLenum mode)
{
    psgl_context_set_blend_equation_separate(mode, mode);
}

void psgl_context_set_blend_equation_separate(GLenum mode_rgb,
                                              GLenum mode_alpha)
{
    uint16_t translated;
    PSGLcontext *context = g_psgl.current_context;
    if (!context ||
        !psgl_translate_blend_equation(mode_rgb, &translated) ||
        !psgl_translate_blend_equation(mode_alpha, &translated))
        return;
    context->blend_equation_rgb = mode_rgb;
    context->blend_equation_alpha = mode_alpha;
    context->dirty |= PSGL_DIRTY_BLEND;
}

void psgl_context_set_scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || width < 0 || height < 0) return;
    context->scissor[0] = x < 0 ? 0 : x;
    context->scissor[1] = y < 0 ? 0 : y;
    context->scissor[2] = width;
    context->scissor[3] = height;
    context->dirty |= PSGL_DIRTY_SCISSOR;
}

void psgl_context_set_depth_func(GLenum func)
{
    uint32_t translated;
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !psgl_translate_compare(func, &translated)) return;
    context->depth_func = func;
    context->dirty |= PSGL_DIRTY_DEPTH;
}

void psgl_context_set_depth_mask(GLboolean flag)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    context->depth_mask = flag ? GL_TRUE : GL_FALSE;
    context->dirty |= PSGL_DIRTY_DEPTH;
}

void psgl_context_set_stencil_func(GLenum func, GLint ref, GLuint mask)
{
    uint32_t translated;
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !psgl_translate_compare(func, &translated)) return;
    context->stencil_func = func;
    context->stencil_ref = ref;
    context->stencil_value_mask = mask;
    context->dirty |= PSGL_DIRTY_STENCIL;
}

void psgl_context_set_stencil_mask(GLuint mask)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    context->stencil_write_mask = mask;
    context->dirty |= PSGL_DIRTY_STENCIL;
}

void psgl_context_set_stencil_op(GLenum fail, GLenum zfail, GLenum zpass)
{
    uint32_t translated;
    PSGLcontext *context = g_psgl.current_context;
    if (!context ||
        !psgl_translate_stencil_op(fail, &translated) ||
        !psgl_translate_stencil_op(zfail, &translated) ||
        !psgl_translate_stencil_op(zpass, &translated))
        return;
    context->stencil_fail = fail;
    context->stencil_depth_fail = zfail;
    context->stencil_depth_pass = zpass;
    context->dirty |= PSGL_DIRTY_STENCIL;
}

void psgl_context_set_cull_face(GLenum mode)
{
    uint32_t translated;
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !psgl_translate_cull_face(mode, &translated)) return;
    context->cull_face = mode;
    context->dirty |= PSGL_DIRTY_RASTER;
}

void psgl_context_set_front_face(GLenum mode)
{
    uint32_t translated;
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !psgl_translate_front_face(mode, &translated)) return;
    context->front_face = mode;
    context->dirty |= PSGL_DIRTY_RASTER;
}

void psgl_context_set_color_mask(GLboolean red, GLboolean green,
                                 GLboolean blue, GLboolean alpha)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    context->color_mask[0] = red ? GL_TRUE : GL_FALSE;
    context->color_mask[1] = green ? GL_TRUE : GL_FALSE;
    context->color_mask[2] = blue ? GL_TRUE : GL_FALSE;
    context->color_mask[3] = alpha ? GL_TRUE : GL_FALSE;
    context->dirty |= PSGL_DIRTY_RASTER;
}

void psgl_context_set_logic_op(GLenum opcode)
{
    uint32_t translated;
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !psgl_translate_logic_op(opcode, &translated)) return;
    context->logic_op = opcode;
    context->dirty |= PSGL_DIRTY_RASTER;
}

void psgl_context_set_shade_model(GLenum mode)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || (mode != GL_FLAT && mode != GL_SMOOTH)) return;
    context->shade_model = mode;
    psgl_mark_lighting_dirty(context);
}

static void psgl_apply_color_material(PSGLcontext *context,
                                      PSGLmaterialState *material)
{
    if (!context || !material) return;
    switch (context->color_material_parameter) {
    case GL_AMBIENT:
        psgl_set_float4(material->ambient, context->current_color);
        break;
    case GL_DIFFUSE:
        psgl_set_float4(material->diffuse, context->current_color);
        break;
    case GL_AMBIENT_AND_DIFFUSE:
        psgl_set_float4(material->ambient, context->current_color);
        psgl_set_float4(material->diffuse, context->current_color);
        break;
    case GL_SPECULAR:
        psgl_set_float4(material->specular, context->current_color);
        break;
    case GL_EMISSION:
        psgl_set_float4(material->emission, context->current_color);
        break;
    default:
        break;
    }
}

void psgl_context_framebuffer_parameter(GLenum target, GLenum pname,
                                        GLint param)
{
    PSGLcontext *context = g_psgl.current_context;
    PSGLdevice *device;
    GLenum mode = (GLenum)param;
    uint32_t samples_x;
    uint32_t samples_y;
    uint32_t needed_pitch;
    if (!context || (target != GL_FRAMEBUFFER_OES && target != 0))
        return;
    if (pname != GL_FRAMEBUFFER_MULTISAMPLING_MODE_SCE ||
        !psgl_msaa_mode_valid(mode))
        return;
    device = context->device;
    if (!device || device->multisampling_mode == mode) return;
    if (!mode) mode = GL_MULTISAMPLING_NONE_SCE;
    samples_x = psgl_msaa_samples_x(mode);
    samples_y = psgl_msaa_samples_y(mode);
    needed_pitch = psgl_align_u32((uint32_t)device->render_width *
                                  samples_x * sizeof(uint32_t), 64u);
    if (needed_pitch > device->pitch ||
        (uint32_t)device->render_height * samples_y > device->storage_height)
        return;
    device->multisampling_mode = mode;
    context->dirty |= PSGL_DIRTY_FRAMEBUFFER | PSGL_DIRTY_CG;
}

void psgl_context_set_current_color(GLfloat red, GLfloat green,
                                    GLfloat blue, GLfloat alpha)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    context->current_color[0] = red;
    context->current_color[1] = green;
    context->current_color[2] = blue;
    context->current_color[3] = alpha;
    if (context->color_material_enabled) {
        if (context->color_material_face == GL_FRONT ||
            context->color_material_face == GL_FRONT_AND_BACK)
            psgl_apply_color_material(context, &context->front_material);
        if (context->color_material_face == GL_BACK ||
            context->color_material_face == GL_FRONT_AND_BACK)
            psgl_apply_color_material(context, &context->back_material);
        psgl_mark_lighting_dirty(context);
    }
}

void psgl_context_set_light_fv(GLenum light, GLenum pname,
                               const GLfloat *params)
{
    uint32_t index = psgl_light_index(light);
    PSGLcontext *context = g_psgl.current_context;
    PSGLlightState *state;
    GLfloat transformed[4];
    if (!context || !params || index >= PSGL_MAX_LIGHTS) return;
    state = &context->lights[index];
    switch (pname) {
    case GL_AMBIENT: psgl_set_float4(state->ambient, params); break;
    case GL_DIFFUSE: psgl_set_float4(state->diffuse, params); break;
    case GL_SPECULAR: psgl_set_float4(state->specular, params); break;
    case GL_POSITION:
        psgl_matrix_transform4(context->modelview, params, transformed);
        psgl_set_float4(state->position, transformed);
        break;
    case GL_SPOT_DIRECTION:
        transformed[0] = params[0];
        transformed[1] = params[1];
        transformed[2] = params[2];
        transformed[3] = 0.0f;
        psgl_matrix_transform4(context->modelview, transformed, transformed);
        psgl_set_float3(state->spot_direction, transformed);
        break;
    case GL_SPOT_EXPONENT: state->spot_exponent = params[0]; break;
    case GL_SPOT_CUTOFF: state->spot_cutoff = params[0]; break;
    case GL_CONSTANT_ATTENUATION: state->constant_attenuation = params[0]; break;
    case GL_LINEAR_ATTENUATION: state->linear_attenuation = params[0]; break;
    case GL_QUADRATIC_ATTENUATION: state->quadratic_attenuation = params[0]; break;
    default: return;
    }
    psgl_mark_lighting_dirty(context);
}

void psgl_context_set_light_f(GLenum light, GLenum pname, GLfloat param)
{
    GLfloat value[4] = { param, param, param, param };
    psgl_context_set_light_fv(light, pname, value);
}

void psgl_context_set_light_model_fv(GLenum pname, const GLfloat *params)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !params) return;
    switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT:
        psgl_set_float4(context->light_model_ambient, params);
        break;
    case GL_LIGHT_MODEL_TWO_SIDE:
        context->light_model_two_side = params[0] != 0.0f ? GL_TRUE : GL_FALSE;
        break;
    default:
        return;
    }
    psgl_mark_lighting_dirty(context);
}

void psgl_context_set_light_model_f(GLenum pname, GLfloat param)
{
    GLfloat value[4] = { param, param, param, param };
    psgl_context_set_light_model_fv(pname, value);
}

static void psgl_set_material_member(PSGLmaterialState *material,
                                     GLenum pname, const GLfloat *params)
{
    if (!material || !params) return;
    switch (pname) {
    case GL_AMBIENT: psgl_set_float4(material->ambient, params); break;
    case GL_DIFFUSE: psgl_set_float4(material->diffuse, params); break;
    case GL_AMBIENT_AND_DIFFUSE:
        psgl_set_float4(material->ambient, params);
        psgl_set_float4(material->diffuse, params);
        break;
    case GL_SPECULAR: psgl_set_float4(material->specular, params); break;
    case GL_EMISSION: psgl_set_float4(material->emission, params); break;
    case GL_SHININESS: material->shininess = params[0]; break;
    default: break;
    }
}

void psgl_context_set_material_fv(GLenum face, GLenum pname,
                                  const GLfloat *params)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !params) return;
    switch (pname) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_AMBIENT_AND_DIFFUSE:
    case GL_SPECULAR:
    case GL_EMISSION:
    case GL_SHININESS:
        break;
    default:
        return;
    }
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK)
        psgl_set_material_member(&context->front_material, pname, params);
    if (face == GL_BACK || face == GL_FRONT_AND_BACK)
        psgl_set_material_member(&context->back_material, pname, params);
    if (face == GL_FRONT || face == GL_BACK || face == GL_FRONT_AND_BACK)
        psgl_mark_lighting_dirty(context);
}

void psgl_context_set_material_f(GLenum face, GLenum pname, GLfloat param)
{
    GLfloat value[4] = { param, param, param, param };
    psgl_context_set_material_fv(face, pname, value);
}

void psgl_context_set_color_material(GLenum face, GLenum mode)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK)
        return;
    switch (mode) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_AMBIENT_AND_DIFFUSE:
    case GL_SPECULAR:
    case GL_EMISSION:
        context->color_material_face = face;
        context->color_material_parameter = mode;
        psgl_mark_lighting_dirty(context);
        break;
    default:
        break;
    }
}

void psgl_context_get_booleanv(GLenum pname, GLboolean *params)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !params) return;
    switch (pname) {
    case GL_LIGHTING: *params = context->lighting_enabled; break;
    case GL_COLOR_MATERIAL: *params = context->color_material_enabled; break;
    case GL_FOG: *params = context->fog_enabled; break;
    case GL_MULTISAMPLE: *params = context->multisample_enabled; break;
    default:
        if (pname >= GL_LIGHT0 && pname < GL_LIGHT0 + PSGL_MAX_LIGHTS)
            *params = context->lights[pname - GL_LIGHT0].enabled;
        else
            *params = GL_FALSE;
        break;
    }
}

void psgl_context_get_floatv(GLenum pname, GLfloat *params)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !params) return;
    switch (pname) {
    case GL_CURRENT_COLOR:
        psgl_get_float4(context->current_color, params);
        break;
    case GL_LIGHT_MODEL_AMBIENT:
        psgl_get_float4(context->light_model_ambient, params);
        break;
    case GL_LIGHT_MODEL_TWO_SIDE:
        params[0] = context->light_model_two_side ? 1.0f : 0.0f;
        break;
    case GL_SHADE_MODEL:
        params[0] = (GLfloat)context->shade_model;
        break;
    case GL_FOG_DENSITY: params[0] = context->fog_density; break;
    case GL_FOG_START:   params[0] = context->fog_start; break;
    case GL_FOG_END:     params[0] = context->fog_end; break;
    case GL_FOG_COLOR:
        psgl_get_float4(context->fog_color, params);
        break;
    case GL_MODELVIEW_MATRIX:
        psgl_matrix_transpose(params, context->modelview);
        break;
    case GL_PROJECTION_MATRIX:
        psgl_matrix_transpose(params, context->projection);
        break;
    case GL_TEXTURE_MATRIX: {
        uint32_t unit = psgl_texture_unit_index(context->active_texture);
        if (unit < PSGL_MAX_TEXTURE_UNITS)
            psgl_matrix_transpose(params, context->texture_matrix[unit]);
        break;
    }
    default:
        params[0] = 0.0f;
        break;
    }
}

void psgl_context_get_lightfv(GLenum light, GLenum pname, GLfloat *params)
{
    uint32_t index = psgl_light_index(light);
    PSGLcontext *context = g_psgl.current_context;
    PSGLlightState *state;
    if (!context || !params || index >= PSGL_MAX_LIGHTS) return;
    state = &context->lights[index];
    switch (pname) {
    case GL_AMBIENT: psgl_get_float4(state->ambient, params); break;
    case GL_DIFFUSE: psgl_get_float4(state->diffuse, params); break;
    case GL_SPECULAR: psgl_get_float4(state->specular, params); break;
    case GL_POSITION: psgl_get_float4(state->position, params); break;
    case GL_SPOT_DIRECTION: psgl_get_float3(state->spot_direction, params); break;
    case GL_SPOT_EXPONENT: params[0] = state->spot_exponent; break;
    case GL_SPOT_CUTOFF: params[0] = state->spot_cutoff; break;
    case GL_CONSTANT_ATTENUATION: params[0] = state->constant_attenuation; break;
    case GL_LINEAR_ATTENUATION: params[0] = state->linear_attenuation; break;
    case GL_QUADRATIC_ATTENUATION: params[0] = state->quadratic_attenuation; break;
    default: break;
    }
}

static void psgl_get_material_member(const PSGLmaterialState *material,
                                     GLenum pname, GLfloat *params)
{
    if (!material || !params) return;
    switch (pname) {
    case GL_AMBIENT: psgl_get_float4(material->ambient, params); break;
    case GL_DIFFUSE: psgl_get_float4(material->diffuse, params); break;
    case GL_SPECULAR: psgl_get_float4(material->specular, params); break;
    case GL_EMISSION: psgl_get_float4(material->emission, params); break;
    case GL_SHININESS: params[0] = material->shininess; break;
    default: break;
    }
}

void psgl_context_get_materialfv(GLenum face, GLenum pname, GLfloat *params)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !params) return;
    if (face == GL_BACK)
        psgl_get_material_member(&context->back_material, pname, params);
    else
        psgl_get_material_member(&context->front_material, pname, params);
}

void psgl_context_set_pixel_store(GLenum pname, GLint param)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || pname != GL_UNPACK_ALIGNMENT) return;
    if (param != 1 && param != 2 && param != 4 && param != 8) return;
    context->unpack_alignment = param;
}

void psgl_context_active_texture(GLenum texture)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    if (psgl_texture_unit_index(texture) >= PSGL_MAX_TEXTURE_UNITS) return;
    context->active_texture = texture;
    context->dirty |= PSGL_DIRTY_TEXTURES;
}

void psgl_context_client_active_texture(GLenum texture)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    if (psgl_texture_unit_index(texture) >= PSGL_MAX_TEXTURE_UNITS) return;
    context->client_active_texture = texture;
}

void psgl_context_set_fog_f(GLenum pname, GLfloat param)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context) return;
    switch (pname) {
    case GL_FOG_MODE:
        if (param != (GLfloat)GL_LINEAR && param != (GLfloat)GL_EXP &&
            param != (GLfloat)GL_EXP2) return;
        context->fog_mode = (GLenum)param;
        break;
    case GL_FOG_DENSITY:
        context->fog_density = param;
        break;
    case GL_FOG_START:
        context->fog_start = param;
        break;
    case GL_FOG_END:
        context->fog_end = param;
        break;
    default: return;
    }
    context->dirty |= PSGL_DIRTY_FOG | PSGL_DIRTY_CG;
}

void psgl_context_set_fog_fv(GLenum pname, const GLfloat *params)
{
    PSGLcontext *context = g_psgl.current_context;
    if (!context || !params) return;
    switch (pname) {
    case GL_FOG_COLOR:
        context->fog_color[0] = params[0];
        context->fog_color[1] = params[1];
        context->fog_color[2] = params[2];
        context->fog_color[3] = params[3];
        break;
    default: return;
    }
    context->dirty |= PSGL_DIRTY_FOG | PSGL_DIRTY_CG;
}

void psgl_context_gen_textures(GLsizei n, GLuint *textures)
{
    if (n < 0 || !textures) return;
    for (GLsizei i = 0; i < n; i++) {
        GLuint name = g_psgl_next_texture_name++;
        if (name == 0u) name = g_psgl_next_texture_name++;
        textures[i] = psgl_create_texture(name) ? name : 0u;
    }
}

void psgl_context_delete_textures(GLsizei n, const GLuint *textures)
{
    PSGLcontext *context = g_psgl.current_context;
    if (n < 0 || !textures) return;
    for (GLsizei i = 0; i < n; i++) {
        GLuint name = textures[i];
        PSGLtextureObject *texture;
        if (name == 0u) continue;
        texture = psgl_find_texture(name);
        if (!texture) continue;
        if (context) {
            for (uint32_t unit = 0; unit < PSGL_MAX_TEXTURE_UNITS; unit++) {
                if (context->textures[unit].texture_2d == name)
                    context->textures[unit].texture_2d = 0u;
            }
            context->dirty |= PSGL_DIRTY_TEXTURES;
        }
        psgl_release_texture_storage(texture);
        texture->name = 0u;
    }
}

void psgl_context_bind_texture(GLenum target, GLuint texture)
{
    PSGLcontext *context = g_psgl.current_context;
    uint32_t unit;
    if (!context || target != GL_TEXTURE_2D) return;
    unit = psgl_texture_unit_index(context->active_texture);
    if (unit >= PSGL_MAX_TEXTURE_UNITS) return;
    if (texture != 0u && !psgl_create_texture(texture)) return;
    context->textures[unit].texture_2d = texture;
    context->dirty |= PSGL_DIRTY_TEXTURES;
}

void psgl_context_tex_image_2d(GLenum target, GLint level,
                               GLint internalformat,
                               GLsizei width, GLsizei height, GLint border,
                               GLenum format, GLenum type,
                               const GLvoid *pixels)
{
    PSGLcontext *context = g_psgl.current_context;
    PSGLtextureObject *texture = psgl_bound_texture(context, target);
    uint32_t pitch;
    uint32_t size;
    void *address;
    uint32_t offset = 0u;
    int swizzled;
    (void)internalformat;
    if (!context || !texture || level != 0 || border != 0 ||
        width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        !psgl_texture_supported(format, type))
        return;
    swizzled = texture->allocation_hint == GL_TEXTURE_SWIZZLED_GPU_SCE &&
               psgl_is_power_of_two((uint32_t)width) &&
               psgl_is_power_of_two((uint32_t)height);
    pitch = swizzled ? (uint32_t)width * 4u
                     : (uint32_t)psgl_align_u16((uint32_t)width * 4u, 64u);
    size = swizzled ? (uint32_t)width * (uint32_t)height * 4u
                    : pitch * (uint32_t)height;
    address = rsxMemalign(PSGL_TEXTURE_ALIGNMENT, size);
    if (!address) return;
    if (cellGcmAddressToOffset(address, &offset) != 0) return;
    psgl_release_texture_storage(texture);
    texture->address = address;
    texture->offset = offset;
    texture->size = size;
    texture->width = (uint16_t)width;
    texture->height = (uint16_t)height;
    texture->pitch = (uint16_t)pitch;
    texture->levels = 1u;
    texture->location = CELL_GCM_LOCATION_LOCAL;
    texture->linear = swizzled ? 0u : 1u;
    texture->rsx_format = CELL_GCM_TEXTURE_A8R8G8B8 |
        (swizzled ? CELL_GCM_TEXTURE_SZ : CELL_GCM_TEXTURE_LN);
    if (pixels)
        psgl_write_texture_image(texture, 0, 0, width, height,
                                 format, type, pixels,
                                 context->unpack_alignment);
    psgl_fill_gcm_texture(texture);
    context->dirty |= PSGL_DIRTY_TEXTURES;
}

void psgl_context_tex_sub_image_2d(GLenum target, GLint level,
                                   GLint xoffset, GLint yoffset,
                                   GLsizei width, GLsizei height,
                                   GLenum format, GLenum type,
                                   const GLvoid *pixels)
{
    PSGLcontext *context = g_psgl.current_context;
    PSGLtextureObject *texture = psgl_bound_texture(context, target);
    if (!context || !texture || !texture->address || !pixels || level != 0 ||
        xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
        xoffset + width > texture->width || yoffset + height > texture->height ||
        !psgl_texture_supported(format, type))
        return;
    psgl_write_texture_image(texture, xoffset, yoffset, width, height,
                             format, type, pixels, context->unpack_alignment);
    context->dirty |= PSGL_DIRTY_TEXTURES;
}

void psgl_context_tex_parameter(GLenum target, GLenum pname, GLint param)
{
    PSGLcontext *context = g_psgl.current_context;
    PSGLtextureObject *texture = psgl_bound_texture(context, target);
    if (!context || !texture) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (!psgl_translate_texture_filter((GLenum)param)) return;
        texture->min_filter = (GLenum)param;
        break;
    case GL_TEXTURE_MAG_FILTER:
        if (param != GL_NEAREST && param != GL_LINEAR) return;
        texture->mag_filter = (GLenum)param;
        break;
    case GL_TEXTURE_WRAP_S:
        if (!psgl_translate_texture_wrap((GLenum)param)) return;
        texture->wrap_s = (GLenum)param;
        break;
    case GL_TEXTURE_WRAP_T:
        if (!psgl_translate_texture_wrap((GLenum)param)) return;
        texture->wrap_t = (GLenum)param;
        break;
    case GL_TEXTURE_ALLOCATION_HINT_SCE:
        if (param != GL_TEXTURE_LINEAR_GPU_SCE &&
            param != GL_TEXTURE_SWIZZLED_GPU_SCE)
            return;
        texture->allocation_hint = (GLenum)param;
        break;
    default:
        return;
    }
    context->dirty |= PSGL_DIRTY_TEXTURES;
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
    } else if (target == GL_TEXTURE_REFERENCE_BUFFER_SCE) {
        context->bound_texture_reference_buffer = name;
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
        context->dirty |= PSGL_DIRTY_CG;
        break;
    case GL_NORMAL_ARRAY:
        context->attribs[PSGL_ATTRIB_NORMAL].enabled = enabled;
        context->dirty |= PSGL_DIRTY_CG;
        break;
    case GL_COLOR_ARRAY:
        context->attribs[PSGL_ATTRIB_COLOR].enabled = enabled;
        context->dirty |= PSGL_DIRTY_CG;
        break;
    case GL_TEXTURE_COORD_ARRAY:
        context->attribs[PSGL_ATTRIB_TEXCOORD].enabled = enabled;
        context->dirty |= PSGL_DIRTY_CG;
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
    context->dirty |= PSGL_DIRTY_CG;
}

void psgl_context_draw_arrays(GLenum mode, GLint first, GLsizei count)
{
    PSGLcontext *context = g_psgl.current_context;
    uint32_t primitive = psgl_gl_primitive(mode);
    if (!context || !context->gcm || first < 0 || count <= 0 || !primitive)
        return;
    psgl_validate_draw_state(context);
    cellGcmSetDrawArrays(context->gcm, primitive, (uint32_t)first,
                         (uint32_t)count);
}

void psgl_context_draw_elements(GLenum mode, GLsizei count, GLenum type,
                                const GLvoid *indices)
{
    PSGLcontext *context = g_psgl.current_context;
    PSGLbufferObject *buffer;
    uint32_t primitive = psgl_gl_primitive(mode);
    uint8_t index_type = psgl_gl_index_type(type);
    uint32_t offset;
    if (!context || !context->gcm || count <= 0 || !primitive ||
        index_type == 0xffu)
        return;
    buffer = psgl_find_buffer(context->bound_element_array_buffer);
    if (!buffer || !buffer->address) return;
    offset = buffer->offset + (uint32_t)(uintptr_t)indices;
    psgl_validate_draw_state(context);
    cellGcmSetDrawIndexArray(context->gcm, (uint8_t)primitive,
                             (uint32_t)count, index_type,
                             CELL_GCM_LOCATION_LOCAL, offset);
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
    cellGcmSetFlipHandler(psgl_flip_trampoline);
}

void psglSetVBlankHandler(void (*handler)(const GLuint head))
{
    g_psgl.vblank_handler = handler;
    cellGcmSetVBlankHandler(psgl_vblank_trampoline);
}
