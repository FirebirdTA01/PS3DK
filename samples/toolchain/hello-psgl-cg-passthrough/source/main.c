#include <Cg/cgBinary.h>
#include <Cg/cgGL.h>
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <ppu-lv2.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define FIXTURE_PATH "/dev_hdd0/tmp/hello-psgl-cg-passthrough.vpo"

typedef struct TestVertexBinary {
    CgBinaryProgram program;
    CgBinaryParameter params[2];
    CgBinaryVertexProgram vp;
    uint32_t ucode[4];
    float mvp_default[4];
    char name_mvp[4];
    char name_position[9];
    char semantic_position[10];
} TestVertexBinary;

static const TestVertexBinary k_vertex_binary = {
    .program = {
        .profile = CG_PROFILE_SCE_VP_RSX,
        .revision = CG_BINARY_FORMAT_REVISION,
        .totalSize = sizeof(TestVertexBinary),
        .parameterCount = 2,
        .parameterArray = offsetof(TestVertexBinary, params),
        .program = offsetof(TestVertexBinary, vp),
        .ucodeSize = 16,
        .ucode = offsetof(TestVertexBinary, ucode),
        .data = {0}
    },
    .params = {
        {
            .type = CG_FLOAT4,
            .res = CG_C,
            .var = CG_UNIFORM,
            .resIndex = 17,
            .name = offsetof(TestVertexBinary, name_mvp),
            .defaultValue = offsetof(TestVertexBinary, mvp_default),
            .embeddedConst = 0,
            .semantic = 0,
            .direction = CG_IN,
            .paramno = 0,
            .isReferenced = CG_TRUE,
            .isShared = CG_FALSE
        },
        {
            .type = CG_FLOAT4,
            .res = CG_ATTR0,
            .var = CG_VARYING,
            .resIndex = 0,
            .name = offsetof(TestVertexBinary, name_position),
            .defaultValue = 0,
            .embeddedConst = 0,
            .semantic = offsetof(TestVertexBinary, semantic_position),
            .direction = CG_IN,
            .paramno = 1,
            .isReferenced = CG_TRUE,
            .isShared = CG_FALSE
        }
    },
    .vp = {
        .instructionCount = 1,
        .instructionSlot = 0,
        .registerCount = 2,
        .attributeInputMask = 0x0001,
        .attributeOutputMask = 0x0100,
        .userClipMask = 0
    },
    .ucode = {0x00000000u, 0x11111111u, 0x22222222u, 0x33333333u},
    .mvp_default = {1.0f, 0.0f, 0.0f, 1.0f},
    .name_mvp = "mvp",
    .name_position = "position",
    .semantic_position = "POSITION0"
};

static int write_fixture(void)
{
    FILE *f = fopen(FIXTURE_PATH, "wb");
    if (!f) return 0;
    if (fwrite(&k_vertex_binary, 1, sizeof(k_vertex_binary), f) !=
        sizeof(k_vertex_binary)) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

int main(void)
{
    PSGLinitOptions options = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    CGcontext cg_context;
    CGprogram program;
    CGparameter mvp;
    float value[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    float readback[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    if (!write_fixture()) {
        printf("hello-psgl-cg-passthrough: fixture failed\n");
        return 1;
    }

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    if (!device || !gl_context) {
        printf("hello-psgl-cg-passthrough: PSGL init failed\n");
        psglExit();
        return 1;
    }
    psglMakeCurrent(gl_context, device);

    cg_context = cgCreateContext();
    program = cgCreateProgramFromFile(cg_context, CG_BINARY, FIXTURE_PATH,
                                      CG_PROFILE_SCE_VP_RSX, NULL, NULL);
    mvp = cgGetNamedParameter(program, "mvp");
    if (!cg_context || !program || !mvp) {
        printf("hello-psgl-cg-passthrough: Cg setup failed err=%d\n",
               (int)cgGetError());
        cgDestroyContext(cg_context);
        psglExit();
        return 1;
    }

    cgGLLoadProgram(program);
    cgGLEnableProfile(CG_PROFILE_SCE_VP_RSX);
    cgGLSetParameter4fv(mvp, value);
    cgGLGetParameter4f(mvp, readback);
    cgGLBindProgram(program);
    if (!cgGLIsProgramLoaded(program) ||
        readback[0] != value[0] ||
        readback[3] != value[3]) {
        printf("hello-psgl-cg-passthrough: bind path failed\n");
        cgDestroyContext(cg_context);
        psglExit();
        return 1;
    }

    glClearColor(0.02f, 0.10f, 0.22f, 1.0f);
    for (int i = 0; i < 4; i++) {
        glClear(GL_COLOR_BUFFER_BIT);
        psglSwap();
        sys_timer_usleep(16000);
    }

    printf("hello-psgl-cg-passthrough: ok param=%s profile=%d\n",
           cgGetParameterName(mvp), (int)cgGetProgramProfile(program));
    cgDestroyContext(cg_context);
    psglExit();
    return 0;
}
