/* core_gl.c — GLES 1.1 core stubs (Slice 1d link gate).
 *
 * Covers gl.h surface.  Every stub returns a spec-correct default:
 * calls that can never fail (e.g. glEnable, glViewport) are NOPs;
 * query functions return spec-minimum / zeroed values.
 */
#include <stddef.h>

#include <GLES/gl.h>

#include "psgl_context.h"

/* ── error state ─────────────────────────────────────────────────── */

static GLenum g_gl_error = GL_NO_ERROR;

static void gl_zero(void *ptr, size_t size)
{
    unsigned char *out = (unsigned char *)ptr;
    while (size--) {
        *out++ = 0;
    }
}

static GLfloat gl_fixed_to_float(GLfixed value)
{
    return (GLfloat)value / 65536.0f;
}

static void gl_fixed4_to_float(const GLfixed *in, GLfloat out[4])
{
    for (unsigned i = 0u; i < 4u; i++)
        out[i] = gl_fixed_to_float(in[i]);
}

GLAPI GLenum glGetError(void)
{
    GLenum e = g_gl_error;
    g_gl_error = GL_NO_ERROR;
    return e;
}

/* ── get-string / queries ────────────────────────────────────────── */

GLAPI const GLubyte *glGetString(GLenum name)
{
    switch (name) {
    case GL_VENDOR:     return (const GLubyte *)"PS3DK";
    case GL_RENDERER:   return (const GLubyte *)"PS3DK PSGL Stub";
    case GL_VERSION:    return (const GLubyte *)"1.1 PS3DK";
    case GL_EXTENSIONS: return (const GLubyte *)"";
    default:            return (const GLubyte *)"";
    }
}

GLAPI void glGetIntegerv(GLenum pname, GLint *params)
{
    if (!params) return;
    switch (pname) {
    case GL_MAX_LIGHTS:                  *params = 8;    break;
    case GL_MAX_TEXTURE_SIZE:            *params = 4096; break;
    case GL_MAX_MODELVIEW_STACK_DEPTH:   *params = 32;   break;
    case GL_MAX_PROJECTION_STACK_DEPTH:  *params = 2;    break;
    case GL_MAX_TEXTURE_STACK_DEPTH:     *params = 2;    break;
    case GL_MAX_VIEWPORT_DIMS:           params[0] = 1920; params[1] = 1080; break;
    case GL_MAX_TEXTURE_UNITS:           *params = 4;    break;
    case GL_MAX_ELEMENTS_VERTICES:       *params = 4096; break;
    case GL_MAX_ELEMENTS_INDICES:        *params = 4096; break;
    case GL_SUBPIXEL_BITS:               *params = 4;    break;
    case GL_RED_BITS:   case GL_GREEN_BITS:
    case GL_BLUE_BITS:  case GL_ALPHA_BITS:
    case GL_DEPTH_BITS: case GL_STENCIL_BITS:
        *params = 8; break;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: *params = 0; break;
    case GL_ALIASED_POINT_SIZE_RANGE:
        params[0] = 1; params[1] = 64; break;
    case GL_ALIASED_LINE_WIDTH_RANGE:
        params[0] = 1; params[1] = 8; break;
    case GL_SMOOTH_POINT_SIZE_RANGE:
        params[0] = 1; params[1] = 64; break;
    case GL_SMOOTH_LINE_WIDTH_RANGE:
        params[0] = 1; params[1] = 8; break;
    case GL_FOG_MODE: {
        PSGLcontext *ctx = psglGetCurrentContext();
        *params = ctx ? (GLint)ctx->fog_mode : 0;
        break;
    }
    default:
        *params = 0; break;
    }
}

/* ── enable / disable / client-state ─────────────────────────────── */

GLAPI void glEnable(GLenum cap)        { psgl_context_set_enable(cap, GL_TRUE); }
GLAPI void glDisable(GLenum cap)       { psgl_context_set_enable(cap, GL_FALSE); }
GLAPI void glEnableClientState(GLenum array)
{ psgl_context_set_client_state(array, GL_TRUE); }
GLAPI void glDisableClientState(GLenum array)
{ psgl_context_set_client_state(array, GL_FALSE); }

/* ── buffer clear ────────────────────────────────────────────────── */

