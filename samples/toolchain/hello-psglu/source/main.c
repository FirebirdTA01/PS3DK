/* PSGL utility library validation -- matrix math + string roundtrip. */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <PSGL/psglu.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x100000);

static int test_ortho(void)
{
    GLfloat m[16];
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2Df(0.0f, 640.0f, 0.0f, 480.0f);
    glGetFloatv(GL_PROJECTION_MATRIX, m);
    glMatrixMode(GL_MODELVIEW);
    if (fabsf(m[0] - 0.003125f) > 0.00001f || fabsf(m[5] - 0.0041667f) > 0.0001f ||
        fabsf(m[12] + 1.0f) > 0.001f || fabsf(m[13] + 1.0f) > 0.001f) {
        printf("hello-psglu: fail ortho m[0]=%f m[5]=%f m[12]=%f m[13]=%f\n",
               m[0], m[5], m[12], m[13]);
        return 0;
    }
    return 1;
}

static int test_perspective(void)
{
    GLfloat m[16];
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspectivef(45.0f, 1.333f, 1.0f, 100.0f);
    glGetFloatv(GL_PROJECTION_MATRIX, m);
    glMatrixMode(GL_MODELVIEW);
    if (fabsf(m[0] - 1.811f) > 0.02f) {
        printf("hello-psglu: fail perspective m[0]=%f\n", m[0]);
        return 0;
    }
    return 1;
}

static int test_lookat(void)
{
    GLfloat m[16];
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAtf(0.0f, 0.0f, 5.0f,
               0.0f, 0.0f, 0.0f,
               0.0f, 1.0f, 0.0f);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    /* forward=(0,0,-5) len=5 -> (0,0,-1), side=(1,0,0), up=(0,1,0) */
    /* m = [side up -forward 0]^T, translate -eye */
    if (fabsf(m[0] - 1.0f) > 0.001f || fabsf(m[5] - 1.0f) > 0.001f ||
        fabsf(m[10] - 1.0f) > 0.001f || fabsf(m[14] - (-5.0f)) > 0.01f) {
        printf("hello-psglu: fail lookat m[0]=%f m[5]=%f m[10]=%f m[14]=%f\n",
               m[0], m[5], m[10], m[14]);
        return 0;
    }
    return 1;
}

static int test_strings(void)
{
    if (strcmp((const char *)gluErrorString(GL_NO_ERROR), "no error") != 0)
        return 0;
    if (strcmp((const char *)gluErrorString(GL_INVALID_ENUM), "invalid enumerant") != 0)
        return 0;
    if (strcmp((const char *)gluGetString(GLU_VERSION), "1.3 PS3DK") != 0)
        return 0;
    return 1;
}

static int test_scale_image(void)
{
    unsigned char in[16] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255};
    unsigned char out[4];
    if (gluScaleImage(GL_RGBA, 2, 2, GL_UNSIGNED_BYTE, in,
                      1, 1, GL_UNSIGNED_BYTE, out) != 0)
        return 0;
    /* average of all 4 pixels = (128,128,64,255) */
    if (out[0] != 128 || out[1] != 128 || out[2] != 64 || out[3] != 255) {
        printf("hello-psglu: fail scale_image %u %u %u %u\n",
               out[0], out[1], out[2], out[3]);
        return 0;
    }
    return 1;
}

int main(void)
{
    PSGLinitOptions options = {0};
    int ok = 1;
    psglInit(&options);
    (void)psglCreateDeviceAuto(0, 0, 0);

    if (!test_ortho()) ok = 0;
    if (!test_perspective()) ok = 0;
    if (!test_lookat()) ok = 0;
    if (!test_strings()) ok = 0;
    if (!test_scale_image()) ok = 0;

    if (ok)
        printf("hello-psglu: ok\n");
    else
        printf("hello-psglu: fail\n");

    psglExit();
    return ok ? 0 : 1;
}
