/* PSGL command buffer call/return roundtrip. */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

SYS_PROCESS_PARAM(1001, 0x100000);

int main(void)
{
    PSGLinitOptions opts = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    GLuint cmd_offset;
    void *cmdbuf;
    void *recorded;

    psglInit(&opts);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    psglMakeCurrent(gl_context, device);

    cmdbuf = psglAllocateCommandBuffer(1024u * 1024u, &cmd_offset);
    if (!cmdbuf) {
        printf("hello-psgl-cmdbuf: fail alloc\n");
        psglExit();
        return 1;
    }

    /* record: clear to GREEN */
    psglBeginCommandRecord(cmdbuf, 1024u * 1024u);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    recorded = psglEndCommandRecord(PSGL_ADD_RETURN);
    if (!recorded) {
        printf("hello-psgl-cmdbuf: fail record\n");
        psglFreeCommandBuffer(cmdbuf);
        psglExit();
        return 1;
    }

    glViewport(0, 0, 1280, 720);

    /* capture loop: recorded GREEN over main-path BLUE */
    printf("hello-psgl-cmdbuf: ready\n");
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);
    for (int i = 0; i < 900; i++) {
        glClear(GL_COLOR_BUFFER_BIT);
        psglCallCommandBuffer(cmdbuf);
        psglSwap();
        sys_timer_usleep(16000);
    }
    printf("hello-psgl-cmdbuf: done\n");

    psglFreeCommandBuffer(cmdbuf);
    psglExit();
    return 0;
}
