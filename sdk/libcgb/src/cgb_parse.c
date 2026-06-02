#include <cell/cgb.h>
#include <cell/cgb/cgb_levelc.h>
#include <Cg/cgBinary.h>
#include <stddef.h>
#include <stdint.h>

#define CGB_FACADE_MAGIC 0x43474250u
#define CGB_FACADE_COMPACT 0x80000000u
#define CGB_FACADE_SIZE_MASK 0x7fffffffu

#define CGB_COMPACT_FOURCC 0x43474200u
#define CGB_COMPACT_HEADER_SIZE 12u
#define CGB_COMPACT_UCODE_OFFSET 32u
#define CGB_COMPACT_PROFILE_VERTEX 0u
#define CGB_COMPACT_PROFILE_FRAGMENT 1u
#define CGB_COMPACT_CONSTANT_TABLE 1u
#define CGB_COMPACT_LOOKUP_TABLE 2u
#define CGB_COMPACT_PARAMETER_INFO 4u
#define CGB_COMPACT_FP_RESOURCE_START 1024u
#define CGB_COMPACT_CGPV_MASK 0x03u
#define CGB_COMPACT_CGPV_VARYING 0x00u
#define CGB_COMPACT_CGPV_UNIFORM 0x01u
#define CGB_COMPACT_CGPV_CONSTANT 0x02u
#define CGB_COMPACT_CGPV_MIXED 0x03u
#define CGB_COMPACT_CGPD_MASK 0x0cu
#define CGB_COMPACT_CGPD_IN 0x00u
#define CGB_COMPACT_CGPD_OUT 0x04u
#define CGB_COMPACT_CGPD_INOUT 0x08u

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

