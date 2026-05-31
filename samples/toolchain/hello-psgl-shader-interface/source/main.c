#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <cell/cgb.h>
#include <cell/gcm.h>
#include <rsx/mm.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "fpshader_fpo.h"
#include "vpshader_vpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

typedef struct Vertex {
    float position[3];
} Vertex;

static const Vertex k_vertices[3] = {
    {{-0.65f, -0.55f, 0.0f}},
    {{ 0.65f, -0.55f, 0.0f}},
    {{ 0.00f,  0.65f, 0.0f}}
};

static const float k_identity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};

static const float k_green[4] = {0.0f, 1.0f, 0.0f, 1.0f};

static void copy_bytes(void *dst, const void *src, uint32_t size)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    while (size--) *out++ = *in++;
}

static int load_shader_metadata(CellCgbProgram *vp_program,
                                CellCgbProgram *fp_program,
                                CellCgbVertexProgramConfiguration *vp_conf,
                                CellCgbFragmentProgramConfiguration *fp_conf,
                                uint32_t *vp_output_mask,
                                uint16_t *mvp_reg,
                                uint16_t *color_offsets,
                                uint32_t *color_offset_count)
{
    uint32_t map_index;
    const float *unused_defaults = NULL;
    uint16_t unused_reg = 0u;

    if (cellCgbRead(vpshader_vpo, vpshader_vpo_size, vp_program) != 0)
        return 0;
    if (cellCgbRead(fpshader_fpo, fpshader_fpo_size, fp_program) != 0)
        return 0;
    if (cellCgbGetVertexConfiguration(vp_program, vp_conf) != 0)
        return 0;
    if (cellCgbGetVertexAttributeOutputMask(vp_program, vp_output_mask) != 0)
        return 0;
    if (cellCgbGetFragmentConfiguration(fp_program, fp_conf) != 0)
        return 0;

    map_index = cellCgbMapLookup(vp_program, "modelViewProj");
    if (map_index >= cellCgbMapGetLength(vp_program)) return 0;
    cellCgbMapGetVertexUniformRegister(vp_program, map_index, mvp_reg,
                                        &unused_defaults);

    map_index = cellCgbMapLookup(fp_program, "solidColor");
    if (map_index >= cellCgbMapGetLength(fp_program)) return 0;
    *color_offset_count = 4u;
    cellCgbMapGetFragmentUniformOffsets(fp_program, map_index, color_offsets,
                                        color_offset_count);
    cellCgbMapGetFragmentUniformRegister(fp_program, map_index, &unused_reg);
    return *color_offset_count != 0u;
}

int main(void)
{
    PSGLinitOptions options = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    CellCgbProgram vp_program;
    CellCgbProgram fp_program;
    CellCgbVertexProgramConfiguration vp_conf;
    CellCgbFragmentProgramConfiguration fp_conf;
    uint32_t vp_output_mask = 0u;
    uint16_t mvp_reg = 0u;
    uint16_t color_offsets[4] = {0u, 0u, 0u, 0u};
    uint32_t color_offset_count = 0u;
    const void *fp_ucode;
    uint32_t fp_ucode_size;
    void *fp_ucode_local;
    GLuint fp_offset = 0u;
    GLuint vbo = 0u;

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    if (!device || !gl_context) {
        printf("hello-psgl-shader-interface: PSGL init failed\n");
        psglExit();
        return 1;
    }
    psglMakeCurrent(gl_context, device);

    if (!load_shader_metadata(&vp_program, &fp_program, &vp_conf, &fp_conf,
                              &vp_output_mask, &mvp_reg, color_offsets,
                              &color_offset_count)) {
        printf("hello-psgl-shader-interface: CGB metadata failed\n");
        psglExit();
        return 1;
    }

    fp_ucode = cellCgbGetUCode(&fp_program);
    fp_ucode_size = cellCgbGetUCodeSize(&fp_program);
    fp_ucode_local = rsxMemalign(64u, fp_ucode_size);
    if (!fp_ucode || !fp_ucode_local) {
        printf("hello-psgl-shader-interface: FP ucode alloc failed\n");
        psglExit();
        return 1;
    }
    copy_bytes(fp_ucode_local, fp_ucode, fp_ucode_size);
    psglAddressToOffset(fp_ucode_local, &fp_offset);
    if (fp_offset == 0u) {
        printf("hello-psgl-shader-interface: FP offset failed\n");
        psglExit();
        return 1;
    }
    fp_conf.offset = fp_offset;

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_vertices), k_vertices,
                 GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), (const void *)0);
    glEnableClientState(GL_VERTEX_ARRAY);

    glViewport(0, 0, 1280, 720);
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);
    psglSetVertexProgramConfiguration(&vp_conf,
                                      cellCgbGetUCode(&vp_program));
    cellGcmSetVertexAttribOutputMask(gCellGcmCurrentContext,
        vp_output_mask | CELL_GCM_ATTRIB_OUTPUT_MASK_POINTSIZE);
    psglSetVertexProgramRegisterBlock(mvp_reg, 4u, k_identity);
    for (uint32_t i = 0u; i < color_offset_count; i++) {
        psglSetFragmentProgramEmbeddedConstantMemoryLocation(
            fp_offset + color_offsets[i], k_green, 4u, true);
    }
    psglSetFragmentProgramConfigurationMemoryLocation(&fp_conf, true);

    printf("hello-psgl-shader-interface: ready\n");
    for (int frame = 0; frame < 900; frame++) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        psglSwap();
        sys_timer_usleep(16000);
    }

    printf("hello-psgl-shader-interface: done\n");
    psglExit();
    return 0;
}
