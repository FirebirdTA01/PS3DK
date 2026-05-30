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
    default:
        *params = 0; break;
    }
}

/* ── enable / disable / client-state ─────────────────────────────── */

GLAPI void glEnable(GLenum cap)        { (void)cap; }
GLAPI void glDisable(GLenum cap)       { (void)cap; }
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
{ (void)func; (void)ref; }
GLAPI void glAlphaFuncx(GLenum func, GLclampx ref)
{ (void)func; (void)ref; }

/* ── blend ───────────────────────────────────────────────────────── */

GLAPI void glBlendFunc(GLenum sfactor, GLenum dfactor)
{ (void)sfactor; (void)dfactor; }

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
{ (void)x; (void)y; (void)width; (void)height; }

/* ── depth ───────────────────────────────────────────────────────── */

GLAPI void glDepthFunc(GLenum func)       { (void)func; }
GLAPI void glDepthMask(GLboolean flag)    { (void)flag; }
GLAPI void glDepthRangef(GLclampf zNear, GLclampf zFar)
{ (void)zNear; (void)zFar; }
GLAPI void glDepthRangex(GLclampx zNear, GLclampx zFar)
{ (void)zNear; (void)zFar; }

/* ── stencil ─────────────────────────────────────────────────────── */

GLAPI void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{ (void)func; (void)ref; (void)mask; }
GLAPI void glStencilMask(GLuint mask)     { (void)mask; }
GLAPI void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{ (void)fail; (void)zfail; (void)zpass; }

/* ── cull face ───────────────────────────────────────────────────── */

GLAPI void glCullFace(GLenum mode)        { (void)mode; }
GLAPI void glFrontFace(GLenum mode)       { (void)mode; }

/* ── colour mask ─────────────────────────────────────────────────── */

GLAPI void glColorMask(GLboolean red, GLboolean green,
                       GLboolean blue, GLboolean alpha)
{ (void)red; (void)green; (void)blue; (void)alpha; }

/* ── colour ──────────────────────────────────────────────────────── */