GLAPI void glClear(GLbitfield mask)    { psgl_context_clear(mask); }
GLAPI void glClearColor(GLclampf red, GLclampf green,
                        GLclampf blue, GLclampf alpha)
{ psgl_context_set_clear_color(red, green, blue, alpha); }
GLAPI void glClearColorx(GLclampx red, GLclampx green,
                         GLclampx blue, GLclampx alpha)
{
    psgl_context_set_clear_color((GLfloat)red / 65536.0f,
                                 (GLfloat)green / 65536.0f,
                                 (GLfloat)blue / 65536.0f,
                                 (GLfloat)alpha / 65536.0f);
}
GLAPI void glClearDepthf(GLclampf depth)   { psgl_context_set_clear_depth(depth); }
GLAPI void glClearDepthx(GLclampx depth)   { psgl_context_set_clear_depth((GLfloat)depth / 65536.0f); }
GLAPI void glClearStencil(GLint s)         { psgl_context_set_clear_stencil(s); }

/* ── alpha test ──────────────────────────────────────────────────── */

GLAPI void glAlphaFunc(GLenum func, GLclampf ref)
{ psgl_context_set_alpha_func(func, ref); }
GLAPI void glAlphaFuncx(GLenum func, GLclampx ref)
{ psgl_context_set_alpha_func(func, (GLfloat)ref / 65536.0f); }

/* ── blend ───────────────────────────────────────────────────────── */

GLAPI void glBlendFunc(GLenum sfactor, GLenum dfactor)
{ psgl_context_set_blend_func(sfactor, dfactor); }

/* ── draw ────────────────────────────────────────────────────────── */

GLAPI void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{ psgl_context_draw_arrays(mode, first, count); }
GLAPI void glDrawElements(GLenum mode, GLsizei count,
                          GLenum type, const GLvoid *indices)
{ psgl_context_draw_elements(mode, count, type, indices); }

/* ── viewport / scissor ──────────────────────────────────────────── */

GLAPI void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{ psgl_context_set_viewport(x, y, width, height); }
GLAPI void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{ psgl_context_set_scissor(x, y, width, height); }

/* ── depth ───────────────────────────────────────────────────────── */

GLAPI void glDepthFunc(GLenum func)       { psgl_context_set_depth_func(func); }
GLAPI void glDepthMask(GLboolean flag)    { psgl_context_set_depth_mask(flag); }
GLAPI void glDepthRangef(GLclampf zNear, GLclampf zFar)
{ (void)zNear; (void)zFar; }
GLAPI void glDepthRangex(GLclampx zNear, GLclampx zFar)
{ (void)zNear; (void)zFar; }

/* ── stencil ─────────────────────────────────────────────────────── */

GLAPI void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{ psgl_context_set_stencil_func(func, ref, mask); }
GLAPI void glStencilMask(GLuint mask)     { psgl_context_set_stencil_mask(mask); }
GLAPI void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{ psgl_context_set_stencil_op(fail, zfail, zpass); }

/* ── cull face ───────────────────────────────────────────────────── */

GLAPI void glCullFace(GLenum mode)        { psgl_context_set_cull_face(mode); }
GLAPI void glFrontFace(GLenum mode)       { psgl_context_set_front_face(mode); }

/* ── colour mask ─────────────────────────────────────────────────── */

GLAPI void glColorMask(GLboolean red, GLboolean green,
                       GLboolean blue, GLboolean alpha)
{ psgl_context_set_color_mask(red, green, blue, alpha); }

/* ── colour ──────────────────────────────────────────────────────── */

GLAPI void glColorMaterial(GLenum face, GLenum mode)
{ psgl_context_set_color_material(face, mode); }
GLAPI void glColor4f(GLfloat red, GLfloat green,
                     GLfloat blue, GLfloat alpha)
{ psgl_context_set_current_color(red, green, blue, alpha); }
GLAPI void glColor4x(GLfixed red, GLfixed green,
                     GLfixed blue, GLfixed alpha)
{
    psgl_context_set_current_color(gl_fixed_to_float(red),
                                   gl_fixed_to_float(green),
                                   gl_fixed_to_float(blue),
                                   gl_fixed_to_float(alpha));
}

/* ── normal ──────────────────────────────────────────────────────── */

GLAPI void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz)
{ (void)nx; (void)ny; (void)nz; }
GLAPI void glNormal3x(GLfixed nx, GLfixed ny, GLfixed nz)
{ (void)nx; (void)ny; (void)nz; }

/* ── shading ─────────────────────────────────────────────────────── */

GLAPI void glShadeModel(GLenum mode)      { psgl_context_set_shade_model(mode); }

/* ── point / line ────────────────────────────────────────────────── */

