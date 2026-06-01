#ifndef PS3DK_LIBPSGL_CONTEXT_H
#define PS3DK_LIBPSGL_CONTEXT_H

#include <PSGL/psgl.h>
#include <cell/gcm.h>
#include <cell/gcm/gcm_command_c.h>
#include <stdint.h>

#define PSGL_MAX_FRAME_BUFFERS 3u
#define PSGL_MAX_TEXTURE_UNITS 4u
#define PSGL_MAX_VERTEX_ATTRIBS 4u
#define PSGL_MAX_GENERIC_ATTRIBS 16u
#define PSGL_MAX_LIGHTS 8u
#define PSGL_MODELVIEW_STACK_DEPTH 16u
#define PSGL_PROJECTION_STACK_DEPTH 2u
#define PSGL_TEXTURE_STACK_DEPTH 2u

typedef enum PSGLvertexAttribSlot {
    PSGL_ATTRIB_VERTEX = 0,
    PSGL_ATTRIB_NORMAL = 1,
    PSGL_ATTRIB_COLOR = 2,
    PSGL_ATTRIB_TEXCOORD = 3
} PSGLvertexAttribSlot;

typedef enum PSGLdirtyBits {
    PSGL_DIRTY_VIEWPORT = 0x00000001u,
    PSGL_DIRTY_SCISSOR = 0x00000002u,
    PSGL_DIRTY_CLEAR = 0x00000004u,
    PSGL_DIRTY_BLEND = 0x00000008u,
    PSGL_DIRTY_DEPTH = 0x00000010u,
    PSGL_DIRTY_STENCIL = 0x00000020u,
    PSGL_DIRTY_ALPHA = 0x00000040u,
    PSGL_DIRTY_TEXTURES = 0x00000080u,
    PSGL_DIRTY_CG = 0x00000100u,
    PSGL_DIRTY_MATRICES = 0x00000200u,
    PSGL_DIRTY_FRAMEBUFFER = 0x00000400u,
    PSGL_DIRTY_RASTER = 0x00000800u,
    PSGL_DIRTY_LIGHTING = 0x00001000u,
    PSGL_DIRTY_FOG = 0x00002000u,
    PSGL_DIRTY_ALL = 0x00003fffu
} PSGLdirtyBits;

typedef struct PSGLframeBuffer {
    uint32_t *address;
    uint32_t offset;
    uint32_t pitch;
    uint32_t *scanout_address;
    uint32_t scanout_offset;
    uint32_t scanout_pitch;
    uint16_t width;
    uint16_t height;
    uint8_t id;
} PSGLframeBuffer;

typedef struct PSGLvertexAttribState {
    GLboolean enabled;
    GLint size;
    GLenum type;
    GLsizei stride;
    const GLvoid *pointer;
    GLuint buffer_name;
    uint32_t buffer_offset;
} PSGLvertexAttribState;

typedef struct PSGLtextureUnitState {
    GLuint texture_2d;
    GLboolean texture_2d_enabled;
} PSGLtextureUnitState;

typedef struct PSGLlightState {
    GLboolean enabled;
    GLfloat ambient[4];
    GLfloat diffuse[4];
    GLfloat specular[4];
    GLfloat position[4];
    GLfloat spot_direction[3];
    GLfloat spot_exponent;
    GLfloat spot_cutoff;
    GLfloat constant_attenuation;
    GLfloat linear_attenuation;
    GLfloat quadratic_attenuation;
} PSGLlightState;

typedef struct PSGLmaterialState {
    GLfloat ambient[4];
    GLfloat diffuse[4];
    GLfloat specular[4];
    GLfloat emission[4];
    GLfloat shininess;
} PSGLmaterialState;

struct PSGLdevice {
    uint32_t initialized;
    uint16_t width;
    uint16_t height;
    uint16_t render_width;
    uint16_t render_height;
    uint16_t storage_height;
    uint32_t color_format;
    uint32_t depth_format;
    uint32_t multisampling_mode;
    uint32_t pitch;
    uint32_t frame_count;
    uint32_t current_frame;
    uint32_t frame_on_display;
    uint32_t flip_target_frame;
    uint32_t flip_pending;
    uint32_t submitted_flips;
    float aspect_ratio;
    PSGLframeBuffer frames[PSGL_MAX_FRAME_BUFFERS];
    uint32_t *depth_address;
    uint32_t depth_offset;
    uint32_t depth_pitch;
};

