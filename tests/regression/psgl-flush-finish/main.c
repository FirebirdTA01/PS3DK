/* glFlush must submit queued commands to the GPU and glFinish must return
   only after they have completed (OpenGL ES 1.1 section 5.1).  Completion is
   observed through a GPU label written after the draws, which the CPU polls
   directly, with no readback call that would itself wait for the GPU. */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <Cg/cgGL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
#include <sys/timer.h>
#include <cell/gcm.h>
#include "vpshader_vpo.h"
#include "fpshader_fpo.h"
SYS_PROCESS_PARAM(1001, 0x100000);
static unsigned checks, failures;
static CGparameter color_parameter;
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_FLUSH_FAIL %s\n", name); }
}
static int write_blob(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}
static int setup_program(void)
{
    const char *vp_path = "/dev_hdd0/tmp/psgl-flush-finish.vpo";
    const char *fp_path = "/dev_hdd0/tmp/psgl-flush-finish.fpo";
    static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    static const float quad[18] = {-1,-1,0, 1,-1,0, 1,1,0, -1,-1,0, 1,1,0, -1,1,0};
    GLuint vertex_buffer = 0;
    if (!write_blob(vp_path, vpshader_vpo, vpshader_vpo_size) ||
        !write_blob(fp_path, fpshader_fpo, fpshader_fpo_size)) return 0;
    CGcontext cg = cgCreateContext();
    CGprogram vp = cgCreateProgramFromFile(cg, CG_BINARY, vp_path, CG_PROFILE_SCE_VP_RSX, NULL, NULL);
    CGprogram fp = cgCreateProgramFromFile(cg, CG_BINARY, fp_path, CG_PROFILE_SCE_FP_RSX, NULL, NULL);
    CGparameter matrix = cgGetNamedParameter(vp, "modelViewProj");
    color_parameter = cgGetNamedParameter(fp, "solidColor");
    if (!cg || !vp || !fp || !matrix || !color_parameter) return 0;
    cgGLLoadProgram(vp); cgGLLoadProgram(fp);
    cgGLEnableProfile(CG_PROFILE_SCE_VP_RSX); cgGLEnableProfile(CG_PROFILE_SCE_FP_RSX);
    cgGLBindProgram(vp); cgGLBindProgram(fp);
    cgGLSetMatrixParameterfr(matrix, identity);
    glGenBuffers(1, &vertex_buffer); glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, 3 * sizeof(float), (const void *)0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
    glDisable(GL_DITHER); glDisable(GL_SCISSOR_TEST);
    return vertex_buffer != 0;
}
static void draw(float r, float g, float b)
{
    const float color[4] = {r, g, b, 1};
    glViewport(0, 0, 8, 4);
    cgGLSetParameter4fv(color_parameter, color);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
/* The test's own back-end label, after PSGL's draws in the same command
   stream: it is written only once the GPU has executed everything before it. */
#define TEST_LABEL_INDEX 200

int main(void)
{
    PSGLinitOptions options = {0};
    psglInit(&options);
    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglCreateContext();
    if (!device || !context) { puts("PSGL_FLUSH_FAIL setup"); return 1; }
    psglMakeCurrent(context, device);
    if (!setup_program()) { puts("PSGL_FLUSH_FAIL setup program"); return 1; }
    volatile uint32_t *label = (volatile uint32_t *)cellGcmGetLabelAddress(TEST_LABEL_INDEX);

    /* Warm-up through glReadPixels (which waits for the GPU) until a draw
       lands: RPCS3's asynchronous shader compiler may skip a cold draw. */
    unsigned tries;
    for (tries = 1; tries <= 10; ++tries) {
        unsigned char rgba[4] = {0,0,0,0};
        draw(1, 1, 1);
        glReadPixels(2, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        if (rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255) break;
        usleep(200000);
    }
    printf("PSGL_FLUSH_WARMUP attempts=%u\n", tries > 10 ? 10 : tries);
    check(tries <= 10, "warm-up draw lands");

    /* glFlush: the queued draw and label must reach the GPU and complete
       within 2 s of polling, with no other call that would submit them. */
    *label = 0;
    draw(0, 1, 0);
    cellGcmSetWriteBackEndLabel(gCellGcmCurrentContext, TEST_LABEL_INDEX, 1);
    glFlush();
    unsigned polls = 0;
    while (*label != 1u && polls < 2000u) { sys_timer_usleep(1000); ++polls; }
    printf("PSGL_FLUSH_POLL polls=%u label=%u\n", polls, (unsigned)*label);
    check(*label == 1u, "glFlush submits a queued draw");
    {
        unsigned char rgba[4];
        glReadPixels(2, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    /* glFinish: once it returns, everything queued before it has completed. */
    draw(0, 0, 1);
    cellGcmSetWriteBackEndLabel(gCellGcmCurrentContext, TEST_LABEL_INDEX, 2);
    glFinish();
    uint32_t after_finish = *label;
    printf("PSGL_FINISH label=%u\n", (unsigned)after_finish);
    check(after_finish == 2u, "glFinish returns after queued work completes");
    draw(1, 0, 0);
    cellGcmSetWriteBackEndLabel(gCellGcmCurrentContext, TEST_LABEL_INDEX, 3);
    glFinish();
    check(*label == 3u, "a second glFinish waits for its own work");
    check(glGetError() == GL_NO_ERROR, "no GL error");
    printf("PSGL_FLUSH_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    psglExit();
    return failures ? 1 : 0;
}