GLAPI void glPointSize(GLfloat size)      { (void)size; }
GLAPI void glPointSizex(GLfixed size)     { (void)size; }
GLAPI void glLineWidth(GLfloat width)     { (void)width; }
GLAPI void glLineWidthx(GLfixed width)    { (void)width; }

/* ── polygon offset ──────────────────────────────────────────────── */

GLAPI void glPolygonOffset(GLfloat factor, GLfloat units)
{ (void)factor; (void)units; }
GLAPI void glPolygonOffsetx(GLfixed factor, GLfixed units)
{ (void)factor; (void)units; }

/* ── hint ────────────────────────────────────────────────────────── */

GLAPI void glHint(GLenum target, GLenum mode)
{ (void)target; (void)mode; }

/* ── logic op ────────────────────────────────────────────────────── */

GLAPI void glLogicOp(GLenum opcode)       { psgl_context_set_logic_op(opcode); }

/* ── pixel store ─────────────────────────────────────────────────── */

GLAPI void glPixelStorei(GLenum pname, GLint param)
{ psgl_context_set_pixel_store(pname, param); }

/* ── sample coverage ─────────────────────────────────────────────── */

GLAPI void glSampleCoverage(GLclampf value, GLboolean invert)
{ (void)value; (void)invert; }
GLAPI void glSampleCoveragex(GLclampx value, GLboolean invert)
{ (void)value; (void)invert; }

/* ── flush / finish ──────────────────────────────────────────────── */

GLAPI void glFlush(void)  {}
GLAPI void glFinish(void) {}

/* ── matrices ────────────────────────────────────────────────────── */

GLAPI void glMatrixMode(GLenum mode)      { psgl_context_matrix_mode(mode); }
GLAPI void glLoadIdentity(void)           { psgl_context_load_identity(); }
GLAPI void glPushMatrix(void)             { psgl_context_push_matrix(); }
GLAPI void glPopMatrix(void)              { psgl_context_pop_matrix(); }
GLAPI void glLoadMatrixf(const GLfloat *m)  { psgl_context_load_matrix(m); }
GLAPI void glLoadMatrixx(const GLfixed *m)
{
    GLfloat matrix[16];
    if (!m) return;
    for (unsigned i = 0; i < 16u; i++)
        matrix[i] = (GLfloat)m[i] / 65536.0f;
    psgl_context_load_matrix(matrix);
}
GLAPI void glMultMatrixf(const GLfloat *m)  { psgl_context_mult_matrix(m); }
GLAPI void glMultMatrixx(const GLfixed *m)
{
    GLfloat matrix[16];
    if (!m) return;
    for (unsigned i = 0; i < 16u; i++)
        matrix[i] = (GLfloat)m[i] / 65536.0f;
    psgl_context_mult_matrix(matrix);
}
GLAPI void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{ psgl_context_rotatef(angle, x, y, z); }
GLAPI void glRotatex(GLfixed angle, GLfixed x, GLfixed y, GLfixed z)
{
    psgl_context_rotatef((GLfloat)angle / 65536.0f,
                         (GLfloat)x / 65536.0f,
                         (GLfloat)y / 65536.0f,
                         (GLfloat)z / 65536.0f);
}
GLAPI void glScalef(GLfloat x, GLfloat y, GLfloat z)
{ psgl_context_scalef(x, y, z); }
GLAPI void glScalex(GLfixed x, GLfixed y, GLfixed z)
{
    psgl_context_scalef((GLfloat)x / 65536.0f,
                        (GLfloat)y / 65536.0f,
                        (GLfloat)z / 65536.0f);
}
GLAPI void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{ psgl_context_translatef(x, y, z); }
GLAPI void glTranslatex(GLfixed x, GLfixed y, GLfixed z)
{
    psgl_context_translatef((GLfloat)x / 65536.0f,
                            (GLfloat)y / 65536.0f,
                            (GLfloat)z / 65536.0f);
}
GLAPI void glFrustumf(GLfloat l, GLfloat r, GLfloat b,
                      GLfloat t, GLfloat n, GLfloat f)
{ psgl_context_frustumf(l, r, b, t, n, f); }
GLAPI void glFrustumx(GLfixed l, GLfixed r, GLfixed b,
                      GLfixed t, GLfixed n, GLfixed f)
{
    psgl_context_frustumf((GLfloat)l / 65536.0f,
                          (GLfloat)r / 65536.0f,
                          (GLfloat)b / 65536.0f,
                          (GLfloat)t / 65536.0f,
                          (GLfloat)n / 65536.0f,
                          (GLfloat)f / 65536.0f);
}
GLAPI void glOrthof(GLfloat l, GLfloat r, GLfloat b,
                    GLfloat t, GLfloat n, GLfloat f)
{ psgl_context_orthof(l, r, b, t, n, f); }
GLAPI void glOrthox(GLfixed l, GLfixed r, GLfixed b,
                    GLfixed t, GLfixed n, GLfixed f)
{
    psgl_context_orthof((GLfloat)l / 65536.0f,
                        (GLfloat)r / 65536.0f,
                        (GLfloat)b / 65536.0f,
                        (GLfloat)t / 65536.0f,
                        (GLfloat)n / 65536.0f,
                        (GLfloat)f / 65536.0f);
}

