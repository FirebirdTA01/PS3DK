/* core_gl_ext.c — GLES 1.1 extension stubs (Slice 1d link gate).
 *
 * Covers glext.h surface: VBO/PBO, 3D textures, cube maps, FBO/RBO,
 * fences (NV), events (SCE), clip planes, occlusion queries,
 * conditional rendering, vertex attribute sets, debug markers,
 * texture reference buffers, blend equation, polygon mode, etc.
 */
#include <stddef.h>

#include <GLES/glext.h>

#include "psgl_context.h"

static void glext_zero(void *ptr, size_t size)
{
    unsigned char *out = (unsigned char *)ptr;
    while (size--) {
        *out++ = 0;
    }
}

/* ── blend equation / colour ─────────────────────────────────────── */

GLAPI void glBlendEquation(GLenum mode)
{ psgl_context_set_blend_equation(mode); }
GLAPI void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha)
{ psgl_context_set_blend_equation_separate(modeRGB, modeAlpha); }
GLAPI void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB,
                               GLenum sfactorAlpha, GLenum dfactorAlpha)
{ psgl_context_set_blend_func_separate(sfactorRGB, dfactorRGB,
                                       sfactorAlpha, dfactorAlpha); }
GLAPI void glBlendColor(GLclampf red, GLclampf green,
                        GLclampf blue, GLclampf alpha)
{ (void)red; (void)green; (void)blue; (void)alpha; }

/* ── normal / colour convenience ─────────────────────────────────── */

GLAPI void glNormal3fv(const GLfloat *v)          { (void)v; }
GLAPI void glColor4fv(const GLfloat *v)
{
    if (v) psgl_context_set_current_color(v[0], v[1], v[2], v[3]);
}
GLAPI void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    psgl_context_set_current_color((GLfloat)r / 255.0f,
                                   (GLfloat)g / 255.0f,
                                   (GLfloat)b / 255.0f,
                                   (GLfloat)a / 255.0f);
}

/* ── get queries ─────────────────────────────────────────────────── */

GLAPI void glGetBooleanv(GLenum pname, GLboolean *params)
{ psgl_context_get_booleanv(pname, params); }
GLAPI void glGetFloatv(GLenum pname, GLfloat *params)
{ psgl_context_get_floatv(pname, params); }
GLAPI void glGetLightfv(GLenum light, GLenum pname, GLfloat *params)
{ psgl_context_get_lightfv(light, pname, params); }
GLAPI void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params)
{ psgl_context_get_materialfv(face, pname, params); }

/* ── 3D textures ─────────────────────────────────────────────────── */

GLAPI void glTexImage3D(GLenum target, GLint level, GLint internalformat,
                        GLsizei width, GLsizei height, GLsizei depth,
                        GLint border, GLenum format, GLenum type,
                        const GLvoid *pixels)
{
    (void)target;  (void)level;  (void)internalformat;
    (void)width;   (void)height; (void)depth;
    (void)border;  (void)format; (void)type;
    (void)pixels;
}

GLAPI void glTexSubImage3D(GLenum target, GLint level,
                           GLint xoffset, GLint yoffset, GLint zoffset,
                           GLsizei width, GLsizei height, GLsizei depth,
                           GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)target;  (void)level;   (void)xoffset;
    (void)yoffset; (void)zoffset;
    (void)width;   (void)height;  (void)depth;
    (void)format;  (void)type;    (void)pixels;
}

GLAPI void glCompressedTexImage3D(GLenum target, GLint level,
                                  GLenum internalformat,
                                  GLsizei width, GLsizei height,
                                  GLsizei depth, GLint border,
                                  GLsizei imageSize, const GLvoid *data)
{
    (void)target;  (void)level;  (void)internalformat;
    (void)width;   (void)height; (void)depth;
    (void)border;  (void)imageSize; (void)data;
}

GLAPI void glCompressedTexSubImage3D(GLenum target, GLint level,
                                     GLint xoffset, GLint yoffset,
                                     GLint zoffset,
                                     GLsizei width, GLsizei height,
                                     GLsizei depth,
                                     GLenum format, GLsizei imageSize,
                                     const GLvoid *data)
{
    (void)target;  (void)level;   (void)xoffset;
    (void)yoffset; (void)zoffset;
    (void)width;   (void)height;  (void)depth;
    (void)format;  (void)imageSize; (void)data;
}

