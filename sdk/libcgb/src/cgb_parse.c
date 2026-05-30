#include <cell/cgb.h>
#include <Cg/cgBinary.h>
#include <stddef.h>
#include <stdint.h>

#define CGB_FACADE_MAGIC 0x43474250u

typedef struct CgbFacade {
    uint32_t magic;
    uint32_t size;
    const CgBinaryProgram *binary;
    const CgBinaryParameter *params;
    const void *program_header;
    const void *ucode;
} CgbFacade;

enum {
    CGB_FACADE_FITS = 1 / (int)(sizeof(CgbFacade) <= CELL_CGB_PROGRAM_STRUCTURE_SIZE)
};

static void cgb_zero(void *ptr, uint32_t size)
{
    unsigned char *out = (unsigned char *)ptr;
    while (size--) *out++ = 0;
}

static void cgb_copy(void *dst, const void *src, uint32_t size)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    while (size--) *out++ = *in++;
}

static void cgb_store(CellCgbProgram *program, const CgbFacade *facade)
{
    cgb_zero(program, sizeof(*program));
    cgb_copy(program->data, facade, (uint32_t)sizeof(*facade));
}

static int cgb_load(const CellCgbProgram *program, CgbFacade *facade)
{
    if (!program || !facade) return 0;
    cgb_copy(facade, program->data, (uint32_t)sizeof(*facade));
    return facade->magic == CGB_FACADE_MAGIC && facade->binary != NULL;
}

static int cgb_range(uint32_t total, uint32_t offset, uint32_t size)
{
    if (offset == 0 || offset > total) return 0;
    if (size > total - offset) return 0;
    return 1;
}

static const void *cgb_ptr(const CgBinaryProgram *binary, uint32_t total,
                           uint32_t offset, uint32_t size)
{
    if (!cgb_range(total, offset, size)) return NULL;
    return (const unsigned char *)binary + offset;
}

static uint32_t cgb_cstr_len(const char *s)
{
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n] != '\0') n++;
    return n;
}

static int cgb_string_valid(const CgBinaryProgram *binary, uint32_t total,
                            uint32_t offset)
{
    const char *s;
    if (offset == 0 || offset >= total) return 0;
    s = (const char *)binary + offset;
    for (uint32_t i = offset; i < total; i++) {
        if (*s++ == '\0') return 1;
    }
    return 0;
}

static int cgb_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int cgb_is_vertex_profile(CGprofile profile)
{
    return profile == CG_PROFILE_SCE_VP_RSX ||
           profile == CG_PROFILE_VP40 ||
           profile == CG_PROFILE_ARBVP1 ||
           profile == CG_PROFILE_VP30 ||
           profile == CG_PROFILE_VP20;
}

static int cgb_is_fragment_profile(CGprofile profile)
{
    return profile == CG_PROFILE_SCE_FP_RSX ||
           profile == CG_PROFILE_FP40 ||
           profile == CG_PROFILE_ARBFP1 ||
           profile == CG_PROFILE_FP30 ||
           profile == CG_PROFILE_FP20;
}

static const CgBinaryParameter *cgb_param(const CgbFacade *facade,
                                          uint32_t index)
{
    if (!facade || !facade->params || index >= facade->binary->parameterCount)
        return NULL;
    return &facade->params[index];
}

static const char *cgb_param_name(const CgbFacade *facade,
                                  const CgBinaryParameter *param)
{
    if (!facade || !param || param->name == 0) return NULL;
    return (const char *)facade->binary + param->name;
}

static int cgb_param_is_vertex_uniform(const CgBinaryParameter *param)
{
    return param && param->res == CG_C;
}

uint32_t cellCgbGetSize(const void *binary)
{
    const CgBinaryProgram *program = (const CgBinaryProgram *)binary;
    if (!program || program->revision != CG_BINARY_FORMAT_REVISION) return 0;
    return program->totalSize;
}

