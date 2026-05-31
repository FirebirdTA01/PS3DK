/* PSGL utility library implementation. */
#include <PSGL/psglu.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PSGLU_PI 3.14159265358979323846f

/* matrix helpers */

PSGLU_EXPORT void gluPerspectivef(GLfloat fovy, GLfloat aspect,
                                  GLfloat zNear, GLfloat zFar)
{
    GLfloat m[16];
    GLfloat f;
    if (zNear <= 0.0f || zFar <= zNear || aspect <= 0.0f) return;
    f = (GLfloat)(1.0 / tan((double)fovy * 0.5 * (PSGLU_PI / 180.0f)));
    memset(m, 0, sizeof(m));
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (zFar + zNear) / (zNear - zFar);
    m[11] = -1.0f;
    m[14] = (2.0f * zFar * zNear) / (zNear - zFar);
    glMultMatrixf(m);
}

PSGLU_EXPORT void gluLookAtf(GLfloat Xeye, GLfloat Yeye, GLfloat Zeye,
                             GLfloat Xtarget, GLfloat Ytarget, GLfloat Ztarget,
                             GLfloat Xup, GLfloat Yup, GLfloat Zup)
{
    GLfloat forward[3], side[3], up[3];
    GLfloat m[16];
    GLfloat len;

    forward[0] = Xtarget - Xeye;
    forward[1] = Ytarget - Yeye;
    forward[2] = Ztarget - Zeye;
    len = (GLfloat)sqrt((double)(forward[0]*forward[0] + forward[1]*forward[1] +
                                 forward[2]*forward[2]));
    if (len > 0.0f) { forward[0] /= len; forward[1] /= len; forward[2] /= len; }

    side[0] = forward[1]*Zup - forward[2]*Yup;
    side[1] = forward[2]*Xup - forward[0]*Zup;
    side[2] = forward[0]*Yup - forward[1]*Xup;
    len = (GLfloat)sqrt((double)(side[0]*side[0] + side[1]*side[1] +
                                 side[2]*side[2]));
    if (len > 0.0f) { side[0] /= len; side[1] /= len; side[2] /= len; }

    up[0] = side[1]*forward[2] - side[2]*forward[1];
    up[1] = side[2]*forward[0] - side[0]*forward[2];
    up[2] = side[0]*forward[1] - side[1]*forward[0];

    memset(m, 0, sizeof(m));
    m[0] = side[0];    m[4] = side[1];    m[8]  = side[2];
    m[1] = up[0];      m[5] = up[1];      m[9]  = up[2];
    m[2] = -forward[0];m[6] = -forward[1];m[10] = -forward[2];
    m[15] = 1.0f;

    glMultMatrixf(m);
    glTranslatef(-Xeye, -Yeye, -Zeye);
}

PSGLU_EXPORT void gluOrtho2Df(GLfloat left, GLfloat right,
                              GLfloat bottom, GLfloat top)
{
    GLfloat m[16];
    memset(m, 0, sizeof(m));
    m[0]  =  2.0f / (right - left);
    m[5]  =  2.0f / (top - bottom);
    m[10] = -1.0f;
    m[12] = -(right + left) / (right - left);
    m[13] = -(top + bottom) / (top - bottom);
    m[15] =  1.0f;
    glMultMatrixf(m);
}

/* string helpers */

PSGLU_EXPORT const GLubyte *gluErrorString(GLenum error)
{
    switch (error) {
    case GL_NO_ERROR:          return (const GLubyte *)"no error";
    case GL_INVALID_ENUM:      return (const GLubyte *)"invalid enumerant";
    case GL_INVALID_VALUE:     return (const GLubyte *)"invalid value";
    case GL_INVALID_OPERATION: return (const GLubyte *)"invalid operation";
    case GL_STACK_OVERFLOW:    return (const GLubyte *)"stack overflow";
    case GL_STACK_UNDERFLOW:   return (const GLubyte *)"stack underflow";
    case GL_OUT_OF_MEMORY:     return (const GLubyte *)"out of memory";
    default:                   return (const GLubyte *)"unknown error";
    }
}

PSGLU_EXPORT const GLubyte *gluGetString(GLenum name)
{
    switch (name) {
    case GLU_VERSION:    return (const GLubyte *)"1.3 PS3DK";
    case GLU_EXTENSIONS: return (const GLubyte *)"";
    default:             return (const GLubyte *)"";
    }
}

/* image helpers (box-filter average for downscale, RGBA-u8) */

