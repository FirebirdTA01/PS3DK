#include "cg_internal.h"
#include "psgl_context.h"

#include <stddef.h>

static void cggl_zero(void *ptr, size_t size)
{
    unsigned char *out = (unsigned char *)ptr;
    while (size--) {
        *out++ = 0;
    }
}

static CGbool g_cggl_vertex_enabled = CG_FALSE;
static CGbool g_cggl_fragment_enabled = CG_FALSE;

static uint32_t cggl_texture_unit(PSGLcgParameter *parameter)
{
    if (!parameter) return 0u;
    if (parameter->resource_index >= CG_TEXUNIT0 &&
        parameter->resource_index <= CG_TEXUNIT15)
        return (uint32_t)(parameter->resource_index - CG_TEXUNIT0);
    return 0u;
}

static uint32_t cggl_attrib_index(PSGLcgParameter *parameter)
{
    if (!parameter) return PSGL_MAX_GENERIC_ATTRIBS;
    if (parameter->resource >= CG_ATTR0 &&
        parameter->resource <= CG_ATTR15)
        return (uint32_t)(parameter->resource - CG_ATTR0);
    return PSGL_MAX_GENERIC_ATTRIBS;
}

static void cggl_copy_float4(float out[4], float x, float y, float z, float w)
{
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

static void cggl_copy_double_to_float(float *out, const double *in,
                                      uint32_t count)
{
    if (!out || !in) return;
    for (uint32_t i = 0; i < count; i++)
        out[i] = (float)in[i];
}

static void cggl_mark_cg_dirty(void)
{
    PSGLcontext *context = psgl_context_current();
    if (context) context->dirty |= PSGL_DIRTY_CG;
}

/* ── profile ─────────────────────────────────────────────────────── */

CGGL_API CGbool CGGLENTRY cgGLIsProfileSupported(CGprofile profile)
{
    return (psgl_cg_profile_is_vertex(profile) ||
            psgl_cg_profile_is_fragment(profile)) ? CG_TRUE : CG_FALSE;
}

CGGL_API void CGGLENTRY cgGLEnableProfile(CGprofile profile)
{
    if (psgl_cg_profile_is_vertex(profile)) g_cggl_vertex_enabled = CG_TRUE;
    if (psgl_cg_profile_is_fragment(profile)) g_cggl_fragment_enabled = CG_TRUE;
}

CGGL_API void CGGLENTRY cgGLDisableProfile(CGprofile profile)
{
    if (psgl_cg_profile_is_vertex(profile)) g_cggl_vertex_enabled = CG_FALSE;
    if (psgl_cg_profile_is_fragment(profile)) g_cggl_fragment_enabled = CG_FALSE;
}

CGGL_API CGprofile CGGLENTRY cgGLGetLatestProfile(CGGLenum profile_type)
{
    if (profile_type == CG_GL_VERTEX) return CG_PROFILE_SCE_VP_RSX;
    if (profile_type == CG_GL_FRAGMENT) return CG_PROFILE_SCE_FP_RSX;
    return CG_PROFILE_UNKNOWN;
}

CGGL_API void CGGLENTRY cgGLSetOptimalOptions(CGprofile profile)
{ (void)profile; }

/* ── program load / bind ─────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLLoadProgram(CGprogram program)
{
    PSGLcgProgram *p = psgl_cg_program(program);
    if (p) {
        p->loaded = CG_TRUE;
        cggl_mark_cg_dirty();
    }
}

CGGL_API CGbool CGGLENTRY cgGLIsProgramLoaded(CGprogram program)
{
    PSGLcgProgram *p = psgl_cg_program(program);
    return (p && p->loaded) ? CG_TRUE : CG_FALSE;
}

CGGL_API void CGGLENTRY cgGLBindProgram(CGprogram program)
{
    PSGLcgProgram *p = psgl_cg_program(program);
    if (!p || !p->loaded) return;
    psgl_context_bind_cg_program(program, p->profile);
}

CGGL_API void CGGLENTRY cgGLUnbindProgram(CGprofile profile)
{ psgl_context_bind_cg_program(NULL, profile); }

CGGL_API GLuint CGGLENTRY cgGLGetProgramID(CGprogram program)
{
    PSGLcgProgram *p = psgl_cg_program(program);
    return p ? (GLuint)(uintptr_t)p : 0u;
}

/* ── parameter set / get ─────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetParameter1f(CGparameter p, float x)
{
    float v[1] = {x};
    psgl_cg_set_parameter_floats(psgl_cg_parameter(p), v, 1u);
    cggl_mark_cg_dirty();
}
CGGL_API void CGGLENTRY cgGLSetParameter2f(CGparameter p, float x, float y)
{
    float v[2] = {x, y};
    psgl_cg_set_parameter_floats(psgl_cg_parameter(p), v, 2u);
    cggl_mark_cg_dirty();
}
CGGL_API void CGGLENTRY cgGLSetParameter3f(CGparameter p, float x, float y,
                                           float z)
{
    float v[3] = {x, y, z};
    psgl_cg_set_parameter_floats(psgl_cg_parameter(p), v, 3u);
    cggl_mark_cg_dirty();
}
CGGL_API void CGGLENTRY cgGLSetParameter4f(CGparameter p, float x, float y,
                                           float z, float w)
{
    float v[4];
    cggl_copy_float4(v, x, y, z, w);
    psgl_cg_set_parameter_floats(psgl_cg_parameter(p), v, 4u);
    cggl_mark_cg_dirty();
}
CGGL_API void CGGLENTRY cgGLSetParameter1fv(CGparameter p, const float *v)
{ psgl_cg_set_parameter_floats(psgl_cg_parameter(p), v, 1u); cggl_mark_cg_dirty(); }
CGGL_API void CGGLENTRY cgGLSetParameter2fv(CGparameter p, const float *v)
{ psgl_cg_set_parameter_floats(psgl_cg_parameter(p), v, 2u); cggl_mark_cg_dirty(); }
CGGL_API void CGGLENTRY cgGLSetParameter3fv(CGparameter p, const float *v)
{ psgl_cg_set_parameter_floats(psgl_cg_parameter(p), v, 3u); cggl_mark_cg_dirty(); }
CGGL_API void CGGLENTRY cgGLSetParameter4fv(CGparameter p, const float *v)
{ psgl_cg_set_parameter_floats(psgl_cg_parameter(p), v, 4u); cggl_mark_cg_dirty(); }

CGGL_API void CGGLENTRY cgGLSetParameter1d(CGparameter p, double x)
{ double d[1] = {x}; cgGLSetParameter1dv(p, d); }
CGGL_API void CGGLENTRY cgGLSetParameter2d(CGparameter p, double x, double y)
{ double d[2] = {x, y}; cgGLSetParameter2dv(p, d); }
CGGL_API void CGGLENTRY cgGLSetParameter3d(CGparameter p, double x, double y,
                                           double z)
{ double d[3] = {x, y, z}; cgGLSetParameter3dv(p, d); }
CGGL_API void CGGLENTRY cgGLSetParameter4d(CGparameter p, double x, double y,
                                           double z, double w)
{ double d[4] = {x, y, z, w}; cgGLSetParameter4dv(p, d); }
CGGL_API void CGGLENTRY cgGLSetParameter1dv(CGparameter p, const double *v)
{ float f[1]; cggl_copy_double_to_float(f, v, 1u); cgGLSetParameter1fv(p, f); }
CGGL_API void CGGLENTRY cgGLSetParameter2dv(CGparameter p, const double *v)
{ float f[2]; cggl_copy_double_to_float(f, v, 2u); cgGLSetParameter2fv(p, f); }
CGGL_API void CGGLENTRY cgGLSetParameter3dv(CGparameter p, const double *v)
{ float f[3]; cggl_copy_double_to_float(f, v, 3u); cgGLSetParameter3fv(p, f); }
CGGL_API void CGGLENTRY cgGLSetParameter4dv(CGparameter p, const double *v)
{ float f[4]; cggl_copy_double_to_float(f, v, 4u); cgGLSetParameter4fv(p, f); }

CGGL_API void CGGLENTRY cgGLGetParameter1f(CGparameter p, float *v)
{ psgl_cg_get_parameter_floats(psgl_cg_parameter(p), v, 1u); }
CGGL_API void CGGLENTRY cgGLGetParameter2f(CGparameter p, float *v)
{ psgl_cg_get_parameter_floats(psgl_cg_parameter(p), v, 2u); }
CGGL_API void CGGLENTRY cgGLGetParameter3f(CGparameter p, float *v)
{ psgl_cg_get_parameter_floats(psgl_cg_parameter(p), v, 3u); }
CGGL_API void CGGLENTRY cgGLGetParameter4f(CGparameter p, float *v)
{ psgl_cg_get_parameter_floats(psgl_cg_parameter(p), v, 4u); }
CGGL_API void CGGLENTRY cgGLGetParameter1d(CGparameter p, double *v)
{ (void)p; if (v) *v = 0.0; }
CGGL_API void CGGLENTRY cgGLGetParameter2d(CGparameter p, double *v)
{ (void)p; if (v) { v[0] = 0.0; v[1] = 0.0; } }
CGGL_API void CGGLENTRY cgGLGetParameter3d(CGparameter p, double *v)
{ (void)p; if (v) { v[0] = 0.0; v[1] = 0.0; v[2] = 0.0; } }
CGGL_API void CGGLENTRY cgGLGetParameter4d(CGparameter p, double *v)
{ (void)p; if (v) cggl_zero(v, 4 * sizeof(double)); }

/* ── parameter arrays ────────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetParameterArray1f(CGparameter p, long off,
                                                long n, const float *v)
{ (void)p; (void)off; (void)n; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameterArray2f(CGparameter p, long off,
                                                long n, const float *v)
{ (void)p; (void)off; (void)n; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameterArray3f(CGparameter p, long off,
                                                long n, const float *v)
{ (void)p; (void)off; (void)n; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameterArray4f(CGparameter p, long off,
                                                long n, const float *v)
{ (void)p; (void)off; (void)n; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameterArray1d(CGparameter p, long off,
                                                long n, const double *v)
{ (void)p; (void)off; (void)n; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameterArray2d(CGparameter p, long off,
                                                long n, const double *v)
{ (void)p; (void)off; (void)n; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameterArray3d(CGparameter p, long off,
                                                long n, const double *v)
{ (void)p; (void)off; (void)n; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameterArray4d(CGparameter p, long off,
                                                long n, const double *v)
{ (void)p; (void)off; (void)n; (void)v; }
CGGL_API void CGGLENTRY cgGLGetParameterArray1f(CGparameter p, long off,
                                                long n, float *v)
{ (void)p; (void)off; if (v) cggl_zero(v, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray2f(CGparameter p, long off,
                                                long n, float *v)
{ (void)p; (void)off; if (v) cggl_zero(v, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray3f(CGparameter p, long off,
                                                long n, float *v)
{ (void)p; (void)off; if (v) cggl_zero(v, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray4f(CGparameter p, long off,
                                                long n, float *v)
{ (void)p; (void)off; if (v) cggl_zero(v, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray1d(CGparameter p, long off,
                                                long n, double *v)
{ (void)p; (void)off; if (v) cggl_zero(v, (size_t)n * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray2d(CGparameter p, long off,
                                                long n, double *v)
{ (void)p; (void)off; if (v) cggl_zero(v, (size_t)n * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray3d(CGparameter p, long off,
                                                long n, double *v)
{ (void)p; (void)off; if (v) cggl_zero(v, (size_t)n * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray4d(CGparameter p, long off,
                                                long n, double *v)
{ (void)p; (void)off; if (v) cggl_zero(v, (size_t)n * sizeof(double)); }

/* ── pointer / client state ──────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetParameterPointer(CGparameter param,
                                                GLint fsize, GLenum type,
                                                GLsizei stride,
                                                const GLvoid *pointer)
{
    uint32_t index = cggl_attrib_index(psgl_cg_parameter(param));
    if (index >= PSGL_MAX_GENERIC_ATTRIBS) return;
    psgl_context_set_generic_attrib_pointer(index, fsize, type,
                                            stride, pointer);
}

CGGL_API void CGGLENTRY cgGLEnableClientState(CGparameter param)
{
    uint32_t index = cggl_attrib_index(psgl_cg_parameter(param));
    if (index >= PSGL_MAX_GENERIC_ATTRIBS) return;
    psgl_context_set_generic_attrib_enabled(index, GL_TRUE);
}

CGGL_API void CGGLENTRY cgGLDisableClientState(CGparameter param)
{
    uint32_t index = cggl_attrib_index(psgl_cg_parameter(param));
    if (index >= PSGL_MAX_GENERIC_ATTRIBS) return;
    psgl_context_set_generic_attrib_enabled(index, GL_FALSE);
}

/* ── matrix parameters ───────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetMatrixParameterdr(CGparameter p,
                                                 const double *m)
{ float f[16]; cggl_copy_double_to_float(f, m, 16u); cgGLSetMatrixParameterfr(p, f); }
CGGL_API void CGGLENTRY cgGLSetMatrixParameterfr(CGparameter p,
                                                 const float *m)
{ psgl_cg_set_parameter_matrix(psgl_cg_parameter(p), m); cggl_mark_cg_dirty(); }
CGGL_API void CGGLENTRY cgGLSetMatrixParameterdc(CGparameter p,
                                                 const double *m)
{ cgGLSetMatrixParameterdr(p, m); }
CGGL_API void CGGLENTRY cgGLSetMatrixParameterfc(CGparameter p,
                                                 const float *m)
{ cgGLSetMatrixParameterfr(p, m); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterdr(CGparameter p, double *m)
{ (void)p; if (m) cggl_zero(m, 16 * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterfr(CGparameter p, float *m)
{ psgl_cg_get_parameter_matrix(psgl_cg_parameter(p), m); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterdc(CGparameter p, double *m)
{ (void)p; if (m) cggl_zero(m, 16 * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterfc(CGparameter p, float *m)
{ (void)p; if (m) cggl_zero(m, 16 * sizeof(float)); }

CGGL_API void CGGLENTRY cgGLSetStateMatrixParameter(CGparameter param,
                                                    CGGLenum matrix,
                                                    CGGLenum transform)
{
    PSGLcgParameter *p = psgl_cg_parameter(param);
    if (!p) return;
    cggl_zero(p->value, 16u * sizeof(float));
    p->value[0] = 1.0f;
    p->value[5] = 1.0f;
    p->value[10] = 1.0f;
    p->value[15] = 1.0f;
    p->value_count = 16u;
    p->state_matrix = CG_TRUE;
    p->state_matrix_source = matrix;
    p->state_matrix_transform = transform;
    p->dirty = CG_TRUE;
    cggl_mark_cg_dirty();
}

CGGL_API void CGGLENTRY cgGLSetMatrixParameterArrayfc(CGparameter p, long off,
                                                      long n,
                                                      const float *m)
{ (void)p; (void)off; (void)n; (void)m; }
CGGL_API void CGGLENTRY cgGLSetMatrixParameterArrayfr(CGparameter p, long off,
                                                      long n,
                                                      const float *m)
{ (void)p; (void)off; (void)n; (void)m; }
CGGL_API void CGGLENTRY cgGLSetMatrixParameterArraydc(CGparameter p, long off,
                                                      long n,
                                                      const double *m)
{ (void)p; (void)off; (void)n; (void)m; }
CGGL_API void CGGLENTRY cgGLSetMatrixParameterArraydr(CGparameter p, long off,
                                                      long n,
                                                      const double *m)
{ (void)p; (void)off; (void)n; (void)m; }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArrayfc(CGparameter p, long off,
                                                      long n, float *m)
{ (void)p; (void)off; (void)n;
  if (m) cggl_zero(m, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArrayfr(CGparameter p, long off,
                                                      long n, float *m)
{ (void)p; (void)off; (void)n;
  if (m) cggl_zero(m, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArraydc(CGparameter p, long off,
                                                      long n, double *m)
{ (void)p; (void)off; (void)n;
  if (m) cggl_zero(m, (size_t)n * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArraydr(CGparameter p, long off,
                                                      long n, double *m)
{ (void)p; (void)off; (void)n;
  if (m) cggl_zero(m, (size_t)n * sizeof(double)); }

/* ── texture ─────────────────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetTextureParameter(CGparameter param,
                                                GLuint texobj)
{
    PSGLcgParameter *p = psgl_cg_parameter(param);
    uint32_t unit = cggl_texture_unit(p);
    psgl_context_active_texture(GL_TEXTURE0 + unit);
    psgl_context_bind_texture(GL_TEXTURE_2D, texobj);
}

CGGL_API GLuint CGGLENTRY cgGLGetTextureParameter(CGparameter param)
{ (void)param; return 0; }

CGGL_API void CGGLENTRY cgGLEnableTextureParameter(CGparameter param)
{
    PSGLcgParameter *p = psgl_cg_parameter(param);
    uint32_t unit = cggl_texture_unit(p);
    psgl_context_active_texture(GL_TEXTURE0 + unit);
}

CGGL_API void CGGLENTRY cgGLDisableTextureParameter(CGparameter param)
{ (void)param; }

CGGL_API GLenum CGGLENTRY cgGLGetTextureEnum(CGparameter param)
{ (void)param; return GL_TEXTURE_2D; }

CGGL_API void CGGLENTRY cgGLSetManageTextureParameters(CGcontext ctx,
                                                       CGbool flag)
{ (void)ctx; (void)flag; }

CGGL_API CGbool CGGLENTRY cgGLGetManageTextureParameters(CGcontext ctx)
{ (void)ctx; return CG_FALSE; }

CGGL_API void CGGLENTRY cgGLSetupSampler(CGparameter param, GLuint texobj)
{ cgGLSetTextureParameter(param, texobj); }

/* ── state registration ──────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLRegisterStates(CGcontext ctx) { (void)ctx; }

CGGL_API void CGGLENTRY cgGLEnableProgramProfiles(CGprogram program)
{
    PSGLcgProgram *p = psgl_cg_program(program);
    if (p) cgGLEnableProfile(p->profile);
}

CGGL_API void CGGLENTRY cgGLDisableProgramProfiles(CGprogram program)
{
    PSGLcgProgram *p = psgl_cg_program(program);
    if (p) cgGLDisableProfile(p->profile);
}

CGGL_API void CGGLENTRY cgGLSetDebugMode(CGbool debug) { (void)debug; }

/* ── buffer ──────────────────────────────────────────────────────── */

CGGL_API CGbuffer CGGLENTRY cgGLCreateBuffer(CGcontext context,
                                             unsigned long size,
                                             GLenum bufferUsage)
{ (void)context; (void)size; (void)bufferUsage; return NULL; }

CGGL_API GLuint CGGLENTRY cgGLGetBufferObject(CGbuffer buffer)
{ (void)buffer; return 0; }

/* ── top-level cgGL.h extras (PS3 bridge) ────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetParameterElementFunc(CGparameter param,
                                                    GLenum func,
                                                    GLuint frequency)
{ (void)param; (void)func; (void)frequency; }

CGGL_API void CGGLENTRY cgGLAttribPointer(GLuint index, GLint fsize,
                                          GLenum type, GLboolean normalize,
                                          GLsizei stride,
                                          const GLvoid *pointer)
{
    (void)normalize;
    psgl_context_set_generic_attrib_pointer(index, fsize, type,
                                            stride, pointer);
}

CGGL_API void CGGLENTRY cgGLAttribElementFunc(GLuint index, GLenum func,
                                              GLuint frequency)
{ (void)index; (void)func; (void)frequency; }

CGGL_API void CGGLENTRY cgGLAttribValues(GLuint index, GLfloat x, GLfloat y,
                                         GLfloat z, GLfloat w)
{ (void)index; (void)x; (void)y; (void)z; (void)w; }

CGGL_API void CGGLENTRY cgGLEnableAttrib(GLuint index)
{ psgl_context_set_generic_attrib_enabled(index, GL_TRUE); }

CGGL_API void CGGLENTRY cgGLDisableAttrib(GLuint index)
{ psgl_context_set_generic_attrib_enabled(index, GL_FALSE); }

CGGL_API void CGGLENTRY cgGLSetVertexRegister4fv(unsigned int index,
                                                 const float *v)
{ (void)index; (void)v; }

CGGL_API void CGGLENTRY cgGLSetVertexRegisterBlock(unsigned int index,
                                                   unsigned int count,
                                                   const float *v)
{ (void)index; (void)count; (void)v; }

CGGL_API void CGGLENTRY cgGLSetFragmentRegister4fv(unsigned int index,
                                                   const float *v)
{ (void)index; (void)v; }

CGGL_API void CGGLENTRY cgGLSetFragmentRegisterBlock(unsigned int index,
                                                     unsigned int count,
                                                     const float *v)
{ (void)index; (void)count; (void)v; }

CGGL_API void CGGLENTRY cgGLSetParameter1b(CGparameter param, CGbool v)
{ (void)param; (void)v; }

CGGL_API void CGGLENTRY cgGLSetProgramBoolVertexRegisters(CGprogram prog,
                                                          unsigned int values)
{ (void)prog; (void)values; }

CGGL_API void CGGLENTRY cgGLSetProgramBoolVertexRegistersBlock(
    CGprogram prog, unsigned int index, unsigned int count, const CGbool *v)
{ (void)prog; (void)index; (void)count; (void)v; }

CGGL_API void CGGLENTRY cgGLSetBoolVertexRegisters(unsigned int values)
{ (void)values; }

CGGL_API void CGGLENTRY cgGLSetBoolVertexRegistersSharingMask(
    CGcontext ctx, unsigned int values)
{ (void)ctx; (void)values; }

CGGL_API unsigned int CGGLENTRY cgGLGetRegisterCount(CGprogram prog)
{
    PSGLcgProgram *p = psgl_cg_program(prog);
    if (!p || p->cgb_profile != CELL_CGB_PROFILE_VERTEX) return 0u;
    return cellCgbGetVertexConstantCount(&p->cgb_program);
}

CGGL_API void CGGLENTRY cgGLSetRegisterCount(CGprogram prog,
                                             const unsigned int regCount)
{ (void)prog; (void)regCount; }

CGGL_API void CGGLENTRY cgRTCgcInit(void) {}
