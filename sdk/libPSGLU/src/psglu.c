/* psglu.c — PSGL utility stubs (Slice 1d link gate).
 */
#include <PSGL/psglu.h>
#include <string.h>

PSGLU_EXPORT void gluPerspectivef(GLfloat fovy, GLfloat aspect,
                                  GLfloat zNear, GLfloat zFar)
{ (void)fovy; (void)aspect; (void)zNear; (void)zFar; }

PSGLU_EXPORT void gluLookAtf(GLfloat Xeye, GLfloat Yeye, GLfloat Zeye,
                             GLfloat Xtarget, GLfloat Ytarget, GLfloat Ztarget,
                             GLfloat Xup, GLfloat Yup, GLfloat Zup)
{ (void)Xeye; (void)Yeye; (void)Zeye;
  (void)Xtarget; (void)Ytarget; (void)Ztarget;
  (void)Xup; (void)Yup; (void)Zup; }

PSGLU_EXPORT void gluOrtho2Df(GLfloat left, GLfloat right,
                              GLfloat bottom, GLfloat top)
{ (void)left; (void)right; (void)bottom; (void)top; }

PSGLU_EXPORT const GLubyte *gluErrorString(GLenum error)
{ (void)error; return (const GLubyte *)"no error"; }

PSGLU_EXPORT const GLubyte *gluGetString(GLenum name)
{ (void)name; return (const GLubyte *)"PS3DK GLU Stub"; }

PSGLU_EXPORT GLint gluScaleImage(GLenum format,
                                 GLsizei wIn, GLsizei hIn,
                                 GLenum typeIn, const void *dataIn,
                                 GLsizei wOut, GLsizei hOut,
                                 GLenum typeOut, GLvoid *dataOut)
{
    (void)format; (void)wIn; (void)hIn; (void)typeIn;
    (void)dataIn; (void)wOut; (void)hOut; (void)typeOut;
    (void)dataOut;
    return 0;
}

PSGLU_EXPORT GLint gluBuild2DMipmaps(GLenum target, GLint internalFormat,
                                     GLsizei width, GLsizei height,
                                     GLenum format, GLenum type,
                                     const void *data)
{
    (void)target; (void)internalFormat;
    (void)width; (void)height;
    (void)format; (void)type; (void)data;
    return 0;
}
