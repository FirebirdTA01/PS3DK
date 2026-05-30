#ifndef PS3DK_CG_CG_H
#define PS3DK_CG_CG_H

#include <Cg/NV/cg.h>

#define cgSetParameter1f cgGLSetParameter1f
#define cgSetParameter1fv cgGLSetParameter1fv
#define cgSetParameter1d cgGLSetParameter1d
#define cgSetParameter1dv cgGLSetParameter1dv
#define cgSetParameter2f cgGLSetParameter2f
#define cgSetParameter2fv cgGLSetParameter2fv
#define cgSetParameter2d cgGLSetParameter2d
#define cgSetParameter2dv cgGLSetParameter2dv
#define cgSetParameter3f cgGLSetParameter3f
#define cgSetParameter3fv cgGLSetParameter3fv
#define cgSetParameter3d cgGLSetParameter3d
#define cgSetParameter3dv cgGLSetParameter3dv
#define cgSetParameter4f cgGLSetParameter4f
#define cgSetParameter4fv cgGLSetParameter4fv
#define cgSetParameter4d cgGLSetParameter4d
#define cgSetParameter4dv cgGLSetParameter4dv
#define cgSetMatrixParameterfc cgGLSetMatrixParameterfc
#define cgSetMatrixParameterfr cgGLSetMatrixParameterfr
#define cgSetMatrixParameterdc cgGLSetMatrixParameterdc
#define cgSetMatrixParameterdr cgGLSetMatrixParameterdr

#ifdef __cplusplus
extern "C" {
#endif

CG_API void CGENTRY psglFXInit(void);
CG_API void CGENTRY psglFXExit(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_CG_CG_H */