static uint16_t cgb_be16(const void *ptr)
{
    const unsigned char *p = (const unsigned char *)ptr;
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t cgb_be32(const void *ptr)
{
    const unsigned char *p = (const unsigned char *)ptr;
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static int cgb_is_compact_facade(const CgbFacade *facade)
{
    return facade && (facade->size & CGB_FACADE_COMPACT) != 0u;
}

static uint32_t cgb_facade_size(const CgbFacade *facade)
{
    return facade ? (facade->size & CGB_FACADE_SIZE_MASK) : 0u;
}

static const unsigned char *cgb_compact_base(const CgbFacade *facade)
{
    return (const unsigned char *)facade->binary;
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

static uint8_t cgb_compact_profile(const CgbFacade *facade)
{
    return cgb_compact_base(facade)[0x0a];
}

static uint8_t cgb_compact_content(const CgbFacade *facade)
{
    return cgb_compact_base(facade)[0x0b];
}

static uint16_t cgb_compact_ucode_size_base(const unsigned char *base)
{
    return cgb_be16(base + 0x08);
}

static uint16_t cgb_compact_ucode_size(const CgbFacade *facade)
{
    return cgb_compact_ucode_size_base(cgb_compact_base(facade));
}

static const unsigned char *cgb_compact_levelb(const CgbFacade *facade)
{
    return (const unsigned char *)facade->params;
}

static uint16_t cgb_compact_levelb_size(const CgbFacade *facade)
{
    const unsigned char *levelb = cgb_compact_levelb(facade);
    return levelb ? cgb_be16(levelb) : 0u;
}

static uint16_t cgb_compact_levelb_count(const CgbFacade *facade)
{
    const unsigned char *levelb = cgb_compact_levelb(facade);
    return levelb ? cgb_be16(levelb + 2) : 0u;
}

static uint16_t cgb_compact_levelb_fp_count(const CgbFacade *facade)
{
    const unsigned char *levelb = cgb_compact_levelb(facade);
    return levelb ? cgb_be16(levelb + 4) : 0u;
}

static const unsigned char *cgb_compact_entry(const CgbFacade *facade,
                                              uint32_t index)
{
    const unsigned char *levelb = cgb_compact_levelb(facade);
    if (!levelb || index >= cgb_compact_levelb_count(facade)) return NULL;
    return levelb + 6u + index * 8u;
}

static const unsigned char *cgb_compact_fp_resources(const CgbFacade *facade)
{
    const unsigned char *levelb = cgb_compact_levelb(facade);
    return levelb ? levelb + 6u + cgb_compact_levelb_count(facade) * 8u : NULL;
}

static const char *cgb_compact_strings(const CgbFacade *facade)
{
    const unsigned char *fp = cgb_compact_fp_resources(facade);
    return fp ? (const char *)(fp + cgb_compact_levelb_fp_count(facade) * 2u) : NULL;
}

static uint32_t cgb_compact_strings_size(const CgbFacade *facade)
{
    const unsigned char *levelb = cgb_compact_levelb(facade);
    uint32_t used;
    uint32_t size;
    if (!levelb) return 0u;
    size = cgb_compact_levelb_size(facade);
    used = 6u + cgb_compact_levelb_count(facade) * 8u +
           cgb_compact_levelb_fp_count(facade) * 2u;
    return used <= size ? size - used : 0u;
}

static const char *cgb_compact_entry_name(const CgbFacade *facade,
                                          uint32_t index)
{
    const unsigned char *entry = cgb_compact_entry(facade, index);
    const char *strings = cgb_compact_strings(facade);
    uint32_t strings_size = cgb_compact_strings_size(facade);
    uint32_t offset;
    if (!entry || !strings) return NULL;
    offset = cgb_be32(entry);
    if (offset >= strings_size) return NULL;
    return strings + offset;
}

static uint16_t cgb_compact_entry_resource(const CgbFacade *facade,
                                           uint32_t index)
{
    const unsigned char *entry = cgb_compact_entry(facade, index);
    return entry ? cgb_be16(entry + 6) : 0u;
}

static int cgb_compact_cstr_valid(const char *strings,
                                  uint32_t strings_size,
                                  uint32_t offset)
{
    if (!strings || offset >= strings_size) return 0;
    for (uint32_t i = offset; i < strings_size; i++) {
        if (strings[i] == '\0') return 1;
    }
    return 0;
}

static int cgb_compact_validate_levelb(const unsigned char *levelb,
                                       uint32_t available)
{
    uint32_t block_size;
    uint32_t entry_count;
    uint32_t fp_count;
    uint32_t used;
    const char *strings;
    uint32_t strings_size;

    if (available < 6u) return 0;
    block_size = cgb_be16(levelb);
    entry_count = cgb_be16(levelb + 2);
    fp_count = cgb_be16(levelb + 4);
    if (block_size < 6u || block_size > available) return 0;
    if (entry_count > (block_size - 6u) / 8u) return 0;
    used = 6u + entry_count * 8u;
    if (fp_count > (block_size - used) / 2u) return 0;
    used += fp_count * 2u;
    strings = (const char *)levelb + used;
    strings_size = block_size - used;
    for (uint32_t i = 0; i < entry_count; i++) {
        const unsigned char *entry = levelb + 6u + i * 8u;
        if (!cgb_compact_cstr_valid(strings, strings_size, cgb_be32(entry)))
            return 0;
    }
    return 1;
}

static const unsigned char *cgb_compact_levelc(const CgbFacade *facade)
{
    const unsigned char *base = cgb_compact_base(facade);
    uint32_t total = cgb_facade_size(facade);
    uint32_t off = CGB_COMPACT_UCODE_OFFSET + cgb_compact_ucode_size(facade);
    uint8_t content = cgb_compact_content(facade);

    if ((content & CGB_COMPACT_CONSTANT_TABLE) != 0u) {
        if (off + 2u > total) return NULL;
        off += cgb_be16(base + off);
    }
    if ((content & CGB_COMPACT_LOOKUP_TABLE) != 0u) {
        if (off + 2u > total) return NULL;
        off += cgb_be16(base + off);
    }
    if ((content & CGB_COMPACT_PARAMETER_INFO) == 0u) return NULL;
    if (off + 4u > total) return NULL;
    return base + off;
}

static int cgb_read_compact(const unsigned char *base, uint32_t size,
                            CellCgbProgram *program)
{
    CgbFacade facade;
    uint32_t ucode_size;
    uint32_t off;
    const unsigned char *levelb = NULL;
    uint8_t profile;
    uint8_t content;

    if (size < CGB_COMPACT_UCODE_OFFSET) return CELL_CGB_ERROR_FAILED;
    if (cgb_be32(base) != CGB_COMPACT_FOURCC) return CELL_CGB_ERROR_FAILED;
    profile = base[0x0a];
    content = base[0x0b];
    if (profile != CGB_COMPACT_PROFILE_VERTEX &&
        profile != CGB_COMPACT_PROFILE_FRAGMENT)
        return CELL_CGB_ERROR_FAILED;
    if ((content & ~(CGB_COMPACT_CONSTANT_TABLE |
                     CGB_COMPACT_LOOKUP_TABLE |
                     CGB_COMPACT_PARAMETER_INFO)) != 0u)
        return CELL_CGB_ERROR_FAILED;

    ucode_size = cgb_compact_ucode_size_base(base);
    if (ucode_size > size - CGB_COMPACT_UCODE_OFFSET)
        return CELL_CGB_ERROR_FAILED;
    if (profile == CGB_COMPACT_PROFILE_VERTEX && (ucode_size & 15u) != 0u)
        return CELL_CGB_ERROR_FAILED;

    off = CGB_COMPACT_UCODE_OFFSET + ucode_size;
    if ((content & CGB_COMPACT_CONSTANT_TABLE) != 0u) {
        uint32_t block_size;
        if (off + 4u > size) return CELL_CGB_ERROR_FAILED;
        block_size = cgb_be16(base + off);
        if (block_size < 4u || block_size > size - off)
            return CELL_CGB_ERROR_FAILED;
        off += block_size;
    }
    if ((content & CGB_COMPACT_LOOKUP_TABLE) != 0u) {
        uint32_t block_size;
        levelb = base + off;
        if (!cgb_compact_validate_levelb(levelb, size - off))
            return CELL_CGB_ERROR_FAILED;
        block_size = cgb_be16(levelb);
        off += block_size;
    }
    if ((content & CGB_COMPACT_PARAMETER_INFO) != 0u) {
        uint32_t block_size;
        uint32_t info_count;
        if (off + 4u > size) return CELL_CGB_ERROR_FAILED;
        block_size = cgb_be16(base + off);
        info_count = cgb_be16(base + off + 2);
        if (block_size < 4u || block_size > size - off)
            return CELL_CGB_ERROR_FAILED;
        if (info_count > (block_size - 4u) / 8u)
            return CELL_CGB_ERROR_FAILED;
        off += block_size;
    }

    if (off > CGB_FACADE_SIZE_MASK) return CELL_CGB_ERROR_FAILED;
    facade.magic = CGB_FACADE_MAGIC;
    facade.size = off | CGB_FACADE_COMPACT;
    facade.binary = (const CgBinaryProgram *)base;
    facade.params = (const CgBinaryParameter *)levelb;
    facade.program_header = base + CGB_COMPACT_HEADER_SIZE;
    facade.ucode = base + CGB_COMPACT_UCODE_OFFSET;
    cgb_store(program, &facade);
    return CELL_CGB_OK;
}

uint32_t cellCgbGetSize(const void *binary)
{
    const CgBinaryProgram *program = (const CgBinaryProgram *)binary;
    const unsigned char *base = (const unsigned char *)binary;
    uint32_t off;
    uint8_t content;
    if (!binary) return 0;
    if (cgb_be32(base) == CGB_COMPACT_FOURCC) {
        if (base[0x0a] != CGB_COMPACT_PROFILE_VERTEX &&
            base[0x0a] != CGB_COMPACT_PROFILE_FRAGMENT)
            return 0;
        content = base[0x0b];
        if ((content & ~(CGB_COMPACT_CONSTANT_TABLE |
                         CGB_COMPACT_LOOKUP_TABLE |
                         CGB_COMPACT_PARAMETER_INFO)) != 0u)
            return 0;
        off = CGB_COMPACT_UCODE_OFFSET + cgb_compact_ucode_size_base(base);
        if ((content & CGB_COMPACT_CONSTANT_TABLE) != 0u)
            off += cgb_be16(base + off);
        if ((content & CGB_COMPACT_LOOKUP_TABLE) != 0u)
            off += cgb_be16(base + off);
        if ((content & CGB_COMPACT_PARAMETER_INFO) != 0u)
            off += cgb_be16(base + off);
        return off;
    }
    if (program->revision != CG_BINARY_FORMAT_REVISION) return 0;
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
    if (!binary || !program)
        return CELL_CGB_ERROR_FAILED;
    if (size >= CGB_COMPACT_UCODE_OFFSET &&
        cgb_be32(binary) == CGB_COMPACT_FOURCC)
        return cgb_read_compact((const unsigned char *)binary, size, program);
    /* TODO: Once rsx-cg-compiler -mcgb emits byte-exact compact CGB
       containers, retire this raw CgBinary transition path. */
    if (size < (uint32_t)offsetof(CgBinaryProgram, data))
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
    if (cgb_is_compact_facade(&facade)) {
        uint8_t profile = cgb_compact_profile(&facade);
        if (profile == CGB_COMPACT_PROFILE_VERTEX) return CELL_CGB_PROFILE_VERTEX;
        if (profile == CGB_COMPACT_PROFILE_FRAGMENT) return CELL_CGB_PROFILE_FRAGMENT;
        return CELL_CGB_PROFILE_UNKNOWN;
    }
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
    if (!cgb_load(program, &facade)) return 0u;
    if (cgb_is_compact_facade(&facade)) return cgb_compact_ucode_size(&facade);
    return facade.binary->ucodeSize;
}

int32_t cellCgbGetVertexConfiguration(
    const CellCgbProgram *program,
    CellCgbVertexProgramConfiguration *conf)
{
    CgbFacade facade;
    const CgBinaryVertexProgram *vp;
    if (conf) cgb_zero(conf, sizeof(*conf));
    if (!conf || !cgb_load(program, &facade))
        return CELL_CGB_ERROR_FAILED;
    if (cgb_is_compact_facade(&facade)) {
        const unsigned char *vpconf = (const unsigned char *)facade.program_header;
        if (cgb_compact_profile(&facade) != CGB_COMPACT_PROFILE_VERTEX)
            return CELL_CGB_ERROR_FAILED;
        conf->instructionSlot = cgb_be16(vpconf + 12);
        conf->instructionCount = (uint16_t)(cgb_compact_ucode_size(&facade) / 16u);
        conf->attributeInputMask = cgb_be16(vpconf);
        conf->registerCount = vpconf[2];
        return CELL_CGB_OK;
    }
    if (!cgb_is_vertex_profile(facade.binary->profile))
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
    if (!conf || !cgb_load(program, &facade))
        return CELL_CGB_ERROR_FAILED;
    if (cgb_is_compact_facade(&facade)) {
        const unsigned char *fpconf = (const unsigned char *)facade.program_header;
        if (cgb_compact_profile(&facade) != CGB_COMPACT_PROFILE_FRAGMENT)
            return CELL_CGB_ERROR_FAILED;
        conf->offset = 0u;
        conf->attributeInputMask = cgb_be32(fpconf);
        conf->texCoordsInputMask = cgb_be16(fpconf + 4);
        conf->texCoords2D = cgb_be16(fpconf + 6);
        conf->texCoordsCentroid = cgb_be16(fpconf + 8);
        conf->fragmentControl = cgb_be32(fpconf + 12);
        conf->registerCount = fpconf[16];
        return CELL_CGB_OK;
    }
    if (!cgb_is_fragment_profile(facade.binary->profile))
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
    if (!attributeOutputMask || !cgb_load(program, &facade))
        return CELL_CGB_ERROR_FAILED;
    if (cgb_is_compact_facade(&facade)) {
        const unsigned char *vpconf = (const unsigned char *)facade.program_header;
        if (cgb_compact_profile(&facade) != CGB_COMPACT_PROFILE_VERTEX)
            return CELL_CGB_ERROR_FAILED;
        *attributeOutputMask = cgb_be32(vpconf + 4);
        return CELL_CGB_OK;
    }
    if (!cgb_is_vertex_profile(facade.binary->profile))
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
    if (!userClipPlaneControlMask || !cgb_load(program, &facade))
        return CELL_CGB_ERROR_FAILED;
    if (cgb_is_compact_facade(&facade)) {
        const unsigned char *vpconf = (const unsigned char *)facade.program_header;
        if (cgb_compact_profile(&facade) != CGB_COMPACT_PROFILE_VERTEX)
            return CELL_CGB_ERROR_FAILED;
        *userClipPlaneControlMask = cgb_be32(vpconf + 8);
        return CELL_CGB_OK;
    }
    if (!cgb_is_vertex_profile(facade.binary->profile))
        return CELL_CGB_ERROR_FAILED;
    vp = (const CgBinaryVertexProgram *)facade.program_header;
    *userClipPlaneControlMask = vp->userClipMask;
    return CELL_CGB_OK;
}

uint32_t cellCgbGetVertexConstantCount(const CellCgbProgram *program)
{
    CgbFacade facade;
    uint32_t count = 0;
    if (!cgb_load(program, &facade))
        return 0;
    if (cgb_is_compact_facade(&facade))
        /* TODO: Parse compact Level A vertex constant tables for
           reference CGBs that carry internal/default VP constants. */
        return 0;
    if (!cgb_is_vertex_profile(facade.binary->profile))
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
    if (!cgb_load(program, &facade))
        return;
    if (cgb_is_compact_facade(&facade))
        /* TODO: Parse compact Level A vertex constant tables for
           reference CGBs that carry internal/default VP constants. */
        return;
    if (!cgb_is_vertex_profile(facade.binary->profile))
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
    if (cgb_is_compact_facade(&facade)) {
        uint32_t count = cgb_compact_levelb_count(&facade);
        for (uint32_t i = 0; i < count; i++) {
            if (cgb_streq(cgb_compact_entry_name(&facade, i), name))
                return i;
        }
        return CELL_CGB_ERROR_FAILED;
    }
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
    if (cgb_is_compact_facade(&facade))
        return cgb_compact_entry_resource(&facade, map_index);
    param = cgb_param(&facade, map_index);
    if (!param) return 0;

    /* Map CG resource to hardware slot/index.  For vertex
     * attributes, the slot is (res - CG_ATTR0); for texture
     * units, (res - CG_TEXUNIT0); for uniform constants,
     * the register is param->resIndex. */
    if (param->res >= CG_ATTR0 && param->res <= CG_ATTR15)
        return (uint16_t)(param->res - CG_ATTR0);
    if (param->res >= CG_TEXUNIT0 && param->res <= CG_TEXUNIT15)
        return (uint16_t)(param->res - CG_TEXUNIT0);
    return (uint16_t)param->resIndex;
}

uint32_t cellCgbMapGetLength(const CellCgbProgram *program)
{
    CgbFacade facade;
    if (!cgb_load(program, &facade)) return 0u;
    if (cgb_is_compact_facade(&facade)) return cgb_compact_levelb_count(&facade);
    return facade.binary->parameterCount;
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
    if (cgb_is_compact_facade(&facade)) {
        param = NULL;
        src = cgb_compact_entry_name(&facade, map_index);
    } else {
        param = cgb_param(&facade, map_index);
        src = cgb_param_name(&facade, param);
    }
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
    if (cgb_is_compact_facade(&facade)) {
        if (map_index < cgb_compact_levelb_count(&facade) && reg)
            *reg = cgb_compact_entry_resource(&facade, map_index);
        return;
    }
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
    if (cgb_is_compact_facade(&facade)) {
        uint16_t resource = cgb_compact_entry_resource(&facade, map_index);
        uint32_t index;
        uint32_t fp_count;
        const unsigned char *fp;
        uint32_t embedded_count;
        if (resource < CGB_COMPACT_FP_RESOURCE_START) return;
        index = (uint32_t)(resource - CGB_COMPACT_FP_RESOURCE_START);
        fp_count = cgb_compact_levelb_fp_count(&facade);
        fp = cgb_compact_fp_resources(&facade);
        if (!fp || index + 2u > fp_count) return;
        embedded_count = cgb_be16(fp + (index + 1u) * 2u);
        if (index + 2u + embedded_count > fp_count) return;
        if (count) *count = embedded_count;
        if (offsets) {
            for (uint32_t i = 0; i < embedded_count; i++)
                offsets[i] = cgb_be16(fp + (index + 2u + i) * 2u);
        }
        return;
    }
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
    if (cgb_is_compact_facade(&facade)) {
        uint16_t resource = cgb_compact_entry_resource(&facade, map_index);
        const unsigned char *fp;
        uint32_t index;
        if (!reg) return;
        if (resource < CGB_COMPACT_FP_RESOURCE_START) {
            *reg = resource;
            return;
        }
        index = (uint32_t)(resource - CGB_COMPACT_FP_RESOURCE_START);
        fp = cgb_compact_fp_resources(&facade);
        if (fp && index < cgb_compact_levelb_fp_count(&facade))
            *reg = cgb_be16(fp + index * 2u);
        return;
    }
    param = cgb_param(&facade, map_index);
    if (param && reg) *reg = (uint16_t)param->resIndex;
}

static const CgBinaryParameter *cgb_levelc_param(CellCgbProgram *program,
                                                 uint32_t map_index)
{
    CgbFacade facade;
    if (!cgb_load(program, &facade)) return NULL;
    return cgb_param(&facade, map_index);
}

static uint16_t cgb_levelc_value_count(CGtype type)
{
    switch (type) {
#define CG_DATATYPE_MACRO(name, compiler_name, enum_name, base_name, nrows, ncols, pc_name) \
    case enum_name: return (uint16_t)(((nrows) ? (nrows) : 1) * ((ncols) ? (ncols) : 1));
#include <Cg/cg_datatypes.h>
#undef CG_DATATYPE_MACRO
    default:
        return 0;
    }
}

uint16_t cellCgbLevelCMapGetCgType(CellCgbProgram *program,
                                   const uint32_t map_index)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    if (cgb_load(program, &facade) && cgb_is_compact_facade(&facade)) {
        const unsigned char *levelc = cgb_compact_levelc(&facade);
        if (!levelc || map_index >= cgb_be16(levelc + 2)) return 0;
        return cgb_be16(levelc + 4u + map_index * 8u);
    }
    param = cgb_levelc_param(program, map_index);
    return param ? (uint16_t)param->type : 0;
}

uint16_t cellCgbLevelCMapGetCgResource(CellCgbProgram *program,
                                       const uint32_t map_index)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    if (cgb_load(program, &facade) && cgb_is_compact_facade(&facade)) {
        const unsigned char *levelc = cgb_compact_levelc(&facade);
        if (!levelc || map_index >= cgb_be16(levelc + 2)) return 0;
        return cgb_be16(levelc + 4u + map_index * 8u + 2u);
    }
    param = cgb_levelc_param(program, map_index);
    return param ? (uint16_t)param->res : 0;
}

uint16_t cellCgbLevelCMapGetVariability(CellCgbProgram *program,
                                        const uint32_t map_index)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    if (cgb_load(program, &facade) && cgb_is_compact_facade(&facade)) {
        const unsigned char *levelc = cgb_compact_levelc(&facade);
        uint16_t flags;
        uint16_t variability;
        if (!levelc || map_index >= cgb_be16(levelc + 2)) return 0;
        flags = cgb_be16(levelc + 4u + map_index * 8u + 4u);
        variability = flags & CGB_COMPACT_CGPV_MASK;
        if (variability == CGB_COMPACT_CGPV_VARYING) return (uint16_t)CG_VARYING;
        if (variability == CGB_COMPACT_CGPV_UNIFORM) return (uint16_t)CG_UNIFORM;
        if (variability == CGB_COMPACT_CGPV_CONSTANT) return (uint16_t)CG_CONSTANT;
        if (variability == CGB_COMPACT_CGPV_MIXED) return (uint16_t)CG_MIXED;
        return 0;
    }
    param = cgb_levelc_param(program, map_index);
    return param ? (uint16_t)param->var : 0;
}

