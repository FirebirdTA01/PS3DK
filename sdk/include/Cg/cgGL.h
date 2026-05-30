#ifndef PS3DK_CG_CGGL_H
#define PS3DK_CG_CGGL_H

#include <Cg/NV/cgGL.h>

#ifdef __cplusplus
extern "C" {
#endif

CGGL_API void CGGLENTRY cgGLSetParameterElementFunc(CGparameter param, GLenum func, GLuint frequency);
CGGL_API void CGGLENTRY cgGLAttribPointer(GLuint index, GLint fsize, GLenum type, GLboolean normalize, GLsizei stride, const GLvoid *pointer);
CGGL_API void CGGLENTRY cgGLAttribElementFunc(GLuint index, GLenum func, GLuint frequency);
CGGL_API void CGGLENTRY cgGLAttribValues(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
CGGL_API void CGGLENTRY cgGLEnableAttrib(GLuint index);
CGGL_API void CGGLENTRY cgGLDisableAttrib(GLuint index);
CGGL_API void CGGLENTRY cgGLSetVertexRegister4fv(unsigned int index, const float *v);
CGGL_API void CGGLENTRY cgGLSetVertexRegisterBlock(unsigned int index, unsigned int count, const float *v);
CGGL_API void CGGLENTRY cgGLSetFragmentRegister4fv(unsigned int index, const float *v);
CGGL_API void CGGLENTRY cgGLSetFragmentRegisterBlock(unsigned int index, unsigned int count, const float *v);
CGGL_API void CGGLENTRY cgGLSetParameter1b(CGparameter param, CGbool v);
CGGL_API void CGGLENTRY cgGLSetProgramBoolVertexRegisters(CGprogram prog, unsigned int values);
CGGL_API void CGGLENTRY cgGLSetProgramBoolVertexRegistersBlock(CGprogram prog, unsigned int index, unsigned int count, const CGbool *v);
CGGL_API void CGGLENTRY cgGLSetBoolVertexRegisters(unsigned int values);
CGGL_API void CGGLENTRY cgGLSetBoolVertexRegistersSharingMask(CGcontext ctx, unsigned int values);
CGGL_API unsigned int CGGLENTRY cgGLGetRegisterCount(CGprogram prog);
CGGL_API void CGGLENTRY cgGLSetRegisterCount(CGprogram prog, const unsigned int regCount);
CGGL_API void CGGLENTRY cgRTCgcInit(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_CG_CGGL_H */