GLAPI void glCopyTexSubImage3D(GLenum target, GLint level,
                               GLint xoffset, GLint yoffset, GLint zoffset,
                               GLint x, GLint y,
                               GLsizei width, GLsizei height)
{
    (void)target;  (void)level;   (void)xoffset;
    (void)yoffset; (void)zoffset;
    (void)x;       (void)y;
    (void)width;   (void)height;
}

/* ── draw range elements ─────────────────────────────────────────── */

GLAPI void glDrawRangeElements(GLenum mode, GLuint start, GLuint end,
                               GLsizei count, GLenum type,
                               const GLvoid *indices)
{ (void)mode; (void)start; (void)end; (void)count; (void)type; (void)indices; }

/* ── fog int variants ────────────────────────────────────────────── */

GLAPI void glFogi(GLenum pname, GLint param)
{ (void)pname; (void)param; }
GLAPI void glFogiv(GLenum pname, const GLint *params)
{ (void)pname; (void)params; }

/* ── polygon mode ────────────────────────────────────────────────── */

GLAPI void glPolygonMode(GLenum face, GLenum mode)
{ (void)face; (void)mode; }
GLAPI void glReadBuffer(GLenum mode)    { (void)mode; }

/* ── texenv int variants ─────────────────────────────────────────── */

GLAPI void glTexEnvi(GLenum target, GLenum pname, GLint param)
{ (void)target; (void)pname; (void)param; }
GLAPI void glTexEnviv(GLenum target, GLenum pname, const GLint *params)
{ (void)target; (void)pname; (void)params; }

/* ── texgen ──────────────────────────────────────────────────────── */

GLAPI void glTexGenf(GLenum coord, GLenum pname, GLfloat param)
{ (void)coord; (void)pname; (void)param; }
GLAPI void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params)
{ (void)coord; (void)pname; (void)params; }
GLAPI void glTexGeni(GLenum coord, GLenum pname, GLint param)
{ (void)coord; (void)pname; (void)param; }
GLAPI void glTexGeniv(GLenum coord, GLenum pname, const GLint *params)
{ (void)coord; (void)pname; (void)params; }

/* ── texparam vec/int variants ───────────────────────────────────── */

GLAPI void glTexParameterfv(GLenum target, GLenum pname,
                            const GLfloat *params)
{ if (params) psgl_context_tex_parameter(target, pname, (GLint)params[0]); }
GLAPI void glTexParameteri(GLenum target, GLenum pname, GLint param)
{ psgl_context_tex_parameter(target, pname, param); }
GLAPI void glTexParameteriv(GLenum target, GLenum pname,
                            const GLint *params)
{ if (params) psgl_context_tex_parameter(target, pname, params[0]); }

/* ── VBO / PBO ───────────────────────────────────────────────────── */

GLAPI void glBindBuffer(GLenum target, GLuint name)
{ psgl_context_bind_buffer(target, name); }

GLAPI void glDeleteBuffers(GLsizei n, const GLuint *buffers)
{ psgl_context_delete_buffers(n, buffers); }

GLAPI void glGenBuffers(GLsizei n, GLuint *buffers)
{ psgl_context_gen_buffers(n, buffers); }

GLAPI void glBufferData(GLenum target, GLsizeiptr size,
                        const GLvoid *data, GLenum usage)
{ psgl_context_buffer_data(target, size, data, usage); }

GLAPI void glBufferSubData(GLenum target, GLintptr offset,
                           GLsizeiptr size, const GLvoid *data)
{ psgl_context_buffer_sub_data(target, offset, size, data); }

GLAPI void glGetBufferParameteriv(GLenum target, GLenum pname,
                                  GLint *params)
{ psgl_context_get_buffer_parameteriv(target, pname, params); }

GLAPI GLvoid *glMapBuffer(GLenum target, GLenum access)
{ return psgl_context_map_buffer(target, access); }

GLAPI GLboolean glUnmapBuffer(GLenum target)
{ return psgl_context_unmap_buffer(target); }

/* ── primitive restart NV ────────────────────────────────────────── */

GLAPI void glPrimitiveRestartIndexNV(GLuint index) { (void)index; }

