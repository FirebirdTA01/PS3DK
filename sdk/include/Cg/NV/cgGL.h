#ifndef PS3DK_CG_NV_CGGL_H
#define PS3DK_CG_NV_CGGL_H

#include <Cg/cg.h>
#include <GLES/gl.h>

#ifndef CGGL_API
#define CGGL_API
#endif
#ifndef CGGLENTRY
#define CGGLENTRY
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CGGLenum {
    CG_GL_MATRIX_IDENTITY = 0,
    CG_GL_MATRIX_TRANSPOSE = 1,
    CG_GL_MATRIX_INVERSE = 2,
    CG_GL_MATRIX_INVERSE_TRANSPOSE = 3,
    CG_GL_MODELVIEW_MATRIX,
    CG_GL_PROJECTION_MATRIX,
    CG_GL_TEXTURE_MATRIX,
    CG_GL_MODELVIEW_PROJECTION_MATRIX,
    CG_GL_VERTEX,
    CG_GL_FRAGMENT
} CGGLenum;

CGGL_API CGbool CGGLENTRY cgGLIsProfileSupported(CGprofile profile);
CGGL_API void CGGLENTRY cgGLEnableProfile(CGprofile profile);
CGGL_API void CGGLENTRY cgGLDisableProfile(CGprofile profile);
CGGL_API CGprofile CGGLENTRY cgGLGetLatestProfile(CGGLenum profile_type);
CGGL_API void CGGLENTRY cgGLSetOptimalOptions(CGprofile profile);
CGGL_API void CGGLENTRY cgGLLoadProgram(CGprogram program);
CGGL_API CGbool CGGLENTRY cgGLIsProgramLoaded(CGprogram program);
CGGL_API void CGGLENTRY cgGLBindProgram(CGprogram program);
CGGL_API void CGGLENTRY cgGLUnbindProgram(CGprofile profile);
CGGL_API GLuint CGGLENTRY cgGLGetProgramID(CGprogram program);
CGGL_API void CGGLENTRY cgGLSetParameter1f(CGparameter param, float x);
CGGL_API void CGGLENTRY cgGLSetParameter2f(CGparameter param, float x, float y);
CGGL_API void CGGLENTRY cgGLSetParameter3f(CGparameter param, float x, float y, float z);
CGGL_API void CGGLENTRY cgGLSetParameter4f(CGparameter param, float x, float y, float z, float w);
CGGL_API void CGGLENTRY cgGLSetParameter1fv(CGparameter param, const float *v);
CGGL_API void CGGLENTRY cgGLSetParameter2fv(CGparameter param, const float *v);
CGGL_API void CGGLENTRY cgGLSetParameter3fv(CGparameter param, const float *v);
CGGL_API void CGGLENTRY cgGLSetParameter4fv(CGparameter param, const float *v);
CGGL_API void CGGLENTRY cgGLSetParameter1d(CGparameter param, double x);
CGGL_API void CGGLENTRY cgGLSetParameter2d(CGparameter param, double x, double y);
CGGL_API void CGGLENTRY cgGLSetParameter3d(CGparameter param, double x, double y, double z);
CGGL_API void CGGLENTRY cgGLSetParameter4d(CGparameter param, double x, double y, double z, double w);
CGGL_API void CGGLENTRY cgGLSetParameter1dv(CGparameter param, const double *v);
CGGL_API void CGGLENTRY cgGLSetParameter2dv(CGparameter param, const double *v);
CGGL_API void CGGLENTRY cgGLSetParameter3dv(CGparameter param, const double *v);
CGGL_API void CGGLENTRY cgGLSetParameter4dv(CGparameter param, const double *v);
CGGL_API void CGGLENTRY cgGLGetParameter1f(CGparameter param, float *v);
CGGL_API void CGGLENTRY cgGLGetParameter2f(CGparameter param, float *v);
CGGL_API void CGGLENTRY cgGLGetParameter3f(CGparameter param, float *v);
CGGL_API void CGGLENTRY cgGLGetParameter4f(CGparameter param, float *v);
CGGL_API void CGGLENTRY cgGLGetParameter1d(CGparameter param, double *v);
CGGL_API void CGGLENTRY cgGLGetParameter2d(CGparameter param, double *v);
CGGL_API void CGGLENTRY cgGLGetParameter3d(CGparameter param, double *v);
CGGL_API void CGGLENTRY cgGLGetParameter4d(CGparameter param, double *v);
CGGL_API void CGGLENTRY cgGLSetParameterArray1f(CGparameter param, long offset, long nelements, const float *v);
CGGL_API void CGGLENTRY cgGLSetParameterArray2f(CGparameter param, long offset, long nelements, const float *v);
CGGL_API void CGGLENTRY cgGLSetParameterArray3f(CGparameter param, long offset, long nelements, const float *v);
CGGL_API void CGGLENTRY cgGLSetParameterArray4f(CGparameter param, long offset, long nelements, const float *v);
CGGL_API void CGGLENTRY cgGLSetParameterArray1d(CGparameter param, long offset, long nelements, const double *v);
CGGL_API void CGGLENTRY cgGLSetParameterArray2d(CGparameter param, long offset, long nelements, const double *v);
CGGL_API void CGGLENTRY cgGLSetParameterArray3d(CGparameter param, long offset, long nelements, const double *v);
CGGL_API void CGGLENTRY cgGLSetParameterArray4d(CGparameter param, long offset, long nelements, const double *v);
CGGL_API void CGGLENTRY cgGLGetParameterArray1f(CGparameter param, long offset, long nelements, float *v);
CGGL_API void CGGLENTRY cgGLGetParameterArray2f(CGparameter param, long offset, long nelements, float *v);
CGGL_API void CGGLENTRY cgGLGetParameterArray3f(CGparameter param, long offset, long nelements, float *v);
CGGL_API void CGGLENTRY cgGLGetParameterArray4f(CGparameter param, long offset, long nelements, float *v);
CGGL_API void CGGLENTRY cgGLGetParameterArray1d(CGparameter param, long offset, long nelements, double *v);
CGGL_API void CGGLENTRY cgGLGetParameterArray2d(CGparameter param, long offset, long nelements, double *v);
CGGL_API void CGGLENTRY cgGLGetParameterArray3d(CGparameter param, long offset, long nelements, double *v);
CGGL_API void CGGLENTRY cgGLGetParameterArray4d(CGparameter param, long offset, long nelements, double *v);
CGGL_API void CGGLENTRY cgGLSetParameterPointer(CGparameter param, GLint fsize, GLenum type, GLsizei stride, const GLvoid *pointer);
CGGL_API void CGGLENTRY cgGLEnableClientState(CGparameter param);
CGGL_API void CGGLENTRY cgGLDisableClientState(CGparameter param);
CGGL_API void CGGLENTRY cgGLSetMatrixParameterdr(CGparameter param, const double *matrix);
CGGL_API void CGGLENTRY cgGLSetMatrixParameterfr(CGparameter param, const float *matrix);
CGGL_API void CGGLENTRY cgGLSetMatrixParameterdc(CGparameter param, const double *matrix);
CGGL_API void CGGLENTRY cgGLSetMatrixParameterfc(CGparameter param, const float *matrix);
CGGL_API void CGGLENTRY cgGLGetMatrixParameterdr(CGparameter param, double *matrix);
CGGL_API void CGGLENTRY cgGLGetMatrixParameterfr(CGparameter param, float *matrix);
CGGL_API void CGGLENTRY cgGLGetMatrixParameterdc(CGparameter param, double *matrix);
CGGL_API void CGGLENTRY cgGLGetMatrixParameterfc(CGparameter param, float *matrix);
CGGL_API void CGGLENTRY cgGLSetStateMatrixParameter(CGparameter param, CGGLenum matrix, CGGLenum transform);
CGGL_API void CGGLENTRY cgGLSetMatrixParameterArrayfc(CGparameter param, long offset, long nelements, const float *matrices);
CGGL_API void CGGLENTRY cgGLSetMatrixParameterArrayfr(CGparameter param, long offset, long nelements, const float *matrices);
CGGL_API void CGGLENTRY cgGLSetMatrixParameterArraydc(CGparameter param, long offset, long nelements, const double *matrices);
CGGL_API void CGGLENTRY cgGLSetMatrixParameterArraydr(CGparameter param, long offset, long nelements, const double *matrices);
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArrayfc(CGparameter param, long offset, long nelements, float *matrices);
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArrayfr(CGparameter param, long offset, long nelements, float *matrices);
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArraydc(CGparameter param, long offset, long nelements, double *matrices);
CGGL_API void CGGLENTRY cgGLGetMatrixParameterArraydr(CGparameter param, long offset, long nelements, double *matrices);
CGGL_API void CGGLENTRY cgGLSetTextureParameter(CGparameter param, GLuint texobj);
CGGL_API GLuint CGGLENTRY cgGLGetTextureParameter(CGparameter param);
CGGL_API void CGGLENTRY cgGLEnableTextureParameter(CGparameter param);
CGGL_API void CGGLENTRY cgGLDisableTextureParameter(CGparameter param);
CGGL_API GLenum CGGLENTRY cgGLGetTextureEnum(CGparameter param);
CGGL_API void CGGLENTRY cgGLSetManageTextureParameters(CGcontext ctx, CGbool flag);
CGGL_API CGbool CGGLENTRY cgGLGetManageTextureParameters(CGcontext ctx);
CGGL_API void CGGLENTRY cgGLSetupSampler(CGparameter param, GLuint texobj);
CGGL_API void CGGLENTRY cgGLRegisterStates(CGcontext);
CGGL_API void CGGLENTRY cgGLEnableProgramProfiles( CGprogram program );
CGGL_API void CGGLENTRY cgGLDisableProgramProfiles( CGprogram program );
CGGL_API void CGGLENTRY cgGLSetDebugMode( CGbool debug );
CGGL_API CGbuffer CGGLENTRY cgGLCreateBuffer(CGcontext context, unsigned long size, GLenum bufferUsage);
CGGL_API GLuint CGGLENTRY cgGLGetBufferObject(CGbuffer buffer);

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_CG_NV_CGGL_H */
