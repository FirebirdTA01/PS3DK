/* hello-psgl-link — PSGL link-gate test.
 *
 * Validates that #include <PSGL/psgl.h> compiles and that the
 * resulting TU links against libPSGL.a + libPSGLU.a.
 * Does not need to run; compile + link is the acceptance gate.
 */
#include <PSGL/psgl.h>
#include <PSGL/psglu.h>
#include <cell/cgb.h>
#include <cell/sscgpu.h>

int main(void)
{
    PSGLinitOptions initOpts = {0};
    psglInit(&initOpts);

    PSGLdevice *device = psglCreateDeviceAuto(/*colorFormat*/0,
                                               /*depthFormat*/0,
                                               /*multisampling*/0);
    (void)device;

    (void)cgCreateContext();
    (void)cgGLBindProgram(NULL);

    /* Link-gate check for libsscgpu.a */
    (void)cellSscGpuQueryAttr(NULL);
    (void)cellCgbGetSize(NULL);

    psglSwap();
    psglExit();

    return 0;
}