/* ── fences NV ───────────────────────────────────────────────────── */

GLAPI void glDeleteFencesNV(GLsizei n, const GLuint *fences)
{ (void)n; (void)fences; }
GLAPI void glGenFencesNV(GLsizei n, GLuint *fences)
{ if (fences) glext_zero(fences, (size_t)n * sizeof(GLuint)); }
GLAPI GLboolean glIsFenceNV(GLuint fence)
{ (void)fence; return GL_FALSE; }
GLAPI GLboolean glTestFenceNV(GLuint fence)
{ (void)fence; return GL_TRUE; }
GLAPI void glGetFenceivNV(GLuint fence, GLenum pname, GLint *params)
{ (void)fence; (void)pname;
  if (params) *params = 0; }
GLAPI void glFinishFenceNV(GLuint fence)   { (void)fence; }
GLAPI void glSetFenceNV(GLuint fence, GLenum condition)
{ (void)fence; (void)condition; }

/* ── events SCE ──────────────────────────────────────────────────── */

GLAPI void glDeleteEventsSCE(GLsizei n, const GLuint *fences)
{ (void)n; (void)fences; }
GLAPI void glGenEventsSCE(GLsizei n, GLuint *fences)
{ if (fences) glext_zero(fences, (size_t)n * sizeof(GLuint)); }
GLAPI void glSetEventSCE(GLuint event)       { (void)event; }
GLAPI void glResetEventSCE(GLuint event)     { (void)event; }
GLAPI void glWaitEventSCE(GLuint event, GLboolean autoReset)
{ (void)event; (void)autoReset; }
GLAPI unsigned long long glMapEventSCE(GLuint event)
{ (void)event; return 0ULL; }
GLAPI void glSetMappedEventSCE(unsigned long long mappedEvent)
{ (void)mappedEvent; }

/* ── clip planes ─────────────────────────────────────────────────── */

GLAPI void glClipPlanef(GLenum plane, const GLfloat *equation)
{ (void)plane; (void)equation; }

/* ── FBO / RBO OES ───────────────────────────────────────────────── */

GLAPI GLboolean glIsRenderbufferOES(GLuint renderbuffer)
{ (void)renderbuffer; return GL_FALSE; }
GLAPI void glBindRenderbufferOES(GLenum target, GLuint renderbuffer)
{ (void)target; (void)renderbuffer; }
GLAPI void glDeleteRenderbuffersOES(GLsizei n, const GLuint *renderbuffers)
{ (void)n; (void)renderbuffers; }
GLAPI void glGenRenderbuffersOES(GLsizei n, GLuint *renderbuffers)
{ if (renderbuffers) glext_zero(renderbuffers, (size_t)n * sizeof(GLuint)); }
GLAPI void glRenderbufferStorageOES(GLenum target, GLenum internalformat,
                                    GLsizei width, GLsizei height)
{ (void)target; (void)internalformat; (void)width; (void)height; }
GLAPI void glGetRenderbufferParameterivOES(GLenum target, GLenum pname,
                                           GLint *params)
{ (void)target; (void)pname; if (params) *params = 0; }

GLAPI GLboolean glIsFramebufferOES(GLuint framebuffer)
{ (void)framebuffer; return GL_FALSE; }
GLAPI void glBindFramebufferOES(GLenum target, GLuint framebuffer)
{ (void)target; (void)framebuffer; }
GLAPI void glDeleteFramebuffersOES(GLsizei n, const GLuint *framebuffers)
{ (void)n; (void)framebuffers; }
GLAPI void glGenFramebuffersOES(GLsizei n, GLuint *framebuffers)
{ if (framebuffers) glext_zero(framebuffers, (size_t)n * sizeof(GLuint)); }
GLAPI GLenum glCheckFramebufferStatusOES(GLenum target)
{ (void)target; return GL_FRAMEBUFFER_COMPLETE_OES; }
GLAPI void glFramebufferTexture2DOES(GLenum target, GLenum attachment,
                                     GLenum textarget, GLuint texture,
                                     GLint level)
{ (void)target; (void)attachment; (void)textarget;
  (void)texture; (void)level; }