uint16_t cellCgbLevelCMapGetDirection(CellCgbProgram *program,
                                      const uint32_t map_index)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    if (cgb_load(program, &facade) && cgb_is_compact_facade(&facade)) {
        const unsigned char *levelc = cgb_compact_levelc(&facade);
        uint16_t flags;
        uint16_t direction;
        if (!levelc || map_index >= cgb_be16(levelc + 2)) return 0;
        flags = cgb_be16(levelc + 4u + map_index * 8u + 4u);
        direction = flags & CGB_COMPACT_CGPD_MASK;
        if (direction == CGB_COMPACT_CGPD_IN) return (uint16_t)CG_IN;
        if (direction == CGB_COMPACT_CGPD_OUT) return (uint16_t)CG_OUT;
        if (direction == CGB_COMPACT_CGPD_INOUT) return (uint16_t)CG_INOUT;
        return 0;
    }
    param = cgb_levelc_param(program, map_index);
    return param ? (uint16_t)param->direction : 0;
}

uint16_t cellCgbLevelCMapGetValueCount(CellCgbProgram *program,
                                       const uint32_t map_index)
{
    CgbFacade facade;
    const CgBinaryParameter *param;
    if (cgb_load(program, &facade) && cgb_is_compact_facade(&facade)) {
        const unsigned char *levelc = cgb_compact_levelc(&facade);
        if (!levelc || map_index >= cgb_be16(levelc + 2)) return 0;
        return cgb_be16(levelc + 4u + map_index * 8u + 6u);
    }
    param = cgb_levelc_param(program, map_index);
    return param ? cgb_levelc_value_count(param->type) : 0;
}
