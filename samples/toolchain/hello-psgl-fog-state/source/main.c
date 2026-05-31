/* PSGL fog state set/query roundtrip. */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdio.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x100000);

static int test_fog_state(void)
{
    int ok = 1;
    GLfloat fv[4];
    GLint iv;
    GLboolean bv;

    /* set known non-default fog values */
    glFogf(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 10.0f);
    glFogf(GL_FOG_END, 100.0f);
    {
        GLfloat color[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        glFogfv(GL_FOG_COLOR, color);
    }

    /* query back */
    glGetIntegerv(GL_FOG_MODE, &iv);
    if (iv != GL_LINEAR) {
        printf("hello-psgl-fog-state: fail mode %d\n", iv);
        ok = 0;
    }
    glGetFloatv(GL_FOG_START, fv);
    if (fv[0] != 10.0f) {
        printf("hello-psgl-fog-state: fail start %f\n", fv[0]);
        ok = 0;
    }
    glGetFloatv(GL_FOG_END, fv);
    if (fv[0] != 100.0f) {
        printf("hello-psgl-fog-state: fail end %f\n", fv[0]);
        ok = 0;
    }
    glGetFloatv(GL_FOG_COLOR, fv);
    if (fv[0] != 0.5f || fv[1] != 0.5f || fv[2] != 0.5f || fv[3] != 0.5f) {
        printf("hello-psgl-fog-state: fail color\n");
        ok = 0;
    }

    /* enable/disable */
    glEnable(GL_FOG);
    glGetBooleanv(GL_FOG, &bv);
    if (!bv) {
        printf("hello-psgl-fog-state: fail enable\n");
        ok = 0;
    }
    glDisable(GL_FOG);
    glGetBooleanv(GL_FOG, &bv);
    if (bv) {
        printf("hello-psgl-fog-state: fail disable\n");
        ok = 0;
    }

    /* GL_EXP density */
    glFogf(GL_FOG_MODE, GL_EXP);
    glFogf(GL_FOG_DENSITY, 0.5f);
    glGetFloatv(GL_FOG_DENSITY, fv);
    if (fv[0] != 0.5f) {
        printf("hello-psgl-fog-state: fail density %f\n", fv[0]);
        ok = 0;
    }
    /* invalid mode is ignored */
    glFogf(GL_FOG_MODE, 999.0f);
    glGetIntegerv(GL_FOG_MODE, &iv);
    if (iv != GL_EXP) {
        printf("hello-psgl-fog-state: fail ignore-invalid-mode\n");
        ok = 0;
    }

    return ok;
}

int main(void)
{
    PSGLinitOptions options = {0};
    psglInit(&options);
    (void)psglCreateDeviceAuto(0, 0, 0);

    if (test_fog_state())
        printf("hello-psgl-fog-state: ok\n");
    else
        printf("hello-psgl-fog-state: fail\n");

    psglExit();
    return 0;
}
