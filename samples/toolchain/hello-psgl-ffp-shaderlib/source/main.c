#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <cell/pngdec.h>
#include <cell/sysmodule.h>
#include <malloc.h>
#include <ppu-lv2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "shaders_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define SHADER_LIBRARY_PATH "/dev_hdd0/tmp/hello-psgl-ffp-shaderlib.bin"

extern const uint8_t ICON0_PNG[] __attribute__((aligned(4)));
extern const uint32_t ICON0_PNG_size;

typedef struct Vertex {
    float position[3];
    float texcoord[2];
} Vertex;

static const Vertex k_vertices[6] = {
    {{-0.80f, -0.80f, 0.0f}, {0.0f, 1.0f}},
    {{ 0.80f, -0.80f, 0.0f}, {1.0f, 1.0f}},
    {{-0.80f,  0.80f, 0.0f}, {0.0f, 0.0f}},
    {{-0.80f,  0.80f, 0.0f}, {0.0f, 0.0f}},
    {{ 0.80f, -0.80f, 0.0f}, {1.0f, 1.0f}},
    {{ 0.80f,  0.80f, 0.0f}, {1.0f, 0.0f}}
};

static void *png_malloc(uint32_t size, void *arg)
{
    (void)arg;
    return memalign(16, size);
}

static int32_t png_free(void *ptr, void *arg)
{
    (void)arg;
    free(ptr);
    return 0;
}

static uint8_t *decode_icon0(uint32_t *out_w, uint32_t *out_h,
                             uint32_t *out_stride)
{
    CellPngDecThreadInParam tin = {
        .spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE,
        .ppuThreadPriority = 512,
        .spuThreadPriority = 200,
        .cbCtrlMallocFunc = CELL_PNGDEC_CB_EA(png_malloc),
        .cbCtrlMallocArg = 0,
        .cbCtrlFreeFunc = CELL_PNGDEC_CB_EA(png_free),
        .cbCtrlFreeArg = 0,
    };
    CellPngDecThreadOutParam tout = {0};
    CellPngDecMainHandle main_h = 0;
    CellPngDecSubHandle sub_h = 0;
    CellPngDecSrc src = {
        .srcSelect = CELL_PNGDEC_BUFFER,
        .streamPtr = (uint32_t)(uintptr_t)ICON0_PNG,
        .streamSize = ICON0_PNG_size,
        .spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE,
    };
    CellPngDecOpnInfo open_info = {0};
    CellPngDecInfo header = {0};
    CellPngDecInParam in_param = {
        .outputMode = CELL_PNGDEC_TOP_TO_BOTTOM,
        .outputColorSpace = CELL_PNGDEC_RGBA,
        .outputBitDepth = 8,
        .outputPackFlag = CELL_PNGDEC_1BYTE_PER_1PIXEL,
        .outputAlphaSelect = CELL_PNGDEC_STREAM_ALPHA,
    };
    CellPngDecOutParam out_param = {0};
    CellPngDecDataOutInfo data_info = {0};
    uint32_t stride;
    uint32_t size;
    uint8_t *rgba = NULL;
    int loaded = 0;

    if (cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC) != 0) return NULL;
    loaded = 1;
    if (cellPngDecCreate(&main_h, &tin, &tout) != 0) goto fail;
    if (cellPngDecOpen(main_h, &sub_h, &src, &open_info) != 0) goto fail;
    if (cellPngDecReadHeader(main_h, sub_h, &header) != 0) goto fail;
    if (cellPngDecSetParameter(main_h, sub_h, &in_param, &out_param) != 0)
        goto fail;

    stride = (uint32_t)out_param.outputWidthByte;
    size = stride * (uint32_t)out_param.outputHeight;
    rgba = (uint8_t *)memalign(16, size);
    if (!rgba) goto fail;

    CellPngDecDataCtrlParam data_ctrl = { .outputBytesPerLine = stride };
    if (cellPngDecDecodeData(main_h, sub_h, rgba, &data_ctrl, &data_info) != 0 ||
        data_info.status != CELL_PNGDEC_DEC_STATUS_FINISH)
        goto fail;

    cellPngDecClose(main_h, sub_h);
    cellPngDecDestroy(main_h);
    cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC);
    *out_w = (uint32_t)out_param.outputWidth;
    *out_h = (uint32_t)out_param.outputHeight;
    *out_stride = stride;
    return rgba;

fail:
    if (rgba) free(rgba);
    if (sub_h) cellPngDecClose(main_h, sub_h);
    if (main_h) cellPngDecDestroy(main_h);
    if (loaded) cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC);
    return NULL;
}

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

int main(void)
{
    PSGLinitOptions options = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    GLuint vbo = 0u;
    GLuint texture = 0u;
    uint32_t texture_width = 0u;
    uint32_t texture_height = 0u;
    uint32_t texture_stride = 0u;
    uint8_t *texture_rgba;

    if (!write_blob(SHADER_LIBRARY_PATH, shaders_bin, shaders_bin_size)) {
        printf("hello-psgl-ffp-shaderlib: library write failed\n");
        return 1;
    }

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    if (!device || !gl_context) {
        printf("hello-psgl-ffp-shaderlib: PSGL init failed\n");
        psglExit();
        return 1;
    }
    psglMakeCurrent(gl_context, device);
    psglLoadShaderLibrary(SHADER_LIBRARY_PATH);

    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texture_rgba = decode_icon0(&texture_width, &texture_height, &texture_stride);
    if (!texture_rgba) {
        printf("hello-psgl-ffp-shaderlib: texture decode failed\n");
        psglExit();
        return 1;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)texture_width,
                 (GLsizei)texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 texture_rgba);
    free(texture_rgba);
    glEnable(GL_TEXTURE_2D);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_vertices), k_vertices, GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), (const void *)0);
    glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex),
                      (const void *)(uintptr_t)sizeof(((Vertex *)0)->position));
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glViewport(0, 0, 1280, 720);
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    printf("hello-psgl-ffp-shaderlib: ready\n");
    for (int i = 0; i < 900; i++) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        psglSwap();
        sys_timer_usleep(16000);
    }

    printf("hello-psgl-ffp-shaderlib: done\n");
    psglExit();
    return 0;
}