struct PSGLcontext {
    PSGLdevice *device;
    CellGcmContextData *gcm;
    GLenum matrix_mode;
    GLfloat modelview[16];
    GLfloat projection[16];
    GLfloat texture_matrix[PSGL_MAX_TEXTURE_UNITS][16];
    GLfloat modelview_stack[PSGL_MODELVIEW_STACK_DEPTH][16];
    GLfloat projection_stack[PSGL_PROJECTION_STACK_DEPTH][16];
    GLfloat texture_stack[PSGL_MAX_TEXTURE_UNITS][PSGL_TEXTURE_STACK_DEPTH][16];
    uint32_t modelview_stack_depth;
    uint32_t projection_stack_depth;
    uint32_t texture_stack_depth[PSGL_MAX_TEXTURE_UNITS];
    GLint viewport[4];
    GLint scissor[4];
    GLboolean blend_enabled;
    GLboolean depth_test_enabled;
    GLboolean stencil_test_enabled;
    GLboolean alpha_test_enabled;
    GLboolean scissor_test_enabled;
    GLboolean cull_face_enabled;
    GLboolean dither_enabled;
    GLboolean logic_op_enabled;
    GLboolean multisample_enabled;
    GLboolean lighting_enabled;
    GLboolean color_material_enabled;
    GLboolean light_model_two_side;
    GLboolean fog_enabled;
    GLenum fog_mode;
    GLfloat fog_density;
    GLfloat fog_start;
    GLfloat fog_end;
    GLfloat fog_color[4];
    GLenum blend_src_rgb;
    GLenum blend_dst_rgb;
    GLenum blend_src_alpha;
    GLenum blend_dst_alpha;
    GLenum blend_equation_rgb;
    GLenum blend_equation_alpha;
    GLenum depth_func;
    GLboolean depth_mask;
    GLenum alpha_func;
    GLfloat alpha_ref;
    GLenum stencil_func;
    GLint stencil_ref;
    GLuint stencil_value_mask;
    GLuint stencil_write_mask;
    GLenum stencil_fail;
    GLenum stencil_depth_fail;
    GLenum stencil_depth_pass;
    GLboolean color_mask[4];
    GLenum logic_op;
    GLenum cull_face;
    GLenum front_face;
    GLenum shade_model;
    GLenum color_material_face;
    GLenum color_material_parameter;
    GLenum active_texture;
    GLenum client_active_texture;
    GLint unpack_alignment;
    GLfloat clear_color[4];
    GLfloat current_color[4];
    GLfloat light_model_ambient[4];
    GLfloat clear_depth;
    GLint clear_stencil;
    GLuint bound_array_buffer;
    GLuint bound_element_array_buffer;
    GLuint bound_texture_reference_buffer;
    PSGLvertexAttribState attribs[PSGL_MAX_VERTEX_ATTRIBS];
    PSGLvertexAttribState generic_attribs[PSGL_MAX_GENERIC_ATTRIBS];
    PSGLtextureUnitState textures[PSGL_MAX_TEXTURE_UNITS];
    PSGLlightState lights[PSGL_MAX_LIGHTS];
    PSGLmaterialState front_material;
    PSGLmaterialState back_material;
    CGprogram bound_vertex_program;
    CGprogram bound_fragment_program;
    uint32_t dirty;
};

int psgl_context_init_system(const PSGLinitOptions *options);
void psgl_context_shutdown_system(void);
PSGLcontext *psgl_context_create(void);
void psgl_context_destroy(PSGLcontext *context);
PSGLdevice *psgl_device_create(const PSGLdeviceParameters *parameters);
void psgl_device_destroy(PSGLdevice *device);
void psgl_context_make_current(PSGLcontext *context, PSGLdevice *device);
void psgl_context_unbind_current(void);
void psgl_context_reset_current(void);
PSGLcontext *psgl_context_current(void);
PSGLdevice *psgl_context_current_device(void);
void psgl_context_load_shader_library(const char *filename);
void psgl_context_bind_cg_program(CGprogram program, CGprofile profile);
void psgl_context_swap(void);
void psgl_context_set_clear_color(GLfloat red, GLfloat green,
                                  GLfloat blue, GLfloat alpha);
void psgl_context_set_clear_depth(GLfloat depth);
void psgl_context_set_clear_stencil(GLint stencil);
void psgl_context_clear(GLbitfield mask);
void psgl_context_set_viewport(GLint x, GLint y, GLsizei width, GLsizei height);
void psgl_context_matrix_mode(GLenum mode);
void psgl_context_load_identity(void);
void psgl_context_push_matrix(void);
void psgl_context_pop_matrix(void);
void psgl_context_load_matrix(const GLfloat *matrix);
void psgl_context_mult_matrix(const GLfloat *matrix);
void psgl_context_rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void psgl_context_scalef(GLfloat x, GLfloat y, GLfloat z);
void psgl_context_translatef(GLfloat x, GLfloat y, GLfloat z);
void psgl_context_frustumf(GLfloat left, GLfloat right, GLfloat bottom,
                           GLfloat top, GLfloat znear, GLfloat zfar);
void psgl_context_orthof(GLfloat left, GLfloat right, GLfloat bottom,
                         GLfloat top, GLfloat znear, GLfloat zfar);