GLAPI void glColor4f(GLfloat red, GLfloat green,
                     GLfloat blue, GLfloat alpha)
{ (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void glColor4x(GLfixed red, GLfixed green,
                     GLfixed blue, GLfixed alpha)
{ (void)red; (void)green; (void)blue; (void)alpha; }

/* ── normal ──────────────────────────────────────────────────────── */

GLAPI void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz)
{ (void)nx; (void)ny; (void)nz; }
GLAPI void glNormal3x(GLfixed nx, GLfixed ny, GLfixed nz)
{ (void)nx; (void)ny; (void)nz; }

/* ── shading ─────────────────────────────────────────────────────── */

GLAPI void glShadeModel(GLenum mode)      { (void)mode; }

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

GLAPI void glLogicOp(GLenum opcode)       { (void)opcode; }

/* ── pixel store ─────────────────────────────────────────────────── */

GLAPI void glPixelStorei(GLenum pname, GLint param)
{ (void)pname; (void)param; }

/* ── sample coverage ─────────────────────────────────────────────── */

GLAPI void glSampleCoverage(GLclampf value, GLboolean invert)
{ (void)value; (void)invert; }
GLAPI void glSampleCoveragex(GLclampx value, GLboolean invert)
{ (void)value; (void)invert; }

/* ── flush / finish ──────────────────────────────────────────────── */

GLAPI void glFlush(void)  {}
GLAPI void glFinish(void) {}

/* ── matrices ────────────────────────────────────────────────────── */

GLAPI void glMatrixMode(GLenum mode)      { (void)mode; }
GLAPI void glLoadIdentity(void)           {}
GLAPI void glPushMatrix(void)             {}
GLAPI void glPopMatrix(void)              {}
GLAPI void glLoadMatrixf(const GLfloat *m)  { (void)m; }
GLAPI void glLoadMatrixx(const GLfixed *m)  { (void)m; }
GLAPI void glMultMatrixf(const GLfloat *m)  { (void)m; }
GLAPI void glMultMatrixx(const GLfixed *m)  { (void)m; }
GLAPI void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{ (void)angle; (void)x; (void)y; (void)z; }
GLAPI void glRotatex(GLfixed angle, GLfixed x, GLfixed y, GLfixed z)
{ (void)angle; (void)x; (void)y; (void)z; }
GLAPI void glScalef(GLfloat x, GLfloat y, GLfloat z)
{ (void)x; (void)y; (void)z; }
GLAPI void glScalex(GLfixed x, GLfixed y, GLfixed z)
{ (void)x; (void)y; (void)z; }
GLAPI void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{ (void)x; (void)y; (void)z; }
GLAPI void glTranslatex(GLfixed x, GLfixed y, GLfixed z)
{ (void)x; (void)y; (void)z; }
GLAPI void glFrustumf(GLfloat l, GLfloat r, GLfloat b,
                      GLfloat t, GLfloat n, GLfloat f)
{ (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
GLAPI void glFrustumx(GLfixed l, GLfixed r, GLfixed b,
                      GLfixed t, GLfixed n, GLfixed f)
{ (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
GLAPI void glOrthof(GLfloat l, GLfloat r, GLfloat b,
                    GLfloat t, GLfloat n, GLfloat f)
{ (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
GLAPI void glOrthox(GLfixed l, GLfixed r, GLfixed b,
                    GLfixed t, GLfixed n, GLfixed f)
{ (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }

/* ── textures ────────────────────────────────────────────────────── */

GLAPI void glActiveTexture(GLenum texture)    { (void)texture; }
GLAPI void glBindTexture(GLenum target, GLuint texture)
{ (void)target; (void)texture; }

GLAPI void glGenTextures(GLsizei n, GLuint *textures)
{
    if (textures) gl_zero(textures, (size_t)n * sizeof(GLuint));
}

GLAPI void glDeleteTextures(GLsizei n, const GLuint *textures)
{ (void)n; (void)textures; }

GLAPI void glTexImage2D(GLenum target, GLint level,
                        GLint internalformat,
                        GLsizei width, GLsizei height, GLint border,
                        GLenum format, GLenum type,
                        const GLvoid *pixels)
{
    (void)target;  (void)level;  (void)internalformat;
    (void)width;   (void)height; (void)border;
    (void)format;  (void)type;   (void)pixels;
}

GLAPI void glTexSubImage2D(GLenum target, GLint level,
                           GLint xoffset, GLint yoffset,
                           GLsizei width, GLsizei height,
                           GLenum format, GLenum type,
                           const GLvoid *pixels)
{
    (void)target;  (void)level;  (void)xoffset;
    (void)yoffset; (void)width;  (void)height;
    (void)format;  (void)type;   (void)pixels;
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
{ (void)target; (void)pname; (void)param; }
GLAPI void glTexParameterx(GLenum target, GLenum pname, GLfixed param)
{ (void)target; (void)pname; (void)param; }

GLAPI void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{ (void)target; (void)pname; (void)param; }
GLAPI void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{ (void)target; (void)pname; (void)params; }
GLAPI void glTexEnvx(GLenum target, GLenum pname, GLfixed param)
{ (void)target; (void)pname; (void)param; }
GLAPI void glTexEnvxv(GLenum target, GLenum pname, const GLfixed *params)
{ (void)target; (void)pname; (void)params; }

GLAPI void glClientActiveTexture(GLenum texture) { (void)texture; }

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
{ (void)pname; (void)param; }
GLAPI void glFogfv(GLenum pname, const GLfloat *params)
{ (void)pname; (void)params; }
GLAPI void glFogx(GLenum pname, GLfixed param)
{ (void)pname; (void)param; }
GLAPI void glFogxv(GLenum pname, const GLfixed *params)
{ (void)pname; (void)params; }

/* ── lighting ────────────────────────────────────────────────────── */

GLAPI void glLightf(GLenum light, GLenum pname, GLfloat param)
{ (void)light; (void)pname; (void)param; }
GLAPI void glLightfv(GLenum light, GLenum pname, const GLfloat *params)
{ (void)light; (void)pname; (void)params; }
GLAPI void glLightx(GLenum light, GLenum pname, GLfixed param)
{ (void)light; (void)pname; (void)param; }
GLAPI void glLightxv(GLenum light, GLenum pname, const GLfixed *params)
{ (void)light; (void)pname; (void)params; }
GLAPI void glLightModelf(GLenum pname, GLfloat param)
{ (void)pname; (void)param; }
GLAPI void glLightModelfv(GLenum pname, const GLfloat *params)
{ (void)pname; (void)params; }
GLAPI void glLightModelx(GLenum pname, GLfixed param)
{ (void)pname; (void)param; }
GLAPI void glLightModelxv(GLenum pname, const GLfixed *params)
{ (void)pname; (void)params; }

/* ── material ────────────────────────────────────────────────────── */

GLAPI void glMaterialf(GLenum face, GLenum pname, GLfloat param)
{ (void)face; (void)pname; (void)param; }
GLAPI void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params)
{ (void)face; (void)pname; (void)params; }
GLAPI void glMaterialx(GLenum face, GLenum pname, GLfixed param)
{ (void)face; (void)pname; (void)param; }
GLAPI void glMaterialxv(GLenum face, GLenum pname, const GLfixed *params)
{ (void)face; (void)pname; (void)params; }

/* ── read pixels ─────────────────────────────────────────────────── */

GLAPI void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, GLvoid *pixels)
{
    (void)x; (void)y; (void)width; (void)height;
    (void)format; (void)type;
    if (pixels) gl_zero(pixels, (size_t)(width * height * 4));
}