int32_t cellCgbRead(const void *binary, const uint32_t size,
                    CellCgbProgram *program)
{
    const CgBinaryProgram *cg = (const CgBinaryProgram *)binary;
    const CgBinaryParameter *params;
    const void *program_header;
    const void *ucode;
    CgbFacade facade;
    uint32_t total;
    uint32_t header_size;

    if (program) cgb_zero(program, sizeof(*program));
    if (!binary || !program || size < (uint32_t)offsetof(CgBinaryProgram, data))
        return CELL_CGB_ERROR_FAILED;
    if (cg->revision != CG_BINARY_FORMAT_REVISION)
        return CELL_CGB_ERROR_FAILED;
    total = cg->totalSize;
    if (total < (uint32_t)offsetof(CgBinaryProgram, data) || total > size)
        return CELL_CGB_ERROR_FAILED;
    if (!cgb_is_vertex_profile(cg->profile) && !cgb_is_fragment_profile(cg->profile))
        return CELL_CGB_ERROR_FAILED;

    if (cg->parameterCount != 0) {
        const uint32_t bytes = cg->parameterCount * (uint32_t)sizeof(CgBinaryParameter);
        if (cg->parameterCount != bytes / (uint32_t)sizeof(CgBinaryParameter))
            return CELL_CGB_ERROR_FAILED;
        params = (const CgBinaryParameter *)cgb_ptr(cg, total, cg->parameterArray, bytes);
        if (!params) return CELL_CGB_ERROR_FAILED;
    } else {
        params = NULL;
    }

    header_size = cgb_is_vertex_profile(cg->profile) ?
        (uint32_t)sizeof(CgBinaryVertexProgram) :
        (uint32_t)sizeof(CgBinaryFragmentProgram);
    program_header = cgb_ptr(cg, total, cg->program, header_size);
    ucode = cgb_ptr(cg, total, cg->ucode, cg->ucodeSize);
    if (!program_header || !ucode) return CELL_CGB_ERROR_FAILED;

    for (uint32_t i = 0; i < cg->parameterCount; i++) {
        const CgBinaryParameter *param = &params[i];
        if (param->name != 0 && !cgb_string_valid(cg, total, param->name))
            return CELL_CGB_ERROR_FAILED;
        if (param->semantic != 0 && !cgb_string_valid(cg, total, param->semantic))
            return CELL_CGB_ERROR_FAILED;
        if (param->defaultValue != 0 &&
            !cgb_range(total, param->defaultValue, 4u * (uint32_t)sizeof(float)))
            return CELL_CGB_ERROR_FAILED;
        if (param->embeddedConst != 0) {
            const CgBinaryEmbeddedConstant *embedded =
                (const CgBinaryEmbeddedConstant *)cgb_ptr(
                    cg, total, param->embeddedConst, sizeof(uint32_t));
            uint32_t bytes;
            if (!embedded) return CELL_CGB_ERROR_FAILED;
            bytes = sizeof(uint32_t) + embedded->ucodeCount * (uint32_t)sizeof(uint32_t);
            if (embedded->ucodeCount !=
                (bytes - (uint32_t)sizeof(uint32_t)) / (uint32_t)sizeof(uint32_t))
                return CELL_CGB_ERROR_FAILED;
            if (!cgb_range(total, param->embeddedConst, bytes))
                return CELL_CGB_ERROR_FAILED;
        }
    }

    facade.magic = CGB_FACADE_MAGIC;
    facade.size = total;
    facade.binary = cg;
    facade.params = params;
    facade.program_header = program_header;
    facade.ucode = ucode;
    cgb_store(program, &facade);
    return CELL_CGB_OK;
}

CellCgbProfile cellCgbGetProfile(const CellCgbProgram *program)
{
    CgbFacade facade;
    if (!cgb_load(program, &facade)) return CELL_CGB_PROFILE_UNKNOWN;
    if (cgb_is_vertex_profile(facade.binary->profile)) return CELL_CGB_PROFILE_VERTEX;
    if (cgb_is_fragment_profile(facade.binary->profile)) return CELL_CGB_PROFILE_FRAGMENT;
    return CELL_CGB_PROFILE_UNKNOWN;
}

const void *cellCgbGetUCode(const CellCgbProgram *program)
{
    CgbFacade facade;
    return cgb_load(program, &facade) ? facade.ucode : NULL;
}

uint32_t cellCgbGetUCodeSize(const CellCgbProgram *program)
{
    CgbFacade facade;
    return cgb_load(program, &facade) ? facade.binary->ucodeSize : 0u;
}