GLAPI void glFramebufferRenderbufferOES(GLenum target, GLenum attachment,
                                        GLenum renderbuffertarget,
                                        GLuint renderbuffer)
{ (void)target; (void)attachment;
  (void)renderbuffertarget; (void)renderbuffer; }
GLAPI void glGetFramebufferAttachmentParameterivOES(GLenum target,
                                                     GLenum attachment,
                                                     GLenum pname,
                                                     GLint *params)
{ (void)target; (void)attachment; (void)pname;
  if (params) *params = 0; }
GLAPI void glGenerateMipmapOES(GLenum target)   { (void)target; }
GLAPI void glFramebufferParameteriSCE(GLenum target, GLenum pname,
                                      GLint param)
{ (void)target; (void)pname; (void)param; }

/* ── occlusion queries ARB ───────────────────────────────────────── */

GLAPI void glGenQueriesARB(GLsizei n, GLuint *ids)
{ if (ids) glext_zero(ids, (size_t)n * sizeof(GLuint)); }
GLAPI void glDeleteQueriesARB(GLsizei n, const GLuint *ids)
{ (void)n; (void)ids; }
GLAPI GLboolean glIsQueryARB(GLuint id)
{ (void)id; return GL_FALSE; }
GLAPI void glBeginQueryARB(GLenum target, GLuint id)
{ (void)target; (void)id; }
GLAPI void glEndQueryARB(GLenum target)  { (void)target; }
GLAPI void glGetQueryivARB(GLenum target, GLenum pname, GLint *params)
{ (void)target; (void)pname; if (params) *params = 0; }
GLAPI void glGetQueryObjectivARB(GLuint id, GLenum pname, GLint *params)
{ (void)id; (void)pname; if (params) *params = 0; }
GLAPI void glGetQueryObjectuivARB(GLuint id, GLenum pname, GLuint *params)
{ (void)id; (void)pname; if (params) *params = 0; }
GLAPI void glGetQueryObjectul64vARB(GLuint id, GLenum pname,
                                    unsigned long long *params)
{ (void)id; (void)pname; if (params) *params = 0ULL; }

/* ── conditional rendering SCE ──────────────────────────────────── */

GLAPI void glBeginConditionalRenderingSCE(GLuint id)  { (void)id; }
GLAPI void glEndConditionalRenderingSCE(void)         {}

/* ── stencil two-side EXT ────────────────────────────────────────── */

GLAPI void glActiveStencilFaceEXT(GLenum face)        { (void)face; }

/* ── vertex attribute sets SCE ───────────────────────────────────── */

GLAPI void glGenAttribSetsSCE(GLsizei n, GLuint *names)
{ if (names) glext_zero(names, (size_t)n * sizeof(GLuint)); }
GLAPI void glDeleteAttribSetsSCE(GLsizei n, const GLuint *names)
{ (void)n; (void)names; }
GLAPI GLboolean glIsAttribSetSCE(GLuint name)
{ (void)name; return GL_FALSE; }
GLAPI void glBindAttribSetSCE(GLuint name)            { (void)name; }
GLAPI void glCopyAttribSetSCE(GLuint fromName, GLuint toName)
{ (void)fromName; (void)toName; }

/* ── string markers ──────────────────────────────────────────────── */

GLAPI void glPushStringMarkerSCE(const GLubyte *string) { (void)string; }
GLAPI void glPopStringMarkerSCE(void)                   {}
GLAPI void glStringMarkerGREMEDY(GLsizei len, const void *string)
{ (void)len; (void)string; }

/* ── texture reference buffers SCE ───────────────────────────────── */

GLAPI void glTextureReferenceSCE(GLenum target, GLuint levels,
                                 GLuint baseWidth, GLuint baseHeight,
                                 GLuint baseDepth, GLenum internalFormat,
                                 GLuint pitch, GLintptr offset)
{
    (void)target;  (void)levels;  (void)baseWidth;
    (void)baseHeight; (void)baseDepth; (void)internalFormat;
    (void)pitch;   (void)offset;
}

GLAPI void glBufferSurfaceSCE(GLenum target, GLuint width, GLuint height,
                              GLenum multisampling, GLenum internalFormat,
                              GLenum usage)
{
    (void)target;  (void)width;  (void)height;
    (void)multisampling; (void)internalFormat; (void)usage;
}