/* ── textures ────────────────────────────────────────────────────── */

GLAPI void glActiveTexture(GLenum texture)    { psgl_context_active_texture(texture); }
GLAPI void glBindTexture(GLenum target, GLuint texture)
{ psgl_context_bind_texture(target, texture); }

GLAPI void glGenTextures(GLsizei n, GLuint *textures)
{ psgl_context_gen_textures(n, textures); }

GLAPI void glDeleteTextures(GLsizei n, const GLuint *textures)
{ psgl_context_delete_textures(n, textures); }

GLAPI void glTexImage2D(GLenum target, GLint level,
                        GLint internalformat,
                        GLsizei width, GLsizei height, GLint border,
                        GLenum format, GLenum type,
                        const GLvoid *pixels)
{
    psgl_context_tex_image_2d(target, level, internalformat,
                              width, height, border, format, type, pixels);
}

GLAPI void glTexSubImage2D(GLenum target, GLint level,
                           GLint xoffset, GLint yoffset,
                           GLsizei width, GLsizei height,
                           GLenum format, GLenum type,
                           const GLvoid *pixels)
{
    psgl_context_tex_sub_image_2d(target, level, xoffset, yoffset,
                                  width, height, format, type, pixels);
}

GLAPI void glCopyTexImage2D(GLenum target, GLint level,
                            GLenum internalformat,
                            GLint x, GLint y,
                            GLsizei width, GLsizei height, GLint border)
{
    (void)target; (void)level;  (void)internalformat;
    (void)x;      (void)y;      (void)width;
    (void)height; (void)border;
}

GLAPI void glCopyTexSubImage2D(GLenum target, GLint level,
                               GLint xoffset, GLint yoffset,
                               GLint x, GLint y,
                               GLsizei width, GLsizei height)
{
    (void)target;  (void)level;   (void)xoffset;
    (void)yoffset; (void)x;       (void)y;
    (void)width;   (void)height;
}

GLAPI void glCompressedTexImage2D(GLenum target, GLint level,
                                  GLenum internalformat,
                                  GLsizei width, GLsizei height,
                                  GLint border, GLsizei imageSize,
                                  const GLvoid *data)
{
    (void)target;  (void)level;  (void)internalformat;
    (void)width;   (void)height; (void)border;
    (void)imageSize; (void)data;
}

GLAPI void glCompressedTexSubImage2D(GLenum target, GLint level,
                                     GLint xoffset, GLint yoffset,
                                     GLsizei width, GLsizei height,
                                     GLenum format, GLsizei imageSize,
                                     const GLvoid *data)
{
    (void)target;  (void)level;   (void)xoffset;
    (void)yoffset; (void)width;   (void)height;
    (void)format;  (void)imageSize; (void)data;
}

GLAPI void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{ psgl_context_tex_parameter(target, pname, (GLint)param); }
GLAPI void glTexParameterx(GLenum target, GLenum pname, GLfixed param)
{ psgl_context_tex_parameter(target, pname, (GLint)(param / 65536)); }

GLAPI void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{ (void)target; (void)pname; (void)param; }
GLAPI void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{ (void)target; (void)pname; (void)params; }
GLAPI void glTexEnvx(GLenum target, GLenum pname, GLfixed param)
{ (void)target; (void)pname; (void)param; }
GLAPI void glTexEnvxv(GLenum target, GLenum pname, const GLfixed *params)
{ (void)target; (void)pname; (void)params; }

GLAPI void glClientActiveTexture(GLenum texture)
{ psgl_context_client_active_texture(texture); }

/* ── vertex / normal / texcoord arrays ───────────────────────────── */