int32_t cellCgbGetVertexConfiguration(
    const CellCgbProgram *program,
    CellCgbVertexProgramConfiguration *conf)
{
    CgbFacade facade;
    const CgBinaryVertexProgram *vp;
    if (conf) cgb_zero(conf, sizeof(*conf));
    if (!conf || !cgb_load(program, &facade) ||
        !cgb_is_vertex_profile(facade.binary->profile))
        return CELL_CGB_ERROR_FAILED;
    vp = (const CgBinaryVertexProgram *)facade.program_header;
    conf->instructionSlot = (uint16_t)vp->instructionSlot;
    conf->instructionCount = (uint16_t)vp->instructionCount;
    conf->attributeInputMask = (uint16_t)vp->attributeInputMask;
    conf->registerCount = (uint8_t)vp->registerCount;
    return CELL_CGB_OK;
}

int32_t cellCgbGetFragmentConfiguration(
    const CellCgbProgram *program,
    CellCgbFragmentProgramConfiguration *conf)
{
    CgbFacade facade;
    const CgBinaryFragmentProgram *fp;
    if (conf) cgb_zero(conf, sizeof(*conf));
    if (!conf || !cgb_load(program, &facade) ||
        !cgb_is_fragment_profile(facade.binary->profile))
        return CELL_CGB_ERROR_FAILED;
    fp = (const CgBinaryFragmentProgram *)facade.program_header;
    conf->attributeInputMask = fp->attributeInputMask;
    conf->texCoordsInputMask = fp->texCoordsInputMask;
    conf->texCoords2D = fp->texCoords2D;
    conf->texCoordsCentroid = fp->texCoordsCentroid;
    conf->fragmentControl = ((uint32_t)fp->partialTexType & 0xffffu) |
                            ((uint32_t)fp->outputFromH0 << 16) |
                            ((uint32_t)fp->depthReplace << 17) |
                            ((uint32_t)fp->pixelKill << 18);
    conf->registerCount = fp->registerCount;
    return CELL_CGB_OK;
}

int32_t cellCgbGetVertexAttributeOutputMask(const CellCgbProgram *program,
                                            uint32_t *attributeOutputMask)
{
    CgbFacade facade;
    const CgBinaryVertexProgram *vp;
    if (attributeOutputMask) *attributeOutputMask = 0;
    if (!attributeOutputMask || !cgb_load(program, &facade) ||
        !cgb_is_vertex_profile(facade.binary->profile))
        return CELL_CGB_ERROR_FAILED;
    vp = (const CgBinaryVertexProgram *)facade.program_header;
    *attributeOutputMask = vp->attributeOutputMask;
    return CELL_CGB_OK;
}

int32_t cellCgbGetUserClipPlaneControlMask(const CellCgbProgram *program,
                                           uint32_t *userClipPlaneControlMask)
{
    CgbFacade facade;
    const CgBinaryVertexProgram *vp;
    if (userClipPlaneControlMask) *userClipPlaneControlMask = 0;
    if (!userClipPlaneControlMask || !cgb_load(program, &facade) ||
        !cgb_is_vertex_profile(facade.binary->profile))
        return CELL_CGB_ERROR_FAILED;
    vp = (const CgBinaryVertexProgram *)facade.program_header;
    *userClipPlaneControlMask = vp->userClipMask;
    return CELL_CGB_OK;
}

uint32_t cellCgbGetVertexConstantCount(const CellCgbProgram *program)
{
    CgbFacade facade;
    uint32_t count = 0;
    if (!cgb_load(program, &facade) || !cgb_is_vertex_profile(facade.binary->profile))
        return 0;
    for (uint32_t i = 0; i < facade.binary->parameterCount; i++) {
        if (cgb_param_is_vertex_uniform(&facade.params[i])) count++;
    }
    return count;
}

