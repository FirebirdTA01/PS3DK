#ifndef PS3DK_PSGLU_H
#define PS3DK_PSGLU_H

#include <PSGL/export.h>
#include <PSGL/psgl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLU_INVALID_ENUM      100900
#define GLU_INVALID_VALUE     100901
#define GLU_OUT_OF_MEMORY     100902
#define GLU_INVALID_OPERATION 100904

#define GLU_VERSION    100800
#define GLU_EXTENSIONS 100801

PSGLU_EXPORT void gluPerspectivef(GLfloat fovy, GLfloat aspect, GLfloat zNear, GLfloat zFar);
PSGLU_EXPORT void gluLookAtf(GLfloat Xeye, GLfloat Yeye, GLfloat Zeye,
                             GLfloat Xtarget, GLfloat Ytarget, GLfloat Ztarget,
                             GLfloat Xup, GLfloat Yup, GLfloat Zup);
PSGLU_EXPORT void gluOrtho2Df(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top);
PSGLU_EXPORT const GLubyte *gluErrorString(GLenum error);
PSGLU_EXPORT const GLubyte *gluGetString(GLenum name);
PSGLU_EXPORT GLint gluScaleImage(GLenum format, GLsizei wIn, GLsizei hIn,
                                 GLenum typeIn, const void *dataIn,
                                 GLsizei wOut, GLsizei hOut, GLenum typeOut,
                                 GLvoid *dataOut);
PSGLU_EXPORT GLint gluBuild2DMipmaps(GLenum target, GLint internalFormat,
                                     GLsizei width, GLsizei height,
                                     GLenum format, GLenum type, const void *data);

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_PSGLU_H */
