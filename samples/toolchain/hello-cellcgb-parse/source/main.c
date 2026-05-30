#include <Cg/cgBinary.h>
#include <cell/cgb.h>
#include <ppu-lv2.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define FIXTURE_PATH "/dev_hdd0/tmp/hello-cellcgb-parse.vpo"

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

static int read_fixture(unsigned char *out, uint32_t *out_size)
{
    FILE *f = fopen(FIXTURE_PATH, "rb");
    size_t got;
    if (!f) return 0;
    got = fread(out, 1, sizeof(k_vertex_binary), f);
    fclose(f);
    if (got != sizeof(k_vertex_binary)) return 0;
    *out_size = (uint32_t)got;
    return 1;
}

int main(void)
{
    unsigned char file_data[sizeof(k_vertex_binary)];
    uint32_t file_size = 0;
    CellCgbProgram program;
    CellCgbVertexProgramConfiguration vconf;
    CellCgbFragmentProgramConfiguration fconf;
    uint32_t output_mask = 0;
    uint32_t clip_mask = 0;
    uint32_t mvp_index;
    uint16_t reg = 0;
    const float *defaults = NULL;
    char name[32];
    uint32_t name_size = sizeof(name);
    uint32_t frag_count = 99;
    uint16_t frag_offsets[4] = {0xffffu, 0xffffu, 0xffffu, 0xffffu};

    if (!write_fixture() || !read_fixture(file_data, &file_size)) {
        printf("hello-cellcgb-parse: file io failed\n");
        return 1;
    }

    if (cellCgbRead(file_data, file_size, &program) != CELL_CGB_OK) {
        printf("hello-cellcgb-parse: read failed size=%u\n", file_size);
        return 1;
    }

    if (cellCgbGetProfile(&program) != CELL_CGB_PROFILE_VERTEX ||
        cellCgbGetSize(file_data) != file_size ||
        cellCgbGetUCodeSize(&program) != 16 ||
        cellCgbGetUCode(&program) == NULL) {
        printf("hello-cellcgb-parse: header query failed\n");
        return 1;
    }

    if (cellCgbGetVertexConfiguration(&program, &vconf) != CELL_CGB_OK ||
        cellCgbGetVertexAttributeOutputMask(&program, &output_mask) != CELL_CGB_OK ||
        cellCgbGetUserClipPlaneControlMask(&program, &clip_mask) != CELL_CGB_OK) {
        printf("hello-cellcgb-parse: vertex query failed\n");
        return 1;
    }

    if (cellCgbGetFragmentConfiguration(&program, &fconf) == CELL_CGB_OK) {
        printf("hello-cellcgb-parse: unexpected fragment config\n");
        return 1;
    }

    mvp_index = cellCgbMapLookup(&program, "mvp");
    cellCgbMapGetName(&program, mvp_index, name, &name_size);
    cellCgbMapGetVertexUniformRegister(&program, mvp_index, &reg, &defaults);
    cellCgbMapGetFragmentUniformOffsets(&program, mvp_index,
                                        frag_offsets, &frag_count);

    if (cellCgbMapGetLength(&program) != 2 ||
        mvp_index != 0 ||
        strcmp(name, "mvp") != 0 ||
        cellCgbMapGetValue(&program, mvp_index) != 17 ||
        cellCgbGetVertexConstantCount(&program) != 1 ||
        reg != 17 ||
        !defaults ||
        defaults[0] != 1.0f ||
        defaults[3] != 1.0f ||
        frag_count != 0) {
        printf("hello-cellcgb-parse: map query failed\n");
        return 1;
    }

    cellCgbGetVertexConstantValues(&program, 0, &reg, &defaults);
    if (reg != 17 || !defaults || defaults[0] != 1.0f) {
        printf("hello-cellcgb-parse: constant query failed\n");
        return 1;
    }

    printf("hello-cellcgb-parse: profile=%u ucode=%u params=%u reg=%u attrOut=0x%08x clip=0x%08x\n",
           (unsigned)cellCgbGetProfile(&program),
           (unsigned)cellCgbGetUCodeSize(&program),
           (unsigned)cellCgbMapGetLength(&program),
           (unsigned)reg,
           (unsigned)output_mask,
           (unsigned)clip_mask);
    return 0;
}