PSGLU_EXPORT GLint gluScaleImage(GLenum format,
                                 GLsizei wIn, GLsizei hIn,
                                 GLenum typeIn, const void *dataIn,
                                 GLsizei wOut, GLsizei hOut,
                                 GLenum typeOut, GLvoid *dataOut)
{
    const unsigned char *src;
    unsigned char *dst;
    GLsizei x, y;
    if (!dataIn || !dataOut || wIn <= 0 || hIn <= 0 || wOut <= 0 || hOut <= 0)
        return GLU_INVALID_VALUE;
    if (format != GL_RGBA || typeIn != GL_UNSIGNED_BYTE ||
        typeOut != GL_UNSIGNED_BYTE)
        return GLU_INVALID_ENUM;

    src = (const unsigned char *)dataIn;
    dst = (unsigned char *)dataOut;

    for (y = 0; y < hOut; y++) {
        unsigned int sy0 = (unsigned int)((float)y / (float)hOut * (float)hIn);
        unsigned int sy1 = (unsigned int)((float)(y + 1) / (float)hOut * (float)hIn);
        if (sy1 <= sy0) sy1 = sy0 + 1u;
        if (sy1 > (unsigned int)hIn) sy1 = (unsigned int)hIn;
        for (x = 0; x < wOut; x++) {
            unsigned int sx0 = (unsigned int)((float)x / (float)wOut * (float)wIn);
            unsigned int sx1 = (unsigned int)((float)(x + 1) / (float)wOut * (float)wIn);
            unsigned long rsum = 0, gsum = 0, bsum = 0, asum = 0;
            unsigned long count = 0;
            unsigned int sx, sy;
            if (sx1 <= sx0) sx1 = sx0 + 1u;
            if (sx1 > (unsigned int)wIn) sx1 = (unsigned int)wIn;
            for (sy = sy0; sy < sy1; sy++) {
                for (sx = sx0; sx < sx1; sx++) {
                    unsigned int si = (sy * (unsigned int)wIn + sx) * 4u;
                    rsum += src[si + 0];
                    gsum += src[si + 1];
                    bsum += src[si + 2];
                    asum += src[si + 3];
                    count++;
                }
            }
            if (count) {
                unsigned int di = ((unsigned int)y * (unsigned int)wOut +
                                   (unsigned int)x) * 4u;
                dst[di + 0] = (unsigned char)((rsum + count / 2u) / count);
                dst[di + 1] = (unsigned char)((gsum + count / 2u) / count);
                dst[di + 2] = (unsigned char)((bsum + count / 2u) / count);
                dst[di + 3] = (unsigned char)((asum + count / 2u) / count);
            }
        }
    }
    return 0;
}

PSGLU_EXPORT GLint gluBuild2DMipmaps(GLenum target, GLint internalFormat,
                                     GLsizei width, GLsizei height,
                                     GLenum format, GLenum type,
                                     const void *data)
{
    GLint level = 0;
    GLsizei w = width, h = height;
    const void *src = data;
    if (!data || width <= 0 || height <= 0) return GLU_INVALID_VALUE;
    if (target != GL_TEXTURE_2D) return GLU_INVALID_ENUM;

    while (w >= 1 && h >= 1) {
        glTexImage2D(target, level, internalFormat, w, h, 0,
                     format, type, src);
        if (w == 1 && h == 1) break;
        {
            GLsizei nw = w > 1 ? w / 2 : 1;
            GLsizei nh = h > 1 ? h / 2 : 1;
            unsigned char *dst;
            const unsigned char *s;
            GLsizei x, y;
            if (type != GL_UNSIGNED_BYTE || format != GL_RGBA) break;
            dst = (unsigned char *)malloc((size_t)(nw * nh * 4));
            if (!dst) break;
            s = (const unsigned char *)src;
            for (y = 0; y < nh; y++) {
                GLsizei sy0 = y * 2, sy1 = sy0 + 1;
                if (sy1 >= h) sy1 = (GLsizei)(h - 1);
                for (x = 0; x < nw; x++) {
                    GLsizei sx0 = x * 2, sx1 = sx0 + 1;
                    unsigned int di, s0, s1, s2, s3, k;
                    if (sx1 >= w) sx1 = (GLsizei)(w - 1);
                    di = ((unsigned int)y * (unsigned int)nw + (unsigned int)x) * 4u;
                    s0 = ((unsigned int)sy0 * (unsigned int)w + (unsigned int)sx0) * 4u;
                    s1 = ((unsigned int)sy0 * (unsigned int)w + (unsigned int)sx1) * 4u;
                    s2 = ((unsigned int)sy1 * (unsigned int)w + (unsigned int)sx0) * 4u;
                    s3 = ((unsigned int)sy1 * (unsigned int)w + (unsigned int)sx1) * 4u;
                    for (k = 0; k < 4u; k++)
                        dst[di+k] = (unsigned char)(((unsigned int)s[s0+k] +
                            (unsigned int)s[s1+k] + (unsigned int)s[s2+k] +
                            (unsigned int)s[s3+k] + 2u) >> 2u);
                }
            }
            if (src != data) free((void *)src);
            src = dst;
            w = nw; h = nh;
            level++;
        }
    }
    if (src != data) free((void *)src);
    return 0;
}