void psgl_context_set_enable(GLenum cap, GLboolean enabled);
void psgl_context_set_alpha_func(GLenum func, GLclampf ref);
void psgl_context_set_blend_func(GLenum sfactor, GLenum dfactor);
void psgl_context_set_blend_func_separate(GLenum sfactor_rgb, GLenum dfactor_rgb,
                                          GLenum sfactor_alpha, GLenum dfactor_alpha);
void psgl_context_set_blend_equation(GLenum mode);
void psgl_context_set_blend_equation_separate(GLenum mode_rgb, GLenum mode_alpha);
void psgl_context_set_scissor(GLint x, GLint y, GLsizei width, GLsizei height);
void psgl_context_set_depth_func(GLenum func);
void psgl_context_set_depth_mask(GLboolean flag);
void psgl_context_set_stencil_func(GLenum func, GLint ref, GLuint mask);
void psgl_context_set_stencil_mask(GLuint mask);
void psgl_context_set_stencil_op(GLenum fail, GLenum zfail, GLenum zpass);
void psgl_context_set_cull_face(GLenum mode);
void psgl_context_set_front_face(GLenum mode);
void psgl_context_set_color_mask(GLboolean red, GLboolean green,
                                 GLboolean blue, GLboolean alpha);
void psgl_context_set_logic_op(GLenum opcode);
void psgl_context_set_shade_model(GLenum mode);
void psgl_context_framebuffer_parameter(GLenum target, GLenum pname,
                                        GLint param);
void psgl_context_set_current_color(GLfloat red, GLfloat green,
                                    GLfloat blue, GLfloat alpha);
void psgl_context_set_light_fv(GLenum light, GLenum pname,
                               const GLfloat *params);
void psgl_context_set_light_f(GLenum light, GLenum pname, GLfloat param);
void psgl_context_set_light_model_fv(GLenum pname, const GLfloat *params);
void psgl_context_set_light_model_f(GLenum pname, GLfloat param);
void psgl_context_set_material_fv(GLenum face, GLenum pname,
                                  const GLfloat *params);
void psgl_context_set_material_f(GLenum face, GLenum pname, GLfloat param);
void psgl_context_set_color_material(GLenum face, GLenum mode);
void psgl_context_get_booleanv(GLenum pname, GLboolean *params);
void psgl_context_get_floatv(GLenum pname, GLfloat *params);
void psgl_context_get_lightfv(GLenum light, GLenum pname, GLfloat *params);
void psgl_context_get_materialfv(GLenum face, GLenum pname, GLfloat *params);
void psgl_context_set_pixel_store(GLenum pname, GLint param);
void psgl_context_active_texture(GLenum texture);
void psgl_context_client_active_texture(GLenum texture);
void psgl_context_gen_textures(GLsizei n, GLuint *textures);
void psgl_context_delete_textures(GLsizei n, const GLuint *textures);
void psgl_context_bind_texture(GLenum target, GLuint texture);
void psgl_context_tex_image_2d(GLenum target, GLint level,
                               GLint internalformat,
                               GLsizei width, GLsizei height, GLint border,
                               GLenum format, GLenum type,
                               const GLvoid *pixels);
void psgl_context_tex_sub_image_2d(GLenum target, GLint level,
                                   GLint xoffset, GLint yoffset,
                                   GLsizei width, GLsizei height,
                                   GLenum format, GLenum type,
                                   const GLvoid *pixels);
void psgl_context_tex_parameter(GLenum target, GLenum pname, GLint param);
void psgl_context_gen_buffers(GLsizei n, GLuint *buffers);
void psgl_context_delete_buffers(GLsizei n, const GLuint *buffers);
void psgl_context_bind_buffer(GLenum target, GLuint name);
void psgl_context_buffer_data(GLenum target, GLsizeiptr size,
                              const GLvoid *data, GLenum usage);
void psgl_context_buffer_sub_data(GLenum target, GLintptr offset,
                                  GLsizeiptr size, const GLvoid *data);
void psgl_context_get_buffer_parameteriv(GLenum target, GLenum pname,
                                         GLint *params);
GLvoid *psgl_context_map_buffer(GLenum target, GLenum access);
GLboolean psgl_context_unmap_buffer(GLenum target);
void psgl_context_set_client_state(GLenum array, GLboolean enabled);
void psgl_context_set_attrib_pointer(PSGLvertexAttribSlot slot, GLint size,
                                     GLenum type, GLsizei stride,
                                     const GLvoid *pointer);
void psgl_context_set_generic_attrib_enabled(GLuint index, GLboolean enabled);
void psgl_context_set_generic_attrib_pointer(GLuint index, GLint size,
                                             GLenum type, GLsizei stride,
                                             const GLvoid *pointer);
void psgl_context_draw_arrays(GLenum mode, GLint first, GLsizei count);
void psgl_context_draw_elements(GLenum mode, GLsizei count, GLenum type,
                                const GLvoid *indices);
void psgl_context_set_fog_f(GLenum pname, GLfloat param);
void psgl_context_set_fog_fv(GLenum pname, const GLfloat *params);

#endif