void cellCgbGetVertexConstantValues(const CellCgbProgram *program,
                                    uint32_t value_index,
                                    uint16_t *reg,
                                    const float **value)
{
    CgbFacade facade;
    uint32_t current = 0;
    if (reg) *reg = 0;
    if (value) *value = NULL;
    if (!cgb_load(program, &facade) || !cgb_is_vertex_profile(facade.binary->profile))
        return;
    for (uint32_t i = 0; i < facade.binary->parameterCount; i++) {
        const CgBinaryParameter *param = &facade.params[i];
        if (!cgb_param_is_vertex_uniform(param)) continue;
        if (current++ == value_index) {
            if (reg) *reg = (uint16_t)param->resIndex;
            if (value && param->defaultValue != 0)
                *value = (const float *)((const unsigned char *)facade.binary +
                                         param->defaultValue);
            return;
        }
    }
}

uint32_t cellCgbMapLookup(CellCgbProgram *program, const char *name)
{
    CgbFacade facade;
    if (!name || !cgb_load(program, &facade)) return CELL_CGB_ERROR_FAILED;
    for (uint32_t i = 0; i < facade.binary->parameterCount; i++) {
        if (cgb_streq(cgb_param_name(&facade, &facade.params[i]), name))
            return i;
    }
    return CELL_CGB_ERROR_FAILED;
}

uint16_t cellCgbMapGetValue(CellCgbProgram *program,
                            const uint32_t map_index)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    if (!cgb_load(program, &facade)) return 0;
    param = cgb_param(&facade, map_index);
    return param ? (uint16_t)param->resIndex : 0;
}

uint32_t cellCgbMapGetLength(const CellCgbProgram *program)
{
    CgbFacade facade;
    return cgb_load(program, &facade) ? facade.binary->parameterCount : 0u;
}

void cellCgbMapGetName(CellCgbProgram *program,
                       const uint32_t map_index,
                       char *name, uint32_t *size)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    const char *src;
    uint32_t required;
    uint32_t capacity;
    if (name) name[0] = '\0';
    if (size && *size == 0) *size = 0;
    if (!cgb_load(program, &facade)) {
        if (size) *size = 0;
        return;
    }
    param = cgb_param(&facade, map_index);
    src = cgb_param_name(&facade, param);
    if (!src) {
        if (size) *size = 0;
        return;
    }
    required = cgb_cstr_len(src) + 1u;
    capacity = size ? *size : 0u;
    if (name && capacity != 0) {
        uint32_t n = required < capacity ? required : capacity;
        for (uint32_t i = 0; i < n; i++) name[i] = src[i];
        name[n - 1u] = '\0';
    }
    if (size) *size = required;
}

void cellCgbMapGetVertexUniformRegister(const CellCgbProgram *program,
                                        const uint32_t map_index,
                                        uint16_t *reg,
                                        const float **default_values)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    if (reg) *reg = 0;
    if (default_values) *default_values = NULL;
    if (!cgb_load(program, &facade)) return;
    param = cgb_param(&facade, map_index);
    if (!cgb_param_is_vertex_uniform(param)) return;
    if (reg) *reg = (uint16_t)param->resIndex;
    if (default_values && param->defaultValue != 0)
        *default_values = (const float *)((const unsigned char *)facade.binary +
                                          param->defaultValue);
}

void cellCgbMapGetFragmentUniformOffsets(const CellCgbProgram *program,
                                         const uint32_t map_index,
                                         uint16_t *offsets,
                                         uint32_t *count)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    const CgBinaryEmbeddedConstant *embedded;
    if (count) *count = 0;
    if (offsets) *offsets = 0;
    if (!cgb_load(program, &facade)) return;
    param = cgb_param(&facade, map_index);
    if (!param || param->embeddedConst == 0) return;
    embedded = (const CgBinaryEmbeddedConstant *)
        ((const unsigned char *)facade.binary + param->embeddedConst);
    if (count) *count = embedded->ucodeCount;
    if (offsets) {
        for (uint32_t i = 0; i < embedded->ucodeCount; i++)
            offsets[i] = (uint16_t)embedded->ucodeOffset[i];
    }
}

void cellCgbMapGetFragmentUniformRegister(const CellCgbProgram *program,
                                          const uint32_t map_index,
                                          uint16_t *reg)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    if (reg) *reg = 0;
    if (!cgb_load(program, &facade)) return;
    param = cgb_param(&facade, map_index);
    if (param && reg) *reg = (uint16_t)param->resIndex;
}
