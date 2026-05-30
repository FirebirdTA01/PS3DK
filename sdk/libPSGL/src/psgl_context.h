#ifndef PS3DK_LIBPSGL_CONTEXT_H
#define PS3DK_LIBPSGL_CONTEXT_H

#include <PSGL/psgl.h>
#include <cell/gcm.h>
#include <cell/gcm/gcm_command_c.h>
#include <stdint.h>

#define PSGL_MAX_FRAME_BUFFERS 3u
#define PSGL_MAX_TEXTURE_UNITS 4u

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
    PSGL_DIRTY_ALL = 0x000007ffu
} PSGLdirtyBits;

typedef struct PSGLframeBuffer {
    uint32_t *address;
    uint32_t offset;
    uint32_t pitch;
    uint16_t width;
    uint16_t height;
    uint8_t id;
} PSGLframeBuffer;

struct PSGLdevice {
    uint32_t initialized;
    uint16_t width;
    uint16_t height;
    uint16_t render_width;
    uint16_t render_height;
    uint32_t color_format;
    uint32_t depth_format;
    uint32_t multisampling_mode;
    uint32_t pitch;
    uint32_t frame_count;
    uint32_t current_frame;
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
    GLfloat texture[16];
    GLint viewport[4];
    GLint scissor[4];
    GLboolean blend_enabled;
    GLboolean depth_test_enabled;
    GLboolean stencil_test_enabled;
    GLboolean alpha_test_enabled;
    GLfloat clear_color[4];
    GLfloat clear_depth;
    GLint clear_stencil;
    GLuint bound_array_buffer;
    GLuint bound_element_array_buffer;
    GLuint bound_textures[PSGL_MAX_TEXTURE_UNITS];
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
void psgl_context_reset_current(void);
PSGLcontext *psgl_context_current(void);
PSGLdevice *psgl_context_current_device(void);
void psgl_context_swap(void);
void psgl_context_set_clear_color(GLfloat red, GLfloat green,
                                  GLfloat blue, GLfloat alpha);
void psgl_context_set_clear_depth(GLfloat depth);
void psgl_context_set_clear_stencil(GLint stencil);
void psgl_context_clear(GLbitfield mask);
void psgl_context_set_viewport(GLint x, GLint y, GLsizei width, GLsizei height);

#endif