GLAPI void glVertexPointer(GLint size, GLenum type,
                           GLsizei stride, const GLvoid *pointer)
{ psgl_context_set_attrib_pointer(PSGL_ATTRIB_VERTEX, size, type, stride, pointer); }
GLAPI void glNormalPointer(GLenum type, GLsizei stride,
                           const GLvoid *pointer)
{ psgl_context_set_attrib_pointer(PSGL_ATTRIB_NORMAL, 3, type, stride, pointer); }
GLAPI void glColorPointer(GLint size, GLenum type,
                          GLsizei stride, const GLvoid *pointer)
{ psgl_context_set_attrib_pointer(PSGL_ATTRIB_COLOR, size, type, stride, pointer); }
GLAPI void glTexCoordPointer(GLint size, GLenum type,
                             GLsizei stride, const GLvoid *pointer)
{ psgl_context_set_attrib_pointer(PSGL_ATTRIB_TEXCOORD, size, type, stride, pointer); }

/* ── multi-texcoord (immediate) ──────────────────────────────────── */

GLAPI void glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t,
                             GLfloat r, GLfloat q)
{ (void)target; (void)s; (void)t; (void)r; (void)q; }
GLAPI void glMultiTexCoord4x(GLenum target, GLfixed s, GLfixed t,
                             GLfixed r, GLfixed q)
{ (void)target; (void)s; (void)t; (void)r; (void)q; }

/* ── fog ─────────────────────────────────────────────────────────── */

GLAPI void glFogf(GLenum pname, GLfloat param)
{ psgl_context_set_fog_f(pname, param); }
GLAPI void glFogfv(GLenum pname, const GLfloat *params)
{ psgl_context_set_fog_fv(pname, params); }
GLAPI void glFogx(GLenum pname, GLfixed param)
{ psgl_context_set_fog_f(pname, gl_fixed_to_float(param)); }
GLAPI void glFogxv(GLenum pname, const GLfixed *params)
{
    GLfloat values[4];
    if (!params) return;
    values[0] = gl_fixed_to_float(params[0]);
    values[1] = gl_fixed_to_float(params[1]);
    values[2] = gl_fixed_to_float(params[2]);
    values[3] = gl_fixed_to_float(params[3]);
    psgl_context_set_fog_fv(pname, values);
}

/* ── lighting ────────────────────────────────────────────────────── */

GLAPI void glLightf(GLenum light, GLenum pname, GLfloat param)
{ psgl_context_set_light_f(light, pname, param); }
GLAPI void glLightfv(GLenum light, GLenum pname, const GLfloat *params)
{ psgl_context_set_light_fv(light, pname, params); }
GLAPI void glLightx(GLenum light, GLenum pname, GLfixed param)
{ psgl_context_set_light_f(light, pname, gl_fixed_to_float(param)); }
GLAPI void glLightxv(GLenum light, GLenum pname, const GLfixed *params)
{
    GLfloat values[4];
    if (!params) return;
    gl_fixed4_to_float(params, values);
    psgl_context_set_light_fv(light, pname, values);
}
GLAPI void glLightModelf(GLenum pname, GLfloat param)
{ psgl_context_set_light_model_f(pname, param); }
GLAPI void glLightModelfv(GLenum pname, const GLfloat *params)
{ psgl_context_set_light_model_fv(pname, params); }
GLAPI void glLightModelx(GLenum pname, GLfixed param)
{ psgl_context_set_light_model_f(pname, gl_fixed_to_float(param)); }
GLAPI void glLightModelxv(GLenum pname, const GLfixed *params)
{
    GLfloat values[4];
    if (!params) return;
    gl_fixed4_to_float(params, values);
    psgl_context_set_light_model_fv(pname, values);
}

/* ── material ────────────────────────────────────────────────────── */

GLAPI void glMaterialf(GLenum face, GLenum pname, GLfloat param)
{ psgl_context_set_material_f(face, pname, param); }
GLAPI void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params)
{ psgl_context_set_material_fv(face, pname, params); }
GLAPI void glMaterialx(GLenum face, GLenum pname, GLfixed param)
{ psgl_context_set_material_f(face, pname, gl_fixed_to_float(param)); }
GLAPI void glMaterialxv(GLenum face, GLenum pname, const GLfixed *params)
{
    GLfloat values[4];
    if (!params) return;
    gl_fixed4_to_float(params, values);
    psgl_context_set_material_fv(face, pname, values);
}

/* ── read pixels ─────────────────────────────────────────────────── */

GLAPI void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, GLvoid *pixels)
{
    (void)x; (void)y; (void)width; (void)height;
    (void)format; (void)type;
    if (pixels) gl_zero(pixels, (size_t)(width * height * 4));
}
