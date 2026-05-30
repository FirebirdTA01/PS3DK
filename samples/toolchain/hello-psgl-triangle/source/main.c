#include <Cg/cgGL.h>
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <ppu-lv2.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "fpshader_fpo.h"
#include "vpshader_vpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define VP_PATH "/dev_hdd0/tmp/hello-psgl-triangle.vpo"
#define FP_PATH "/dev_hdd0/tmp/hello-psgl-triangle.fpo"

typedef struct Vertex {
    float position[3];
} Vertex;

static const Vertex k_vertices[3] = {
    {{-0.65f, -0.55f, 0.0f}},
    {{ 0.65f, -0.55f, 0.0f}},
    {{ 0.00f,  0.65f, 0.0f}}
};

static int write_blob(const char *path, const unsigned char *data,
                      unsigned int size)
{
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    if (fwrite(data, 1u, size, file) != size) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int write_shaders(void)
{
    return write_blob(VP_PATH, vpshader_vpo, vpshader_vpo_size) &&
           write_blob(FP_PATH, fpshader_fpo, fpshader_fpo_size);
}

int main(void)
{
    PSGLinitOptions options = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    CGcontext cg_context;
    CGprogram vp;
    CGprogram fp;
    CGparameter mvp_param;
    CGparameter color_param;
    GLuint vbo = 0u;
    const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    const float color[4] = {1.0f, 0.15f, 0.05f, 1.0f};

    if (!write_shaders()) {
        printf("hello-psgl-triangle: shader write failed\n");
        return 1;
    }

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    if (!device || !gl_context) {
        printf("hello-psgl-triangle: PSGL init failed\n");
        psglExit();
        return 1;
    }
    psglMakeCurrent(gl_context, device);

    cg_context = cgCreateContext();
    vp = cgCreateProgramFromFile(cg_context, CG_BINARY, VP_PATH,
                                 CG_PROFILE_SCE_VP_RSX, NULL, NULL);
    fp = cgCreateProgramFromFile(cg_context, CG_BINARY, FP_PATH,
                                 CG_PROFILE_SCE_FP_RSX, NULL, NULL);
    mvp_param = cgGetNamedParameter(vp, "modelViewProj");
    color_param = cgGetNamedParameter(fp, "solidColor");
    if (!cg_context || !vp || !fp || !mvp_param || !color_param) {
        printf("hello-psgl-triangle: Cg setup failed err=%d\n",
               (int)cgGetError());
        cgDestroyContext(cg_context);
        psglExit();
        return 1;
    }

    cgGLLoadProgram(vp);
    cgGLLoadProgram(fp);
    cgGLEnableProfile(CG_PROFILE_SCE_VP_RSX);
    cgGLEnableProfile(CG_PROFILE_SCE_FP_RSX);
    cgGLBindProgram(vp);
    cgGLBindProgram(fp);
    cgGLSetMatrixParameterfr(mvp_param, identity);
    cgGLSetParameter4fv(color_param, color);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_vertices), k_vertices,
                 GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), (const void *)0);
    glEnableClientState(GL_VERTEX_ARRAY);

    glViewport(0, 0, 1280, 720);
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);
    printf("hello-psgl-triangle: ready\n");
    for (int i = 0; i < 900; i++) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        psglSwap();
        sys_timer_usleep(16000);
    }

    printf("hello-psgl-triangle: done\n");
    cgDestroyContext(cg_context);
    psglExit();
    return 0;
}
