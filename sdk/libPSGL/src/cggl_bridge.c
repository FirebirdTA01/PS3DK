/* cggl_bridge.c — CgGL bridge API stubs (Slice 1d link gate).
 *
 * Covers NV/cgGL.h + Cg/cgGL.h surface.  All functions are safe NOPs
 * returning spec-correct defaults: CG_FALSE, NULL, or 0.
 */
#include <Cg/cgGL.h>
#include <string.h>

/* ── profile ─────────────────────────────────────────────────────── */

CGGL_API CGbool CGGLENTRY cgGLIsProfileSupported(CGprofile profile)
{ (void)profile; return CG_FALSE; }

CGGL_API void CGGLENTRY cgGLEnableProfile(CGprofile profile)
{ (void)profile; }

CGGL_API void CGGLENTRY cgGLDisableProfile(CGprofile profile)
{ (void)profile; }

CGGL_API CGprofile CGGLENTRY cgGLGetLatestProfile(CGGLenum profile_type)
{ (void)profile_type; return (CGprofile)0; }

CGGL_API void CGGLENTRY cgGLSetOptimalOptions(CGprofile profile)
{ (void)profile; }

/* ── program load / bind ─────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLLoadProgram(CGprogram program)
{ (void)program; }

CGGL_API CGbool CGGLENTRY cgGLIsProgramLoaded(CGprogram program)
{ (void)program; return CG_FALSE; }

CGGL_API void CGGLENTRY cgGLBindProgram(CGprogram program)
{ (void)program; }

CGGL_API void CGGLENTRY cgGLUnbindProgram(CGprofile profile)
{ (void)profile; }

CGGL_API GLuint CGGLENTRY cgGLGetProgramID(CGprogram program)
{ (void)program; return 0; }

/* ── parameter set / get ─────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetParameter1f(CGparameter p, float x)
{ (void)p; (void)x; }
CGGL_API void CGGLENTRY cgGLSetParameter2f(CGparameter p, float x, float y)
{ (void)p; (void)x; (void)y; }
CGGL_API void CGGLENTRY cgGLSetParameter3f(CGparameter p, float x, float y,
                                           float z)
{ (void)p; (void)x; (void)y; (void)z; }
CGGL_API void CGGLENTRY cgGLSetParameter4f(CGparameter p, float x, float y,
                                           float z, float w)
{ (void)p; (void)x; (void)y; (void)z; (void)w; }
CGGL_API void CGGLENTRY cgGLSetParameter1fv(CGparameter p, const float *v)
{ (void)p; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameter2fv(CGparameter p, const float *v)
{ (void)p; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameter3fv(CGparameter p, const float *v)
{ (void)p; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameter4fv(CGparameter p, const float *v)
{ (void)p; (void)v; }

CGGL_API void CGGLENTRY cgGLSetParameter1d(CGparameter p, double x)
{ (void)p; (void)x; }
CGGL_API void CGGLENTRY cgGLSetParameter2d(CGparameter p, double x, double y)
{ (void)p; (void)x; (void)y; }
CGGL_API void CGGLENTRY cgGLSetParameter3d(CGparameter p, double x, double y,
                                           double z)
{ (void)p; (void)x; (void)y; (void)z; }
CGGL_API void CGGLENTRY cgGLSetParameter4d(CGparameter p, double x, double y,
                                           double z, double w)
{ (void)p; (void)x; (void)y; (void)z; (void)w; }
CGGL_API void CGGLENTRY cgGLSetParameter1dv(CGparameter p, const double *v)
{ (void)p; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameter2dv(CGparameter p, const double *v)
{ (void)p; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameter3dv(CGparameter p, const double *v)
{ (void)p; (void)v; }
CGGL_API void CGGLENTRY cgGLSetParameter4dv(CGparameter p, const double *v)
{ (void)p; (void)v; }

CGGL_API void CGGLENTRY cgGLGetParameter1f(CGparameter p, float *v)
{ (void)p; if (v) *v = 0.0f; }
CGGL_API void CGGLENTRY cgGLGetParameter2f(CGparameter p, float *v)
{ (void)p; if (v) { v[0] = 0.0f; v[1] = 0.0f; } }
CGGL_API void CGGLENTRY cgGLGetParameter3f(CGparameter p, float *v)
{ (void)p; if (v) { v[0] = 0.0f; v[1] = 0.0f; v[2] = 0.0f; } }
CGGL_API void CGGLENTRY cgGLGetParameter4f(CGparameter p, float *v)
{ (void)p; if (v) memset(v, 0, 4 * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetParameter1d(CGparameter p, double *v)
{ (void)p; if (v) *v = 0.0; }
CGGL_API void CGGLENTRY cgGLGetParameter2d(CGparameter p, double *v)
{ (void)p; if (v) { v[0] = 0.0; v[1] = 0.0; } }
CGGL_API void CGGLENTRY cgGLGetParameter3d(CGparameter p, double *v)
{ (void)p; if (v) { v[0] = 0.0; v[1] = 0.0; v[2] = 0.0; } }
CGGL_API void CGGLENTRY cgGLGetParameter4d(CGparameter p, double *v)
{ (void)p; if (v) memset(v, 0, 4 * sizeof(double)); }

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
{ (void)p; (void)off; if (v) memset(v, 0, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray2f(CGparameter p, long off,
                                                long n, float *v)
{ (void)p; (void)off; if (v) memset(v, 0, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray3f(CGparameter p, long off,
                                                long n, float *v)
{ (void)p; (void)off; if (v) memset(v, 0, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray4f(CGparameter p, long off,
                                                long n, float *v)
{ (void)p; (void)off; if (v) memset(v, 0, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray1d(CGparameter p, long off,
                                                long n, double *v)
{ (void)p; (void)off; if (v) memset(v, 0, (size_t)n * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray2d(CGparameter p, long off,
                                                long n, double *v)
{ (void)p; (void)off; if (v) memset(v, 0, (size_t)n * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray3d(CGparameter p, long off,
                                                long n, double *v)
{ (void)p; (void)off; if (v) memset(v, 0, (size_t)n * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetParameterArray4d(CGparameter p, long off,
                                                long n, double *v)
{ (void)p; (void)off; if (v) memset(v, 0, (size_t)n * sizeof(double)); }

/* ── pointer / client state ──────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetParameterPointer(CGparameter param,
                                                GLint fsize, GLenum type,
                                                GLsizei stride,
                                                const GLvoid *pointer)
{ (void)param; (void)fsize; (void)type; (void)stride; (void)pointer; }

CGGL_API void CGGLENTRY cgGLEnableClientState(CGparameter param)
{ (void)param; }

CGGL_API void CGGLENTRY cgGLDisableClientState(CGparameter param)
{ (void)param; }

/* ── matrix parameters ───────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetMatrixParameterdr(CGparameter p,
                                                 const double *m)
{ (void)p; (void)m; }
CGGL_API void CGGLENTRY cgGLSetMatrixParameterfr(CGparameter p,
                                                 const float *m)
{ (void)p; (void)m; }
CGGL_API void CGGLENTRY cgGLSetMatrixParameterdc(CGparameter p,
                                                 const double *m)
{ (void)p; (void)m; }
CGGL_API void CGGLENTRY cgGLSetMatrixParameterfc(CGparameter p,
                                                 const float *m)
{ (void)p; (void)m; }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterdr(CGparameter p, double *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterfr(CGparameter p, float *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterdc(CGparameter p, double *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterfc(CGparameter p, float *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(float)); }

CGGL_API void CGGLENTRY cgGLSetStateMatrixParameter(CGparameter param,
                                                    CGGLenum matrix,
                                                    CGGLenum transform)
{ (void)param; (void)matrix; (void)transform; }

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
  if (m) memset(m, 0, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArrayfr(CGparameter p, long off,
                                                      long n, float *m)
{ (void)p; (void)off; (void)n;
  if (m) memset(m, 0, (size_t)n * sizeof(float)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArraydc(CGparameter p, long off,
                                                      long n, double *m)
{ (void)p; (void)off; (void)n;
  if (m) memset(m, 0, (size_t)n * sizeof(double)); }
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArraydr(CGparameter p, long off,
                                                      long n, double *m)
{ (void)p; (void)off; (void)n;
  if (m) memset(m, 0, (size_t)n * sizeof(double)); }

/* ── texture ─────────────────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLSetTextureParameter(CGparameter param,
                                                GLuint texobj)
{ (void)param; (void)texobj; }

CGGL_API GLuint CGGLENTRY cgGLGetTextureParameter(CGparameter param)
{ (void)param; return 0; }

CGGL_API void CGGLENTRY cgGLEnableTextureParameter(CGparameter param)
{ (void)param; }

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
{ (void)param; (void)texobj; }

/* ── state registration ──────────────────────────────────────────── */

