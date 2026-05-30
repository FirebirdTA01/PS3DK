#include <PSGL/psgl.h>
#include <GLES/gl.h>
#include <ppu-lv2.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

SYS_PROCESS_PARAM(1001, 0x100000);

int main(void)
{
    PSGLinitOptions options = {0};
    psglInit(&options);

    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglGetCurrentContext();
    if (!context) context = psglCreateContext();
    if (!device || !context) {
        printf("hello-psgl-clear: PSGL init failed device=%p context=%p\n",
               (void *)device, (void *)context);
        psglExit();
        return 1;
    }

    psglMakeCurrent(context, device);
    GLuint width = 0;
    GLuint height = 0;
    psglGetRenderBufferDimensions(device, &width, &height);
    printf("hello-psgl-clear: ready %ux%u\n", width, height);
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    glClearColor(0.05f, 0.25f, 0.55f, 1.0f);
    glClearDepthf(1.0f);
    glClearStencil(0);

    for (int i = 0; i < 60; i++) {
        if (i < 2) printf("hello-psgl-clear: frame %d before\n", i);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        psglSwap();
        if (i < 2) printf("hello-psgl-clear: frame %d after\n", i);
        sys_timer_usleep(16000);
    }

    psglExit();
    printf("hello-psgl-clear: done\n");
    return 0;
}
