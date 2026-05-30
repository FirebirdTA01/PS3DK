#include <Cg/cgGL.h>
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <ppu-lv2.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "fp_blue_alpha_fpo.h"
#include "fp_red_fpo.h"
#include "vpshader_vpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define VP_PATH "/dev_hdd0/tmp/hello-psgl-blend.vpo"
#define FP_RED_PATH "/dev_hdd0/tmp/hello-psgl-blend-red.fpo"
#define FP_BLUE_PATH "/dev_hdd0/tmp/hello-psgl-blend-blue.fpo"

typedef struct Vertex {
    float position[3];
} Vertex;

static const Vertex k_vertices[6] = {
    {{-0.70f, -0.60f, 0.0f}},
    {{ 0.30f, -0.60f, 0.0f}},
    {{-0.20f,  0.70f, 0.0f}},
    {{-0.30f, -0.50f, 0.0f}},
    {{ 0.70f, -0.50f, 0.0f}},
    {{ 0.20f,  0.80f, 0.0f}}
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
           write_blob(FP_RED_PATH, fp_red_fpo, fp_red_fpo_size) &&
           write_blob(FP_BLUE_PATH, fp_blue_alpha_fpo, fp_blue_alpha_fpo_size);
}

int main(void)
{
    PSGLinitOptions options = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    CGcontext cg_context;
    CGprogram vp;
    CGprogram fp_red;
    CGprogram fp_blue;
    CGparameter mvp_param;
    GLuint vbo = 0u;
    const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    if (!write_shaders()) {
        printf("hello-psgl-blend: shader write failed\n");
        return 1;
    }

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    if (!device || !gl_context) {
        printf("hello-psgl-blend: PSGL init failed\n");
        psglExit();
        return 1;
    }
    psglMakeCurrent(gl_context, device);

    cg_context = cgCreateContext();
    vp = cgCreateProgramFromFile(cg_context, CG_BINARY, VP_PATH,
                                 CG_PROFILE_SCE_VP_RSX, NULL, NULL);
    fp_red = cgCreateProgramFromFile(cg_context, CG_BINARY, FP_RED_PATH,
                                     CG_PROFILE_SCE_FP_RSX, NULL, NULL);
    fp_blue = cgCreateProgramFromFile(cg_context, CG_BINARY, FP_BLUE_PATH,
                                      CG_PROFILE_SCE_FP_RSX, NULL, NULL);
    mvp_param = cgGetNamedParameter(vp, "modelViewProj");
    if (!cg_context || !vp || !fp_red || !fp_blue || !mvp_param) {
        printf("hello-psgl-blend: Cg setup failed err=%d\n", (int)cgGetError());
        cgDestroyContext(cg_context);
        psglExit();
        return 1;
    }

    cgGLLoadProgram(vp);
    cgGLLoadProgram(fp_red);
    cgGLLoadProgram(fp_blue);
    cgGLEnableProfile(CG_PROFILE_SCE_VP_RSX);
    cgGLEnableProfile(CG_PROFILE_SCE_FP_RSX);
    cgGLBindProgram(vp);
    cgGLBindProgram(fp_red);
    cgGLSetMatrixParameterfr(mvp_param, identity);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_vertices), k_vertices, GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), (const void *)0);
    glEnableClientState(GL_VERTEX_ARRAY);

    glViewport(0, 0, 1280, 720);
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    printf("hello-psgl-blend: ready\n");
    for (int i = 0; i < 900; i++) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        cgGLBindProgram(fp_red);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        cgGLBindProgram(fp_blue);
        glEnable(GL_BLEND);
        glDrawArrays(GL_TRIANGLES, 3, 3);
        psglSwap();
        sys_timer_usleep(16000);
    }

    printf("hello-psgl-blend: done\n");
    cgDestroyContext(cg_context);
    psglExit();
    return 0;
}