CGGL_API void CGGLENTRY cgGLRegisterStates(CGcontext ctx) { (void)ctx; }

CGGL_API void CGGLENTRY cgGLEnableProgramProfiles(CGprogram program)
{ (void)program; }

CGGL_API void CGGLENTRY cgGLDisableProgramProfiles(CGprogram program)
{ (void)program; }

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
{ (void)index; (void)fsize; (void)type;
  (void)normalize; (void)stride; (void)pointer; }

CGGL_API void CGGLENTRY cgGLAttribElementFunc(GLuint index, GLenum func,
                                              GLuint frequency)
{ (void)index; (void)func; (void)frequency; }

CGGL_API void CGGLENTRY cgGLAttribValues(GLuint index, GLfloat x, GLfloat y,
                                         GLfloat z, GLfloat w)
{ (void)index; (void)x; (void)y; (void)z; (void)w; }

CGGL_API void CGGLENTRY cgGLEnableAttrib(GLuint index)   { (void)index; }
CGGL_API void CGGLENTRY cgGLDisableAttrib(GLuint index)  { (void)index; }

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
{ (void)prog; return 0; }

CGGL_API void CGGLENTRY cgGLSetRegisterCount(CGprogram prog,
                                             const unsigned int regCount)
{ (void)prog; (void)regCount; }

CGGL_API void CGGLENTRY cgRTCgcInit(void) {}
